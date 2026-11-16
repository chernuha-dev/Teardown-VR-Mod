#pragma once
#include "tdvr_openxr_probe.h"
#include "tdvr_frame.h"

namespace tdvr {
struct FrameDispatch {
    PFN_xrWaitFrame wait_frame;
    PFN_xrBeginFrame begin_frame;
    PFN_xrLocateViews locate_views;
    PFN_xrAcquireSwapchainImage acquire_image;
    PFN_xrWaitSwapchainImage wait_image;
    PFN_xrReleaseSwapchainImage release_image;
    PFN_xrEndFrame end_frame;
};
struct FrameTarget {
    XrSwapchain swapchain;
    uint32_t image_count;
    XrExtent2Di extent;
};
struct FrameSettings {
    // Caller owns a running PRIMARY_STEREO session and a live reference space.
    XrSession session;
    XrSpace space;
    XrEnvironmentBlendMode blend_mode;
    FrameTarget eyes[2];
    TdvrPose world_from_tracking;
    double world_scale;
    double near_clip_m;
    double far_clip_m;
    uint64_t simulation_id; // producer guarantees one immutable simulation state
};
// Called ONCE for the pair after both images have been acquired and waited.
// Must finish all GPU access, transition images back to the runtime's required
// state and retain no packet references before returning OR throwing. This
// scheduling contract cannot enforce fences or freeze the actual game.
using RenderStereo = bool (*)(const TdvrStereoFrame&, const uint32_t indices[2], void*);
struct FrameReport {
    XrResult result = XR_ERROR_VALIDATION_FAILURE;
    std::string stage;
    XrTime display_time = 0;
    bool submitted = false;
    // On any ownership/timeout error, caller must stop frames and destroy session.
    bool restart_session = false;
};
// One frame only: no session start, input, event polling or game hooks.
// Finite image wait (5ms). If wait times out, no illegal release is attempted;
// the session owner must tear down acquired images by destroying the session.
FrameReport run_stereo_frame(const FrameDispatch& api, const FrameSettings& settings,
    RenderStereo render, void* context = nullptr);
}
