// SPDX-License-Identifier: MIT
#include "tdvr_xr_panel.h"
namespace tdvr {
PanelApi panel_loader_api() {
    return {xrCreateInstance, xrDestroyInstance, xrGetSystem, xrGetInstanceProcAddr,
        xrCreateSession, xrDestroySession, xrEnumerateSwapchainFormats, xrCreateSwapchain,
        xrDestroySwapchain, xrEnumerateSwapchainImages, xrCreateReferenceSpace, xrDestroySpace,
        xrPollEvent, xrBeginSession, xrEndSession, xrWaitFrame, xrBeginFrame, xrEndFrame,
        xrAcquireSwapchainImage, xrWaitSwapchainImage, xrReleaseSwapchainImage, xrLocateViews};
}
}
