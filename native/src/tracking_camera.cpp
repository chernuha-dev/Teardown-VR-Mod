// SPDX-License-Identifier: MIT
#include "tdvr_tracking_camera.h"

TdvrResult tdvr_prepare_tracked_cameras(const TdvrXrViewSnapshot& views,
    const TdvrTrackingCameraContext& context, TdvrEngineCameraPair* output) {
    constexpr uint32_t required = TDVR_ORIENTATION_VALID | TDVR_POSITION_VALID
        | TDVR_ORIENTATION_TRACKED | TDVR_POSITION_TRACKED;
    if (!output || views.size != sizeof(views) || views.abi != 1 || views.available != 1
        || views.sample_result != 0 || !context.xr_frame_id || views.frame_id != context.xr_frame_id
        || views.predicted_display_time <= 0 || views.predicted_display_time != context.frame.predicted_display_time_ns
        || (views.view_state_flags & required) != required) return TDVR_INVALID_ARGUMENT;
    TdvrFrameInput input{};
    input.frame = context.frame;
    input.world_from_tracking = context.world_from_tracking;
    input.world_scale = context.world_scale;
    input.near_clip_m = context.near_clip_m; input.far_clip_m = context.far_clip_m;
    for (unsigned eye = 0; eye < 2; ++eye)
        input.eyes[eye] = {context.frame, views.eyes[eye], views.fov[eye], required};
    TdvrStereoFrame frame;
    if (tdvr_build_stereo_frame(&input, &frame) != TDVR_OK) return TDVR_INVALID_ARGUMENT;
    return tdvr_prepare_engine_cameras(frame, output);
}
