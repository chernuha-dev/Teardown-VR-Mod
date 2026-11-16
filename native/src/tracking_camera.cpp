// SPDX-License-Identifier: MIT
#include "tdvr_tracking_camera.h"
#include <cmath>

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

TdvrResult tdvr_calibrate_tracking_origin(const TdvrXrViewSnapshot& views,
    const TdvrPose& world_anchor, double world_scale, TdvrPose* output) {
    if (!output) return TDVR_INVALID_ARGUMENT;
    // Reuse the full pair validator: malformed right-eye data must not establish
    // an origin that only appeared valid through its left eye.
    const TdvrTrackingCameraContext context{views.frame_id,
        {0,views.predicted_display_time}, {{0,0,0},{0,0,0,1}}, world_scale,.05,500};
    TdvrEngineCameraPair validated;
    if (tdvr_prepare_tracked_cameras(views,context,&validated) != TDVR_OK) return TDVR_INVALID_ARGUMENT;
    TdvrPose center{{0,0,0},{0,0,0,0}};
    double dot = 0, norm = 0;
    for (unsigned i = 0; i < 4; ++i) dot += views.eyes[0].orientation[i]*views.eyes[1].orientation[i];
    const double sign = dot < 0 ? -1.0 : 1.0; // q and -q describe the same rotation
    for (unsigned i = 0; i < 4; ++i) {
        center.orientation[i] = .5*views.eyes[0].orientation[i]+.5*sign*views.eyes[1].orientation[i];
        norm += center.orientation[i]*center.orientation[i];
    }
    if (!std::isfinite(norm) || norm <= 0) return TDVR_INVALID_ARGUMENT;
    for (double& value : center.orientation) value /= std::sqrt(norm);
    for (unsigned i = 0; i < 3; ++i) center.position[i] = .5*views.eyes[0].position[i]+.5*views.eyes[1].position[i];
    TdvrPose inverse_rotation{{0,0,0},{-center.orientation[0],-center.orientation[1],
        -center.orientation[2],center.orientation[3]}};
    const TdvrPose negative_center{{-center.position[0],-center.position[1],-center.position[2]},{0,0,0,1}};
    TdvrPose inverse, result;
    // T_origin = T_game * inverse(S * T_center); scale translation exactly once.
    if (tdvr_pose_to_world(&inverse_rotation,&negative_center,world_scale,&inverse) != TDVR_OK
        || tdvr_pose_to_world(&world_anchor,&inverse,1,&result) != TDVR_OK) return TDVR_INVALID_ARGUMENT;
    *output = result;
    return TDVR_OK;
}
