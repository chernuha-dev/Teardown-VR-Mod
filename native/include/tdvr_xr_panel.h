#pragma once
// SPDX-License-Identifier: MIT
#include <windows.h>
#include <d3d12.h>
#include <dxgicommon.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <memory>
#include "tdvr_tracking.h"

namespace tdvr {
// Explicit diagnostic: a flat game image on one quad, NOT stereo scene rendering.
struct PanelApi {
    PFN_xrCreateInstance create_instance = nullptr;
    PFN_xrDestroyInstance destroy_instance = nullptr;
    PFN_xrGetSystem get_system = nullptr;
    PFN_xrGetInstanceProcAddr get_proc = nullptr;
    PFN_xrCreateSession create_session = nullptr;
    PFN_xrDestroySession destroy_session = nullptr;
    PFN_xrEnumerateSwapchainFormats formats = nullptr;
    PFN_xrCreateSwapchain create_swapchain = nullptr;
    PFN_xrDestroySwapchain destroy_swapchain = nullptr;
    PFN_xrEnumerateSwapchainImages images = nullptr;
    PFN_xrCreateReferenceSpace create_space = nullptr;
    PFN_xrDestroySpace destroy_space = nullptr;
    PFN_xrPollEvent poll = nullptr;
    PFN_xrBeginSession begin_session = nullptr;
    PFN_xrEndSession end_session = nullptr;
    PFN_xrWaitFrame wait_frame = nullptr;
    PFN_xrBeginFrame begin_frame = nullptr;
    PFN_xrEndFrame end_frame = nullptr;
    PFN_xrAcquireSwapchainImage acquire = nullptr;
    PFN_xrWaitSwapchainImage wait_image = nullptr;
    PFN_xrReleaseSwapchainImage release = nullptr;
    PFN_xrLocateViews locate_views = nullptr;
};
struct PanelStats {
    uint64_t frames = 0, submitted = 0;
    XrResult xr = XR_SUCCESS;
    HRESULT gpu = S_OK;
    uint32_t stage = 0; // 0 idle, 1 initializing, 2 events, 3 frame, 4 copy, 5 submit, 6 stopped, 7 failed
    bool quarantined = false;
    uint32_t source_format = 0, swapchain_format = 0, resource_format = 0, color_space = 0, failure_stage = 0;
};
class XrPanel {
public:
    explicit XrPanel(const PanelApi& api);
    ~XrPanel();
    XrPanel(const XrPanel&) = delete;
    XrPanel& operator=(const XrPanel&) = delete;
    // Caller serializes calls on the Present thread. Source is a live backbuffer
    // in PRESENT state on this DIRECT queue. Failed unfinished GPU work retains
    // its source; a completed successful copy releases it before returning.
    void present(ID3D12CommandQueue* queue, ID3D12Resource* source, DXGI_COLOR_SPACE_TYPE color_space) noexcept;
    PanelStats stats() const noexcept;
    TdvrXrViewSnapshot views() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
PanelApi panel_loader_api(); // Used only by opt-in capture startup, never DllMain.
}
