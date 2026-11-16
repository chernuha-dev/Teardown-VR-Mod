#include "tdvr_frame.h"
#include <cmath>

namespace {
bool same_key(const TdvrFrameKey& a, const TdvrFrameKey& b) {
    return a.simulation_id == b.simulation_id
        && a.predicted_display_time_ns == b.predicted_display_time_ns;
}

bool normalize_pose(TdvrPose& pose) {
    for (double value : pose.position) if (!std::isfinite(value)) return false;
    double norm = 0;
    for (double value : pose.orientation) {
        if (!std::isfinite(value)) return false;
        norm += value * value;
    }
    if (!std::isfinite(norm) || std::abs(norm - 1.0) > 1e-5) return false;
    const double length = std::sqrt(norm);
    for (double& value : pose.orientation) value /= length;
    return true;
}

bool inverse_pose(const TdvrPose& pose, TdvrMatrix& view) {
    const double x = pose.orientation[0], y = pose.orientation[1];
    const double z = pose.orientation[2], w = pose.orientation[3];
    // Transpose of the unit-quaternion rotation, then -R^T * position.
    view = TdvrMatrix{{
        1 - 2 * (y*y + z*z), 2 * (x*y + z*w), 2 * (x*z - y*w), 0,
        2 * (x*y - z*w), 1 - 2 * (x*x + z*z), 2 * (y*z + x*w), 0,
        2 * (x*z + y*w), 2 * (y*z - x*w), 1 - 2 * (x*x + y*y), 0,
        0, 0, 0, 1
    }};
    for (int row = 0; row < 3; ++row) {
        view.m[row * 4 + 3] = -(view.m[row * 4] * pose.position[0]
            + view.m[row * 4 + 1] * pose.position[1]
            + view.m[row * 4 + 2] * pose.position[2]);
    }
    for (double value : view.m) if (!std::isfinite(value)) return false;
    return true;
}
}

TdvrResult tdvr_build_stereo_frame(const TdvrFrameInput* input, TdvrStereoFrame* output) {
    if (!input || !output) return TDVR_INVALID_ARGUMENT;
    TdvrFrameInput snapshot = *input;
    constexpr std::uint32_t required_flags = TDVR_ORIENTATION_VALID | TDVR_POSITION_VALID
        | TDVR_ORIENTATION_TRACKED | TDVR_POSITION_TRACKED;
    if (!std::isfinite(snapshot.world_scale) || snapshot.world_scale <= 0
        || !normalize_pose(snapshot.world_from_tracking)) return TDVR_INVALID_ARGUMENT;

    TdvrStereoFrame result;
    result.frame_ = snapshot.frame;
    result.world_from_tracking_ = snapshot.world_from_tracking;
    result.world_scale_ = snapshot.world_scale;
    for (int eye = 0; eye < 2; ++eye) {
        auto& sample = snapshot.eyes[eye];
        auto& target = result.eyes_[eye];
        if (!same_key(sample.frame, snapshot.frame) || sample.pose_flags != required_flags
            || !normalize_pose(sample.tracking_pose)
            || tdvr_pose_to_world(&snapshot.world_from_tracking, &sample.tracking_pose,
                snapshot.world_scale, &target.world_pose) != TDVR_OK
            || !normalize_pose(target.world_pose)
            || !inverse_pose(target.world_pose, target.view)
            || tdvr_projection(&sample.fov, snapshot.near_clip_m * snapshot.world_scale,
                snapshot.far_clip_m * snapshot.world_scale, &target.projection) != TDVR_OK)
            return TDVR_INVALID_ARGUMENT;
    }
    result.valid_ = true;
    *output = result;
    return TDVR_OK;
}
