// SPDX-License-Identifier: MIT
#include "tdvr_camera_override.h"
#include <limits>

bool TdvrCameraOverride::arm(const TdvrCameraOverrideRequest& request, uint64_t now_ms) {
    if (request.size != sizeof(request) || request.abi != 1 || request.reserved) return false;
    if (!request.render_calls) { remaining_ = 0; return true; }
    if (request.eye > 1 || request.render_calls > 600 || !request.duration_ms || request.duration_ms > 10000
        || now_ms > std::numeric_limits<uint64_t>::max()-request.duration_ms) return false;
    TdvrStereoFrame frame;
    TdvrEngineCameraPair prepared;
    if (tdvr_build_stereo_frame(&request.frame, &frame) != TDVR_OK
        || tdvr_prepare_engine_cameras(frame, &prepared) != TDVR_OK) return false;
    cameras_ = prepared;
    eye_ = request.eye;
    expires_ = now_ms + request.duration_ms;
    remaining_ = request.render_calls;
    return true;
}
uint32_t TdvrCameraOverride::remaining(uint64_t now_ms) const {
    return now_ms < expires_ ? remaining_ : 0;
}
bool TdvrCameraOverride::take(uint64_t now_ms, TdvrEngineCameraMatrices& output) {
    if (!remaining(now_ms)) { remaining_ = 0; return false; }
    output = cameras_.eyes[eye_];
    --remaining_;
    return true;
}

bool TdvrTrackedCameraPreview::arm(const TdvrTrackedCameraRequest& request, uint64_t now_ms) {
    if (request.size != sizeof(request) || request.abi != 1 || request.reserved) return false;
    if (!request.render_calls) { remaining_ = 0; return true; }
    if (request.eye > 1 || request.render_calls > 600 || !request.duration_ms || request.duration_ms > 10000
        || now_ms > std::numeric_limits<uint64_t>::max()-request.duration_ms) return false;
    const TdvrPose identity{{0,0,0},{0,0,0,1}};
    TdvrPose checked;
    TdvrFov fov{-.8,.8,.8,-.8}; TdvrMatrix projection;
    if (tdvr_pose_to_world(&request.world_anchor,&identity,request.world_scale,&checked) != TDVR_OK
        || tdvr_projection(&fov,request.near_clip_m,request.far_clip_m,&projection) != TDVR_OK) return false;
    request_ = request; remaining_ = request.render_calls; expires_ = now_ms+request.duration_ms;
    last_frame_ = 0; last_time_ = 0; calibrated_ = false;
    return true;
}
uint32_t TdvrTrackedCameraPreview::remaining(uint64_t now_ms) const { return now_ms < expires_ ? remaining_ : 0; }
bool TdvrTrackedCameraPreview::take(const TdvrXrViewSnapshot& current, uint64_t now_ms, TdvrEngineCameraMatrices& output) {
    if (!remaining(now_ms)) { remaining_ = 0; return false; }
    // A loss/duplicate terminates the preview; never reuse a last-known pose or
    // silently recalibrate after recovery. Explicit rearming is required.
    if (current.frame_id <= last_frame_ || current.predicted_display_time <= last_time_) { remaining_ = 0; return false; }
    TdvrPose origin = origin_;
    if (!calibrated_ && tdvr_calibrate_tracking_origin(current,request_.world_anchor,request_.world_scale,&origin) != TDVR_OK) {
        remaining_ = 0; return false;
    }
    const TdvrTrackingCameraContext context{current.frame_id,{0,current.predicted_display_time},origin,
        request_.world_scale,request_.near_clip_m,request_.far_clip_m};
    TdvrEngineCameraPair cameras;
    if (tdvr_prepare_tracked_cameras(current,context,&cameras) != TDVR_OK) { remaining_ = 0; return false; }
    output = cameras.eyes[request_.eye]; origin_ = origin; calibrated_ = true;
    last_frame_ = current.frame_id; last_time_ = current.predicted_display_time; --remaining_;
    return true;
}
