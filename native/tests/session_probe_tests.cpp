#include "tdvr_session_probe.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
template<class T> T handle(uintptr_t n) { return reinterpret_cast<T>(n); }
struct Fake {
    bool fail_create = false, unsupported_format = false, empty_formats = false;
    bool fail_right = false, null_image = false, empty_images = false, duplicate = false;
    bool cleanup_error = false, grow = false, grow_forever = false, bad_count = false;
    bool null_session = false, null_swapchain = false;
    unsigned swapchain_calls = 0, fills = 0;
    std::vector<std::string> events;
} fake;
XrResult XRAPI_PTR create_session(XrInstance instance, const XrSessionCreateInfo* info, XrSession* out) {
    require(instance == handle<XrInstance>(1) && info->type == XR_TYPE_SESSION_CREATE_INFO
        && info->systemId == 7, "invalid session input");
    const auto* binding = static_cast<const XrGraphicsBindingD3D12KHR*>(info->next);
    require(binding && binding->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR
        && binding->device == handle<ID3D12Device*>(2) && binding->queue == handle<ID3D12CommandQueue*>(3),
        "missing required D3D12 binding");
    fake.events.push_back("create session");
    if (fake.fail_create) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    *out = fake.null_session ? XR_NULL_HANDLE : handle<XrSession>(8);
    return XR_SUCCESS;
}
XrResult XRAPI_PTR destroy_session(XrSession session) {
    require(session == handle<XrSession>(8), "wrong session destroyed");
    fake.events.push_back("destroy session");
    return fake.cleanup_error ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
}
XrResult XRAPI_PTR formats(XrSession session, uint32_t capacity, uint32_t* count, int64_t* out) {
    require(session == handle<XrSession>(8), "wrong format session");
    *count = fake.empty_formats ? 0 : 2;
    if (capacity) {
        out[0] = fake.unsupported_format ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        out[1] = fake.unsupported_format ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_PTR create_swapchain(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain* out) {
    require(session == handle<XrSession>(8) && info->type == XR_TYPE_SWAPCHAIN_CREATE_INFO
        && info->format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && info->width == 64 && info->height == 64
        && info->sampleCount == 1 && info->arraySize == 1 && info->faceCount == 1 && info->mipCount == 1
        && info->usageFlags == (XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT),
        "invalid eye swapchain description/format preference");
    const auto eye = fake.swapchain_calls++;
    fake.events.push_back(eye ? "create right" : "create left");
    if (eye == 1 && fake.fail_right) return XR_ERROR_OUT_OF_MEMORY;
    *out = fake.null_swapchain ? XR_NULL_HANDLE : handle<XrSwapchain>(10 + (fake.duplicate ? 0 : eye));
    return XR_SUCCESS;
}
XrResult XRAPI_PTR destroy_swapchain(XrSwapchain swapchain) {
    require(swapchain == handle<XrSwapchain>(10) || swapchain == handle<XrSwapchain>(11), "wrong swapchain destroyed");
    fake.events.push_back(swapchain == handle<XrSwapchain>(10) ? "destroy left" : "destroy right");
    return fake.cleanup_error ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
}
XrResult XRAPI_PTR images(XrSwapchain swapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* out) {
    require(swapchain == handle<XrSwapchain>(10) || swapchain == handle<XrSwapchain>(11), "wrong image swapchain");
    *count = fake.empty_images ? 0 : fake.bad_count ? 5000 : 3;
    if (capacity) {
        ++fake.fills;
        if (fake.grow_forever || (fake.grow && fake.fills == 1)) return XR_ERROR_SIZE_INSUFFICIENT;
        auto* views = reinterpret_cast<XrSwapchainImageD3D12KHR*>(out);
        for (uint32_t i = 0; i != 3; ++i) {
            require(views[i].type == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR && !views[i].next, "image type/next not initialized");
            views[i].texture = fake.null_image ? nullptr : handle<ID3D12Resource*>(20 + i);
        }
    }
    return XR_SUCCESS;
}
const tdvr::SessionProbeDispatch api{create_session, destroy_session, formats, create_swapchain, destroy_swapchain, images};
tdvr::SessionProbeReport run() {
    return tdvr::probe_session(api, handle<XrInstance>(1), 7, handle<ID3D12Device*>(2), handle<ID3D12CommandQueue*>(3));
}
void released(const tdvr::SessionProbeReport& result, unsigned count) {
    require(result.destroyed_swapchains == count && result.destroyed_session, "children/session not cleaned");
    require(fake.events.back() == "destroy session", "session must outlive its swapchains");
}
}
int main() {
    auto result = run();
    require(result.result == XR_SUCCESS && result.image_count[0] == 3 && result.image_count[1] == 3, "normal allocation failed");
    released(result, 2);
    require(fake.events == std::vector<std::string>{"create session", "create left", "create right", "destroy right", "destroy left", "destroy session"},
        "wrong creation/destruction order");
    fake = {}; fake.fail_create = true;
    result = run();
    require(result.result == XR_ERROR_GRAPHICS_DEVICE_INVALID && !result.destroyed_session
        && result.destroyed_swapchains == 0, "failed creation mishandled");
    fake = {}; fake.null_session = true;
    result = run();
    require(result.result != XR_SUCCESS && !result.destroyed_session, "null session accepted");
    fake = {}; fake.unsupported_format = true;
    result = run();
    require(result.result == XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED, "unsupported format guessed");
    released(result, 0);
    fake = {}; fake.empty_formats = true;
    result = run();
    require(result.result != XR_SUCCESS, "empty formats accepted"); released(result, 0);
    fake = {}; fake.fail_right = true;
    result = run();
    require(result.result == XR_ERROR_OUT_OF_MEMORY, "right creation error lost"); released(result, 1);
    fake = {}; fake.null_swapchain = true;
    result = run();
    require(result.result != XR_SUCCESS, "null swapchain accepted"); released(result, 0);
    fake = {}; fake.null_image = true;
    result = run();
    require(result.result != XR_SUCCESS, "null image accepted"); released(result, 1);
    fake = {}; fake.empty_images = true;
    result = run();
    require(result.result != XR_SUCCESS, "empty images accepted"); released(result, 1);
    fake = {}; fake.bad_count = true;
    result = run();
    require(result.result != XR_SUCCESS, "excessive count accepted"); released(result, 1);
    fake = {}; fake.duplicate = true;
    result = run();
    require(result.result != XR_SUCCESS, "same swapchain used for both eyes"); released(result, 1);
    fake = {}; fake.cleanup_error = true;
    result = run();
    require(result.result != XR_SUCCESS && result.stage.find("cleanup failed") != std::string::npos,
        "destroy failure reported as success"); released(result, 2);
    fake = {}; fake.grow = true;
    result = run();
    require(result.result == XR_SUCCESS && fake.fills == 3, "enumeration growth not handled"); released(result, 2);
    fake = {}; fake.grow_forever = true;
    result = run();
    require(result.result == XR_ERROR_SIZE_INSUFFICIENT && fake.fills == 3, "enumeration unbounded"); released(result, 1);
    fake = {};
    auto invalid = api; invalid.destroy_session = nullptr;
    result = tdvr::probe_session(invalid, handle<XrInstance>(1), 7, handle<ID3D12Device*>(2), handle<ID3D12CommandQueue*>(3));
    require(result.result != XR_SUCCESS && fake.events.empty(), "incomplete dispatch reached runtime");
    std::cout << "Session binding, two swapchains, enumeration and reverse cleanup verified with fake API; no headset required.\n";
}
