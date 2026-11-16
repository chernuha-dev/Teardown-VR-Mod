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
