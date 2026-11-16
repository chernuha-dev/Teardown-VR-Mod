#include "tdvr_session_probe.h"
#include <algorithm>
#include <array>
#include <vector>

namespace tdvr {
namespace {
struct Resources {
    const SessionProbeDispatch& api;
    XrSession session = XR_NULL_HANDLE;
    std::array<XrSwapchain, 2> eyes{};
    // Clear handles before calling into the runtime: cleanup must not double-destroy.
    XrResult cleanup(SessionProbeReport* report = nullptr) {
        XrResult failure = XR_SUCCESS;
        for (auto it = eyes.rbegin(); it != eyes.rend(); ++it) {
            if (*it == XR_NULL_HANDLE) continue;
            const auto handle = *it;
            *it = XR_NULL_HANDLE;
            const XrResult result = api.destroy_swapchain(handle);
            if (report) ++report->destroyed_swapchains;
            if (result != XR_SUCCESS && failure == XR_SUCCESS) failure = result;
        }
        if (session != XR_NULL_HANDLE) {
            const auto handle = session;
            session = XR_NULL_HANDLE;
            const XrResult result = api.destroy_session(handle);
            if (report) report->destroyed_session = true;
            if (result != XR_SUCCESS && failure == XR_SUCCESS) failure = result;
        }
        return failure;
    }
    ~Resources() { cleanup(); }
};
template<class T, class Enumerate>
XrResult enumerate(std::vector<T>& out, T initial, Enumerate call) {
    for (int attempt = 0; attempt != 3; ++attempt) {
        uint32_t count = 0;
        XrResult result = call(0, &count, nullptr);
        if (result != XR_SUCCESS) return result;
        if (!count || count > 4096) return XR_ERROR_RUNTIME_FAILURE;
        out.assign(count, initial);
        uint32_t written = 0;
        result = call(count, &written, out.data());
        if (result == XR_ERROR_SIZE_INSUFFICIENT) continue;
        if (result != XR_SUCCESS) return result;
        if (!written || written > count) return XR_ERROR_RUNTIME_FAILURE;
        out.resize(written);
        return XR_SUCCESS;
    }
    return XR_ERROR_SIZE_INSUFFICIENT;
}
}
SessionProbeReport probe_session(const SessionProbeDispatch& api, XrInstance instance,
    XrSystemId system, ID3D12Device* device, ID3D12CommandQueue* queue) {
    SessionProbeReport report;
    Resources resources{api};
    auto finish = [&](XrResult result, const char* stage) {
        report.result = result;
        report.stage = stage;
        const XrResult cleanup = resources.cleanup(&report);
        if (cleanup != XR_SUCCESS) {
            report.stage += "; cleanup failed";
            // Keep an earlier operation failure while making cleanup failure explicit.
            if (result == XR_SUCCESS) report.result = cleanup;
        }
        return report;
    };
    if (!instance || !system || !device || !queue || !api.create_session
        || !api.destroy_session || !api.formats || !api.create_swapchain
        || !api.destroy_swapchain || !api.images)
        return finish(XR_ERROR_VALIDATION_FAILURE, "invalid arguments/dispatch");
    XrGraphicsBindingD3D12KHR binding{};
    binding.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
    binding.device = device;
    binding.queue = queue;
    XrSessionCreateInfo info{};
    info.type = XR_TYPE_SESSION_CREATE_INFO;
    info.next = &binding;
    info.systemId = system;
    XrResult result = api.create_session(instance, &info, &resources.session);
    if (result != XR_SUCCESS) return finish(result, "xrCreateSession");
    if (!resources.session) return finish(XR_ERROR_RUNTIME_FAILURE, "null session on success");
    std::vector<int64_t> formats;
    result = enumerate(formats, int64_t{}, [&](uint32_t n, uint32_t* count, int64_t* values) {
        return api.formats(resources.session, n, count, values);
    });
    if (result != XR_SUCCESS) return finish(result, "xrEnumerateSwapchainFormats");
    for (const auto candidate : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM}) {
        if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(candidate)) != formats.end()) {
            report.format = candidate;
            break;
        }
    }
    if (!report.format) return finish(XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED, "no supported RGBA8 format");
    for (size_t eye = 0; eye != 2; ++eye) {
        XrSwapchainCreateInfo swapchain{};
        swapchain.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        swapchain.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        swapchain.format = report.format;
        swapchain.sampleCount = 1;
        swapchain.width = swapchain.height = 64;
        swapchain.faceCount = swapchain.arraySize = swapchain.mipCount = 1;
        result = api.create_swapchain(resources.session, &swapchain, &resources.eyes[eye]);
        if (result != XR_SUCCESS) return finish(result, eye == 0 ? "create left swapchain" : "create right swapchain");
        if (!resources.eyes[eye]) return finish(XR_ERROR_RUNTIME_FAILURE, "null swapchain on success");
        if (eye && resources.eyes[1] == resources.eyes[0]) {
            // A broken runtime returning an owned handle twice must not cause double-destroy.
            resources.eyes[1] = XR_NULL_HANDLE;
            return finish(XR_ERROR_RUNTIME_FAILURE, "duplicate eye swapchain handle");
        }
        XrSwapchainImageD3D12KHR initial{};
        initial.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        std::vector<XrSwapchainImageD3D12KHR> images;
        result = enumerate(images, initial, [&](uint32_t n, uint32_t* count, XrSwapchainImageD3D12KHR* values) {
            return api.images(resources.eyes[eye], n, count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(values));
        });
        if (result != XR_SUCCESS) return finish(result, "xrEnumerateSwapchainImages");
        for (const auto& image : images)
            if (!image.texture) return finish(XR_ERROR_RUNTIME_FAILURE, "null D3D12 swapchain image");
        report.image_count[eye] = static_cast<uint32_t>(images.size());
    }
    return finish(XR_SUCCESS, "session and two swapchains allocated and destroyed; no frames submitted");
}
}
