#pragma once
#include "tdvr_openxr_probe.h"
#include <cstdint>

namespace tdvr {
// Allocation/lifetime probe only. No begin/end session, frame, locate or input APIs.
struct SessionProbeDispatch {
    PFN_xrCreateSession create_session;
    PFN_xrDestroySession destroy_session;
    PFN_xrEnumerateSwapchainFormats formats;
    PFN_xrCreateSwapchain create_swapchain;
    PFN_xrDestroySwapchain destroy_swapchain;
    PFN_xrEnumerateSwapchainImages images;
};
struct SessionProbeReport {
    XrResult result = XR_ERROR_VALIDATION_FAILURE;
    std::string stage;
    std::int64_t format = 0;
    std::uint32_t image_count[2]{};
    // Number of attempted destroys, including failed destroys. Never implies success.
    std::uint32_t destroyed_swapchains = 0;
    bool destroyed_session = false;
};
// Instance must have enabled D3D12 and queried graphics requirements first.
// Device/queue must remain alive for the whole call and satisfy runtime.
// Each eye gets its own tiny 64x64, sample-count 1 swapchain. Images are inspected
// only for non-null pointers: no GPU use or presentation while session is idle.
SessionProbeReport probe_session(const SessionProbeDispatch& api, XrInstance instance,
    XrSystemId system, ID3D12Device* device, ID3D12CommandQueue* queue);
}
