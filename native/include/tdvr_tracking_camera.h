#pragma once
// SPDX-License-Identifier: MIT
#include "tdvr_tracking.h"
#include "tdvr_engine_camera.h"

// Supplied by the owner of the CURRENT render frame, not copied from the
// snapshot to make an old observation pass. No clock conversion or stale fallback.
struct TdvrTrackingCameraContext {
    uint64_t xr_frame_id;
    TdvrFrameKey frame; // actual game simulation identity + expected XR display time
    TdvrPose world_from_tracking; // tracking origin, NOT game eye plus HMD height
    double world_scale, near_clip_m, far_clip_m;
};

// Produces both engine camera records atomically from one owned located pair.
// Does not establish simulation ownership, refresh caches, render, or consume
// the XR frame. Caller must serialize frame lifecycle and prevent duplicate use.
// Output is untouched on any failure.
TdvrResult tdvr_prepare_tracked_cameras(const TdvrXrViewSnapshot& views,
    const TdvrTrackingCameraContext& context, TdvrEngineCameraPair* output);
