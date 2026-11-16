// SPDX-License-Identifier: MIT
#include "tdvr_engine_camera.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace {
bool convert(const TdvrMatrix& input, float (&output)[16]) {
    for (int row = 0; row < 4; ++row) for (int column = 0; column < 4; ++column) {
        const double value = input.m[row * 4 + column];
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) return false;
        output[column * 4 + row] = static_cast<float>(value);
    }
    return true;
}
}

TdvrResult tdvr_prepare_engine_cameras(const TdvrStereoFrame& frame, TdvrEngineCameraPair* output) {
    if (!output || !frame.valid()) return TDVR_INVALID_ARGUMENT;
    TdvrEngineCameraPair result{};
    result.frame = frame.key();
    const TdvrEyeFrame* eyes[] = {&frame.left_eye(), &frame.right_eye()};
    for (int eye = 0; eye < 2; ++eye) {
        auto& target = result.eyes[eye];
        if (!convert(eyes[eye]->projection, target.projection) || !convert(eyes[eye]->view, target.view))
            return TDVR_INVALID_ARGUMENT;
        // RH finite-depth projection must remain nondegenerate in engine precision.
        // Rounding near/far separation away would break depth reconstruction.
        if (!(target.projection[0] > 0 && target.projection[5] > 0
            && target.projection[10] < -1 && target.projection[14] < 0
            && target.projection[11] == -1)) return TDVR_INVALID_ARGUMENT;
        // Reconstruct finite clip distances from the original DOUBLE projection,
        // not its rounded float coefficients. These are in scaled world units.
        const double a = eyes[eye]->projection.m[10], b = eyes[eye]->projection.m[11];
        const double near_clip = b/a, far_clip = b/(a+1);
        if (!std::isfinite(near_clip) || !std::isfinite(far_clip)
            || near_clip <= 0 || far_clip > std::numeric_limits<float>::max()) return TDVR_INVALID_ARGUMENT;
        target.near_clip = static_cast<float>(near_clip);
        target.far_clip = static_cast<float>(far_clip);
        if (!(target.near_clip > 0 && target.far_clip > target.near_clip)) return TDVR_INVALID_ARGUMENT;
    }
    *output = result;
    return TDVR_OK;
}

TdvrEngineCameraScope::TdvrEngineCameraScope(TdvrEngineCameraMatrices& camera,
    const TdvrEngineCameraMatrices& eye) noexcept : camera_(camera) {
    std::memcpy(&original_, &camera_, sizeof(camera_));
    std::memcpy(&camera_, &eye, sizeof(camera_));
}
TdvrEngineCameraScope::~TdvrEngineCameraScope() {
    std::memcpy(&camera_, &original_, sizeof(camera_));
}
