#include "tdvr_math.h"
#include <cmath>
#include <initializer_list>

namespace {
bool valid_pose(const TdvrPose& p) {
    for (double v : p.position) if (!std::isfinite(v)) return false;
    double norm = 0;
    for (double v : p.orientation) {
        if (!std::isfinite(v)) return false;
        norm += v * v;
    }
    return std::abs(norm - 1.0) <= 1e-5;
}
}

TdvrResult tdvr_pose_to_world(const TdvrPose* base, const TdvrPose* tracking,
                             double world_scale, TdvrPose* output) {
    if (!base || !tracking || !output || !valid_pose(*base) || !valid_pose(*tracking)
        || !std::isfinite(world_scale) || world_scale <= 0) return TDVR_INVALID_ARGUMENT;
    TdvrPose result{};
    const auto& q = base->orientation;
    const auto& r = tracking->orientation;
    double v[3], cross[3];
    for (int i = 0; i < 3; ++i) v[i] = tracking->position[i] * world_scale;
    cross[0] = 2 * (q[1] * v[2] - q[2] * v[1]);
    cross[1] = 2 * (q[2] * v[0] - q[0] * v[2]);
    cross[2] = 2 * (q[0] * v[1] - q[1] * v[0]);
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3, k = (i + 2) % 3;
        result.position[i] = base->position[i] + v[i] + q[3] * cross[i]
                             + q[j] * cross[k] - q[k] * cross[j];
        result.orientation[i] = q[3] * r[i] + r[3] * q[i] + q[j] * r[k] - q[k] * r[j];
    }
    result.orientation[3] = q[3] * r[3] - q[0] * r[0] - q[1] * r[1] - q[2] * r[2];
    if (!valid_pose(result)) return TDVR_INVALID_ARGUMENT;
    *output = result;
    return TDVR_OK;
}

TdvrResult tdvr_projection(const TdvrFov* fov, double near_z, double far_z, TdvrMatrix* output) {
    if (!fov || !output || !std::isfinite(near_z) || !std::isfinite(far_z)
        || near_z <= 0 || far_z <= near_z) return TDVR_INVALID_ARGUMENT;
    constexpr double half_pi = 1.5707963267948966;
    for (double angle : {fov->left, fov->right, fov->up, fov->down})
        if (!std::isfinite(angle) || std::abs(angle) >= half_pi) return TDVR_INVALID_ARGUMENT;
    const double l = std::tan(fov->left), r = std::tan(fov->right);
    const double u = std::tan(fov->up), d = std::tan(fov->down);
    if (r <= l || u <= d) return TDVR_INVALID_ARGUMENT;
    TdvrMatrix result{};
    result.m[0] = 2 / (r - l);
    result.m[2] = (r + l) / (r - l);
    result.m[5] = 2 / (u - d);
    result.m[6] = (u + d) / (u - d);
    result.m[10] = -far_z / (far_z - near_z);
    result.m[11] = -far_z * near_z / (far_z - near_z);
    result.m[14] = -1;
    for (double v : result.m) if (!std::isfinite(v)) return TDVR_INVALID_ARGUMENT;
    *output = result;
    return TDVR_OK;
}
