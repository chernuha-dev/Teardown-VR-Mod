// SPDX-License-Identifier: MIT
// Real WARP copies, fake XR dispatch. No loader, SteamVR, game or headset.
#include "tdvr_xr_panel.h"
#include "tdvr_eye_copy.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <deque>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
void require(bool condition, const char* message) { if (!condition) { std::cerr << message << '\n'; std::exit(1); } }
void check(HRESULT value) { require(SUCCEEDED(value), "D3D12 failure"); }
template<class T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }
struct Fixture {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Resource> source, target;
    std::deque<XrSessionState> events{XR_SESSION_STATE_READY};
    unsigned begins = 0, ends = 0, acquired = 0, released = 0, frame_ends = 0, quads = 0;
    unsigned instances_destroyed = 0, sessions_destroyed = 0, spaces_destroyed = 0, swaps_destroyed = 0;
    bool render = true, wrong_luid = false, unsupported_format = false, bad_index = false;
    XrResult create_result = XR_SUCCESS, image_wait_result = XR_SUCCESS, end_result = XR_SUCCESS, destroy_result = XR_SUCCESS;
    void submit(ID3D12GraphicsCommandList* list) {
        check(list->Close()); ID3D12CommandList* lists[] = {list}; queue->ExecuteCommandLists(1, lists);
        ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr); require(event != nullptr, "event");
        check(queue->Signal(fence.Get(), 1)); check(fence->SetEventOnCompletion(1, event));
        require(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0 && fence->GetCompletedValue() == 1, "GPU fence"); CloseHandle(event);
    }
    ComPtr<ID3D12Resource> texture(D3D12_RESOURCE_STATES initial, UINT64 width = 19, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width; desc.Height = 11; desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Format = format; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> resource;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr, IID_PPV_ARGS(&resource)));
        return resource;
    }
    explicit Fixture(DXGI_FORMAT target_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> adapter; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
        source = texture(D3D12_RESOURCE_STATE_RENDER_TARGET); target = texture(D3D12_RESOURCE_STATE_RENDER_TARGET, 19, target_format);
        ComPtr<ID3D12DescriptorHeap> rtv; D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv))); const auto view = rtv->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(source.Get(), nullptr, view);
        ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list; check(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        const float red[] = {0.25f, 0.5f, 0.75f, 1}; list->ClearRenderTargetView(view, red, 0, nullptr);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {source.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT}; list->ResourceBarrier(1, &barrier);
        submit(list.Get());
    }
    void pixels_on_release() {
        // This runs INSIDE fake xrReleaseSwapchainImage. All pixels must already
        // be copied, and destination must have returned to RENDER_TARGET.
        const auto desc = target->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = bytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
        ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list; check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {target.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE}; list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{}; from.pResource = target.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = layout;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET; list->ResourceBarrier(1, &barrier);
        submit(list.Get());
        void* data = nullptr; D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)}; check(readback->Map(0, &range, &data));
        for (unsigned y = 0; y < 11; ++y) for (unsigned x = 0; x < 19; ++x) {
            const auto pixel = static_cast<const unsigned char*>(data) + layout.Offset + y * layout.Footprint.RowPitch + x * 4;
            require(pixel[0] == 64 && pixel[1] == 128 && pixel[2] == 191 && pixel[3] == 255, "XR image pixels not ready at release");
        }
        D3D12_RANGE no_writes{}; readback->Unmap(0, &no_writes);
    }
};
Fixture* f = nullptr;
#define FN(name, args) XrResult XRAPI_CALL name args
FN(create_instance, (const XrInstanceCreateInfo* info, XrInstance* out)) {
    require(info->enabledExtensionCount == 1 && std::strcmp(info->enabledExtensionNames[0], XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0, "D3D12 extension");
    if (f->create_result == XR_SUCCESS) *out = handle<XrInstance>(1); return f->create_result;
}
FN(destroy_instance, (XrInstance)) { ++f->instances_destroyed; return XR_SUCCESS; }
FN(get_system, (XrInstance, const XrSystemGetInfo*, XrSystemId* out)) { *out = 1; return XR_SUCCESS; }
FN(requirements, (XrInstance, XrSystemId, XrGraphicsRequirementsD3D12KHR* out)) {
    out->adapterLuid = f->device->GetAdapterLuid(); if (f->wrong_luid) out->adapterLuid.LowPart ^= 1;
    out->minFeatureLevel = D3D_FEATURE_LEVEL_11_0; return XR_SUCCESS;
}
FN(get_proc, (XrInstance, const char* name, PFN_xrVoidFunction* out)) {
    require(std::strcmp(name, "xrGetD3D12GraphicsRequirementsKHR") == 0, "requirements proc");
    *out = reinterpret_cast<PFN_xrVoidFunction>(requirements); return XR_SUCCESS;
}
FN(create_session, (XrInstance, const XrSessionCreateInfo* info, XrSession* out)) {
    const auto binding = static_cast<const XrGraphicsBindingD3D12KHR*>(info->next);
    require(binding->device == f->device.Get() && binding->queue == f->queue.Get(), "game queue binding");
    *out = handle<XrSession>(2); return XR_SUCCESS;
}
FN(destroy_session, (XrSession)) { ++f->sessions_destroyed; return f->destroy_result; }
FN(formats, (XrSession, uint32_t capacity, uint32_t* count, int64_t* out)) { *count = 1; if (capacity) out[0] = f->unsupported_format ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; return XR_SUCCESS; }
FN(create_swapchain, (XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* out)) { require(info->width == 19 && info->height == 11 && info->format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "exact format/extent"); *out = handle<XrSwapchain>(3); return XR_SUCCESS; }
FN(destroy_swapchain, (XrSwapchain)) { ++f->swaps_destroyed; return XR_SUCCESS; }
FN(images, (XrSwapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* out)) { *count = 1; if (capacity) reinterpret_cast<XrSwapchainImageD3D12KHR*>(out)->texture = f->target.Get(); return XR_SUCCESS; }
FN(create_space, (XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* out)) { require(info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL && info->poseInReferenceSpace.orientation.w == 1, "local origin"); *out = handle<XrSpace>(4); return XR_SUCCESS; }
FN(destroy_space, (XrSpace)) { ++f->spaces_destroyed; return XR_SUCCESS; }
FN(poll, (XrInstance, XrEventDataBuffer* out)) {
    if (f->events.empty()) return XR_EVENT_UNAVAILABLE;
    auto event = reinterpret_cast<XrEventDataSessionStateChanged*>(out); event->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    event->session = handle<XrSession>(2); event->state = f->events.front(); f->events.pop_front(); return XR_SUCCESS;
}
FN(begin_session, (XrSession, const XrSessionBeginInfo* info)) { require(info->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, "stereo session type"); ++f->begins; return XR_SUCCESS; }
FN(end_session, (XrSession)) { ++f->ends; return XR_SUCCESS; }
FN(wait_frame, (XrSession, const XrFrameWaitInfo*, XrFrameState* out)) { out->predictedDisplayTime = 123456; out->shouldRender = f->render; return XR_SUCCESS; }
FN(begin_frame, (XrSession, const XrFrameBeginInfo*)) { return XR_SUCCESS; }
FN(end_frame, (XrSession, const XrFrameEndInfo* info)) {
    ++f->frame_ends; require(info->displayTime == 123456 && info->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE, "frame timing");
    if (info->layerCount) {
        require(info->layerCount == 1 && f->released == f->quads + 1, "one released quad");
        const auto quad = reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[0]);
        require(quad->type == XR_TYPE_COMPOSITION_LAYER_QUAD && quad->eyeVisibility == XR_EYE_VISIBILITY_BOTH
            && quad->subImage.imageRect.extent.width == 19 && quad->subImage.imageRect.extent.height == 11
            && quad->pose.position.z == -2.5f && quad->pose.orientation.w == 1, "flat panel metadata"); ++f->quads;
    }
    return f->end_result;
}
FN(acquire, (XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index)) { ++f->acquired; *index = f->bad_index ? 99 : 0; return XR_SUCCESS; }
FN(wait_image, (XrSwapchain, const XrSwapchainImageWaitInfo* info)) { require(info->timeout == 5'000'000, "bounded image wait"); return f->image_wait_result; }
FN(release, (XrSwapchain, const XrSwapchainImageReleaseInfo*)) { f->pixels_on_release(); ++f->released; return XR_SUCCESS; }
tdvr::PanelApi api() { return {create_instance, destroy_instance, get_system, get_proc, create_session, destroy_session,
    formats, create_swapchain, destroy_swapchain, images, create_space, destroy_space, poll, begin_session, end_session,
    wait_frame, begin_frame, end_frame, acquire, wait_image, release}; }
}
int main() {
    { Fixture test(DXGI_FORMAT_R8G8B8A8_TYPELESS); f = &test; tdvr::XrPanel panel(api());
      panel.present(f->queue.Get(), f->source.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
      require(panel.stats().submitted == 1 && f->released == 1, "typeless XR storage rejected"); }
    { Fixture test; f = &test;
      ComPtr<ID3D12CommandAllocator> allocator; check(f->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
      ComPtr<ID3D12GraphicsCommandList> list; check(f->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
      require(tdvr::record_eye_copy(list.Get(), f->source.Get(), D3D12_RESOURCE_STATE_PRESENT, f->target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET) == E_INVALIDARG, "strict eye copy must still reject UNORM/SRGB mismatch");
      require(tdvr::record_sdr_display_copy(list.Get(), f->source.Get(), D3D12_RESOURCE_STATE_PRESENT, f->target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == E_INVALIDARG, "linear input rejected");
      const auto wrong = f->texture(D3D12_RESOURCE_STATE_RENDER_TARGET, 19, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
      require(tdvr::record_sdr_display_copy(list.Get(), f->source.Get(), D3D12_RESOURCE_STATE_PRESENT, wrong.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == E_INVALIDARG, "channel swizzle rejected");
      tdvr::XrPanel panel(api()); panel.present(f->queue.Get(), f->source.Get(), DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
      require(panel.stats().stage == 7 && panel.stats().gpu == DXGI_ERROR_UNSUPPORTED && f->instances_destroyed == 0, "HDR rejected before XR"); }
    { tdvr::XrPanel incomplete({}); incomplete.present(nullptr, nullptr, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
      require(incomplete.stats().stage == 7 && incomplete.stats().xr == XR_ERROR_VALIDATION_FAILURE, "incomplete dispatch"); }
    { Fixture test; f = &test; { tdvr::XrPanel panel(api());
        panel.present(f->queue.Get(), f->source.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709); panel.present(f->queue.Get(), f->source.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        require(panel.stats().submitted == 2 && f->released == 2, "two copied frames");
        f->render = false; panel.present(f->queue.Get(), f->source.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709); require(f->acquired == 2 && f->frame_ends == 3, "shouldRender false");
        f->events.push_back(XR_SESSION_STATE_STOPPING); panel.present(f->queue.Get(), f->source.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709); require(f->ends == 1 && f->frame_ends == 3, "STOPPING");
        f->events.push_back(XR_SESSION_STATE_EXITING); panel.present(f->queue.Get(), f->source.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709); require(panel.stats().stage == 6, "EXITING");
      } require(f->instances_destroyed == 1 && f->sessions_destroyed == 1 && f->spaces_destroyed == 1 && f->swaps_destroyed == 1, "cleanup once"); }
    for (unsigned scenario = 0; scenario < 6; ++scenario) { Fixture test; f = &test;
        if (scenario == 0) f->create_result = XR_ERROR_RUNTIME_UNAVAILABLE;
        if (scenario == 1) f->wrong_luid = true;
        if (scenario == 2) f->unsupported_format = true;
        if (scenario == 3) f->image_wait_result = XR_TIMEOUT_EXPIRED;
        if (scenario == 4) f->bad_index = true;
        if (scenario == 5) f->end_result = XR_ERROR_RUNTIME_FAILURE;
        tdvr::XrPanel panel(api()); panel.present(f->queue.Get(), f->source.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        require(panel.stats().stage == 7 && !panel.stats().quarantined && !panel.stats().submitted, "failure terminates diagnostic");
        if (scenario != 5) require(f->released == 0, "no premature release");
        if (scenario >= 3) require(f->frame_ends == 1, "failed frame ended once");
        const auto count = f->frame_ends; panel.present(f->queue.Get(), f->source.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709); require(f->frame_ends == count, "no automatic retry");
    }
    { Fixture test; f = &test; tdvr::XrPanel panel(api()); panel.present(f->queue.Get(), f->source.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
      const auto resized = f->texture(D3D12_RESOURCE_STATE_PRESENT, 20); panel.present(f->queue.Get(), resized.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
      require(panel.stats().stage == 7 && panel.stats().gpu == DXGI_ERROR_UNSUPPORTED && f->frame_ends == 1, "resize stops panel before another frame"); }
    { Fixture test; f = &test; tdvr::XrPanel panel(api()); panel.present(f->queue.Get(), f->source.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
      f->destroy_result = XR_ERROR_RUNTIME_FAILURE; f->events.push_back(XR_SESSION_STATE_EXITING); panel.present(f->queue.Get(), f->source.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
      require(panel.stats().stage == 7 && panel.stats().xr == XR_ERROR_RUNTIME_FAILURE, "cleanup failure reported"); }
    std::cout << "XR panel fake lifecycle + WARP readback at release boundary passed\n";
}
