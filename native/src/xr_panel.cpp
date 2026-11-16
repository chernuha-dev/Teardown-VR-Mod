// SPDX-License-Identifier: MIT
#include "tdvr_xr_panel.h"
#include "tdvr_eye_copy.h"
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>
#include <stdexcept>

namespace tdvr {
using Microsoft::WRL::ComPtr;
namespace {
struct XrFailure { XrResult result; };
struct GpuFailure { HRESULT result; };
void xrcheck(XrResult r) { if (r != XR_SUCCESS) throw XrFailure{r}; }
void gpucheck(HRESULT r) { if (FAILED(r)) throw GpuFailure{r}; }
template<class T, class F> std::vector<T> enumerate(T initial, F call) {
    for (unsigned retry = 0; retry < 3; ++retry) {
        uint32_t count = 0;
        xrcheck(call(0, &count, nullptr));
        if (!count || count > 4096) throw XrFailure{XR_ERROR_LIMIT_REACHED};
        std::vector<T> values(count, initial);
        uint32_t written = 0;
        const auto result = call(count, &written, values.data());
        if (result == XR_ERROR_SIZE_INSUFFICIENT) continue;
        xrcheck(result);
        if (!written || written > count) throw XrFailure{XR_ERROR_RUNTIME_FAILURE};
        values.resize(written);
        return values;
    }
    throw XrFailure{XR_ERROR_SIZE_INSUFFICIENT};
}
bool same_device(ID3D12Device* a, ID3D12Device* b) {
    ComPtr<IUnknown> x, y;
    gpucheck(a->QueryInterface(IID_PPV_ARGS(&x)));
    gpucheck(b->QueryInterface(IID_PPV_ARGS(&y)));
    return x.Get() == y.Get();
}
}
struct XrPanel::Impl {
    PanelApi api;
    PanelStats report;
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    // Held only while GPU work might reference it. Never release an unfinished
    // game's backbuffer or runtime image, even after a failed Signal/wait.
    ComPtr<ID3D12Resource> pending_source;
    HANDLE event = nullptr;
    D3D12_RESOURCE_DESC source_desc{};
    uint64_t fence_value = 0;
    bool running = false, frame = false, terminal = false, gpu_pending = false;
    XrTime display_time = 0;
    explicit Impl(const PanelApi& value) : api(value) {}
    bool valid_api() const {
        return api.create_instance && api.destroy_instance && api.get_system && api.get_proc
            && api.create_session && api.destroy_session && api.formats && api.create_swapchain
            && api.destroy_swapchain && api.images && api.create_space && api.destroy_space
            && api.poll && api.begin_session && api.end_session && api.wait_frame
            && api.begin_frame && api.end_frame && api.acquire && api.wait_image && api.release;
    }
    void cleanup() noexcept {
        if (gpu_pending) { report.quarantined = true; return; }
        auto cleaned = [&](XrResult result) {
            if (result != XR_SUCCESS) {
                if (report.xr == XR_SUCCESS) report.xr = result;
                report.stage = 7;
            }
        };
        if (frame && session) {
            XrFrameEndInfo end{}; end.type = XR_TYPE_FRAME_END_INFO;
            end.displayTime = display_time; end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            cleaned(api.end_frame(session, &end)); frame = false;
        }
        if (swapchain) { cleaned(api.destroy_swapchain(swapchain)); swapchain = XR_NULL_HANDLE; }
        images.clear();
        if (space) { cleaned(api.destroy_space(space)); space = XR_NULL_HANDLE; }
        if (session) { cleaned(api.destroy_session(session)); session = XR_NULL_HANDLE; }
        if (instance) { cleaned(api.destroy_instance(instance)); instance = XR_NULL_HANDLE; }
        if (event) { CloseHandle(event); event = nullptr; }
        pending_source.Reset(); list.Reset(); allocator.Reset(); fence.Reset(); queue.Reset(); device.Reset();
    }
    void initialize(ID3D12CommandQueue* q, ID3D12Resource* source) {
        report.stage = 1;
        if (!valid_api()) throw XrFailure{XR_ERROR_VALIDATION_FAILURE};
        if (!q || !source || q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) throw GpuFailure{E_INVALIDARG};
        gpucheck(q->GetDevice(IID_PPV_ARGS(&device)));
        ComPtr<ID3D12Device> source_device;
        gpucheck(source->GetDevice(IID_PPV_ARGS(&source_device)));
        if (!same_device(device.Get(), source_device.Get())) throw GpuFailure{E_INVALIDARG};
        queue = q;
        source_desc = source->GetDesc();
        if (source_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !source_desc.Width || !source_desc.Height
            || source_desc.Width > 16384 || source_desc.Height > 16384 || source_desc.DepthOrArraySize != 1
            || source_desc.MipLevels != 1 || source_desc.SampleDesc.Count != 1
            || (source_desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
                && source_desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
            throw GpuFailure{E_INVALIDARG};
        const char* extension = XR_KHR_D3D12_ENABLE_EXTENSION_NAME;
        XrInstanceCreateInfo info{}; info.type = XR_TYPE_INSTANCE_CREATE_INFO;
        constexpr char name[] = "TeardownVR flat panel diagnostic";
        std::memcpy(info.applicationInfo.applicationName, name, sizeof(name));
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        info.enabledExtensionCount = 1; info.enabledExtensionNames = &extension;
        xrcheck(api.create_instance(&info, &instance));
        if (!instance) throw XrFailure{XR_ERROR_RUNTIME_FAILURE};
        XrSystemGetInfo system_info{}; system_info.type = XR_TYPE_SYSTEM_GET_INFO; system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system = XR_NULL_SYSTEM_ID;
        xrcheck(api.get_system(instance, &system_info, &system));
        PFN_xrGetD3D12GraphicsRequirementsKHR requirements = nullptr;
        xrcheck(api.get_proc(instance, "xrGetD3D12GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&requirements)));
        if (!requirements) throw XrFailure{XR_ERROR_FUNCTION_UNSUPPORTED};
        XrGraphicsRequirementsD3D12KHR required{}; required.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;
        xrcheck(requirements(instance, system, &required));
        const auto luid = device->GetAdapterLuid();
        if (luid.HighPart != required.adapterLuid.HighPart || luid.LowPart != required.adapterLuid.LowPart)
            throw XrFailure{XR_ERROR_GRAPHICS_DEVICE_INVALID};
        D3D12_FEATURE_DATA_FEATURE_LEVELS feature{}; feature.NumFeatureLevels = 1; feature.pFeatureLevelsRequested = &required.minFeatureLevel;
        gpucheck(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &feature, sizeof(feature)));
        if (feature.MaxSupportedFeatureLevel < required.minFeatureLevel) throw XrFailure{XR_ERROR_GRAPHICS_DEVICE_INVALID};
        XrGraphicsBindingD3D12KHR binding{}; binding.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR; binding.device = device.Get(); binding.queue = q;
        XrSessionCreateInfo session_info{}; session_info.type = XR_TYPE_SESSION_CREATE_INFO; session_info.next = &binding; session_info.systemId = system;
        xrcheck(api.create_session(instance, &session_info, &session));
        if (!session) throw XrFailure{XR_ERROR_RUNTIME_FAILURE};
        const auto formats = enumerate(int64_t{}, [&](uint32_t n, uint32_t* count, int64_t* values) { return api.formats(session, n, count, values); });
        if (std::find(formats.begin(), formats.end(), source_desc.Format) == formats.end()) throw XrFailure{XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED};
        XrReferenceSpaceCreateInfo reference{}; reference.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
        reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; reference.poseInReferenceSpace.orientation.w = 1;
        xrcheck(api.create_space(session, &reference, &space));
        if (!space) throw XrFailure{XR_ERROR_RUNTIME_FAILURE};
        XrSwapchainCreateInfo swap{}; swap.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        swap.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        swap.format = source_desc.Format; swap.width = static_cast<uint32_t>(source_desc.Width); swap.height = source_desc.Height;
        swap.sampleCount = swap.faceCount = swap.arraySize = swap.mipCount = 1;
        xrcheck(api.create_swapchain(session, &swap, &swapchain));
        if (!swapchain) throw XrFailure{XR_ERROR_RUNTIME_FAILURE};
        XrSwapchainImageD3D12KHR image{}; image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        images = enumerate(image, [&](uint32_t n, uint32_t* count, XrSwapchainImageD3D12KHR* values) {
            return api.images(swapchain, n, count, reinterpret_cast<XrSwapchainImageBaseHeader*>(values)); });
        for (size_t i = 0; i < images.size(); ++i) {
            if (!images[i].texture) throw XrFailure{XR_ERROR_RUNTIME_FAILURE};
            for (size_t j = 0; j < i; ++j) if (images[j].texture == images[i].texture) throw XrFailure{XR_ERROR_RUNTIME_FAILURE};
        }
        gpucheck(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        gpucheck(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        gpucheck(list->Close());
        gpucheck(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) throw GpuFailure{HRESULT_FROM_WIN32(GetLastError())};
    }
    void events() {
        report.stage = 2;
        // Bound pathological runtime event streams; remaining events wait for the next Present.
        for (unsigned count = 0; count < 64; ++count) {
            XrEventDataBuffer event_data{}; event_data.type = XR_TYPE_EVENT_DATA_BUFFER;
            const auto result = api.poll(instance, &event_data);
            if (result == XR_EVENT_UNAVAILABLE) return;
            xrcheck(result);
            if (event_data.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) { terminal = true; return; }
            if (event_data.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
            const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event_data);
            if (changed.session != session) continue;
            if (changed.state == XR_SESSION_STATE_READY && !running) {
                XrSessionBeginInfo begin{}; begin.type = XR_TYPE_SESSION_BEGIN_INFO; begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                xrcheck(api.begin_session(session, &begin)); running = true;
            } else if (changed.state == XR_SESSION_STATE_STOPPING && running) {
                xrcheck(api.end_session(session)); running = false;
            } else if (changed.state == XR_SESSION_STATE_EXITING || changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                terminal = true; return;
            }
        }
    }
    void render(ID3D12Resource* source) {
        report.stage = 3;
        XrFrameWaitInfo wait{}; wait.type = XR_TYPE_FRAME_WAIT_INFO;
        XrFrameState state{}; state.type = XR_TYPE_FRAME_STATE;
        xrcheck(api.wait_frame(session, &wait, &state)); display_time = state.predictedDisplayTime;
        XrFrameBeginInfo begin{}; begin.type = XR_TYPE_FRAME_BEGIN_INFO;
        const auto begun = api.begin_frame(session, &begin);
        if (begun != XR_SUCCESS && begun != XR_FRAME_DISCARDED) throw XrFailure{begun};
        frame = true; ++report.frames;
        XrCompositionLayerQuad quad{}; quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        if (state.shouldRender) {
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo acquire{}; acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
            xrcheck(api.acquire(swapchain, &acquire, &index));
            XrSwapchainImageWaitInfo image_wait{}; image_wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO; image_wait.timeout = 5'000'000;
            xrcheck(api.wait_image(swapchain, &image_wait)); // XR_TIMEOUT_EXPIRED is not success.
            if (index >= images.size()) throw XrFailure{XR_ERROR_RUNTIME_FAILURE};
            report.stage = 4;
            gpucheck(allocator->Reset()); gpucheck(list->Reset(allocator.Get(), nullptr));
            gpucheck(record_eye_copy(list.Get(), source, D3D12_RESOURCE_STATE_PRESENT, images[index].texture, D3D12_RESOURCE_STATE_RENDER_TARGET));
            gpucheck(list->Close());
            pending_source = source; gpu_pending = true;
            ID3D12CommandList* lists[] = {list.Get()};
            queue->ExecuteCommandLists(1, lists);
            gpucheck(queue->Signal(fence.Get(), ++fence_value));
            gpucheck(fence->SetEventOnCompletion(fence_value, event));
            const auto waited = WaitForSingleObject(event, 2000);
            if (waited != WAIT_OBJECT_0) throw GpuFailure{waited == WAIT_TIMEOUT ? DXGI_ERROR_WAIT_TIMEOUT : E_FAIL};
            gpucheck(device->GetDeviceRemovedReason());
            const auto completed = fence->GetCompletedValue();
            if (completed == std::numeric_limits<uint64_t>::max() || completed < fence_value) throw GpuFailure{DXGI_ERROR_DEVICE_REMOVED};
            gpu_pending = false; pending_source.Reset();
            XrSwapchainImageReleaseInfo release{}; release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            xrcheck(api.release(swapchain, &release));
            quad.space = space; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = swapchain;
            quad.subImage.imageRect.extent = {static_cast<int32_t>(source_desc.Width), static_cast<int32_t>(source_desc.Height)};
            quad.pose.orientation.w = 1; quad.pose.position = {0, 0, -2.5f};
            quad.size.width = 3; quad.size.height = 3 * static_cast<float>(source_desc.Height) / static_cast<float>(source_desc.Width);
        }
        report.stage = 5;
        const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        XrFrameEndInfo end{}; end.type = XR_TYPE_FRAME_END_INFO; end.displayTime = display_time;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = state.shouldRender ? 1u : 0u; end.layers = state.shouldRender ? &layer : nullptr;
        frame = false; // EndFrame is attempted exactly once, including failure.
        xrcheck(api.end_frame(session, &end));
        if (state.shouldRender) ++report.submitted;
    }
};
XrPanel::XrPanel(const PanelApi& api) : impl_(std::make_unique<Impl>(api)) {}
XrPanel::~XrPanel() {
    impl_->cleanup();
    if (impl_->gpu_pending) impl_.release(); // Quarantine runtime + GPU allocations until process exit.
}
PanelStats XrPanel::stats() const noexcept { return impl_->report; }
void XrPanel::present(ID3D12CommandQueue* queue, ID3D12Resource* source) noexcept {
    auto& p = *impl_;
    if (p.terminal) return;
    try {
        if (!p.instance) p.initialize(queue, source);
        if (queue != p.queue.Get() || !source) throw GpuFailure{E_INVALIDARG};
        const auto desc = source->GetDesc();
        if (desc.Width != p.source_desc.Width || desc.Height != p.source_desc.Height || desc.Format != p.source_desc.Format)
            throw GpuFailure{DXGI_ERROR_UNSUPPORTED}; // Resize requires an explicit fresh process in this diagnostic.
        p.events();
        if (p.terminal) { p.report.stage = 6; p.cleanup(); return; }
        if (p.running) p.render(source);
    } catch (const XrFailure& error) { p.report.xr = error.result; p.terminal = true; }
      catch (const GpuFailure& error) { p.report.gpu = error.result; p.terminal = true; }
      catch (...) { p.report.gpu = E_FAIL; p.terminal = true; }
    if (p.terminal) { p.report.stage = 7; p.cleanup(); }
}
}
