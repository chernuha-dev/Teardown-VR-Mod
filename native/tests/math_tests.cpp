#include "tdvr_math.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
bool close(double a, double b) { return std::abs(a - b) < 1e-9; }
double ndc(const TdvrMatrix& m, int row, double x, double y, double z) {
    return (m.m[row*4]*x + m.m[row*4+1]*y + m.m[row*4+2]*z + m.m[row*4+3]) / -z;
}

int main() {
    const double s = std::sqrt(0.5);
    TdvrPose base{{10, 20, 30}, {0, s, 0, s}};
    TdvrPose hand{{0, 1, -1}, {s, 0, 0, s}}, out{};
    require(tdvr_pose_to_world(&base, &hand, 2, &out) == TDVR_OK, "pose composition failed");
    require(close(out.position[0], 8) && close(out.position[1], 22)
        && close(out.position[2], 30), "yaw / metre scale / translation incorrect");
    require(close(out.orientation[0], .5) && close(out.orientation[1], .5)
        && close(out.orientation[2], -.5) && close(out.orientation[3], .5), "quaternion order incorrect");
    TdvrPose invalid = hand;
    invalid.orientation[3] = 0;
    require(tdvr_pose_to_world(&base, &invalid, 1, &out) == TDVR_INVALID_ARGUMENT, "bad quaternion accepted");
    require(close(out.position[0], 8), "failure modified output");
    require(tdvr_pose_to_world(&base, &hand, 0, &out) == TDVR_INVALID_ARGUMENT, "zero scale accepted");
    invalid = hand;
    invalid.position[0] = std::numeric_limits<double>::quiet_NaN();
    require(tdvr_pose_to_world(&base, &invalid, 1, &out) == TDVR_INVALID_ARGUMENT, "NaN accepted");
    require(tdvr_pose_to_world(nullptr, &hand, 1, &out) == TDVR_INVALID_ARGUMENT, "null accepted");
    // IPD is already present in eye translation: preserve it through heading and world scale.
    TdvrPose identity{{0, 0, 0}, {0, 0, 0, 1}};
    TdvrPose left{{-.032, 0, 0}, {0, 0, 0, 1}}, right{{.032, 0, 0}, {0, 0, 0, 1}};
    TdvrPose l{}, r{};
    require(tdvr_pose_to_world(&identity, &left, 1, &l) == TDVR_OK
        && tdvr_pose_to_world(&identity, &right, 1, &r) == TDVR_OK, "eye poses failed");
    require(close(r.position[0] - l.position[0], .064), "IPD changed");
    // Positive pitch rotates forward upward; positive roll rotates right upward.
    for (const auto rotation : {TdvrPose{{0,0,0},{s,0,0,s}}, TdvrPose{{0,0,0},{0,0,s,s}}}) {
        TdvrPose axis = identity;
        if (rotation.orientation[0] != 0) axis.position[2] = -1;
        else axis.position[0] = 1;
        require(tdvr_pose_to_world(&rotation, &axis, 1, &out) == TDVR_OK, "axis pose failed");
        require(close(out.position[1], 1), "pitch / roll sign incorrect");
    }

    // Both eyes: four asymmetric frustum edges must map to clip edges at every depth.
    for (const auto fov : {TdvrFov{-.9, .7, .8, -.6}, TdvrFov{-.7, .9, .8, -.6}}) {
        TdvrMatrix m{};
        require(tdvr_projection(&fov, .05, 1000, &m) == TDVR_OK, "projection failed");
        for (double depth : {.05, 1., 1000.}) {
            require(close(ndc(m, 0, std::tan(fov.left)*depth, 0, -depth), -1), "left edge");
            require(close(ndc(m, 0, std::tan(fov.right)*depth, 0, -depth), 1), "right edge");
            require(close(ndc(m, 1, 0, std::tan(fov.up)*depth, -depth), 1), "top edge");
            require(close(ndc(m, 1, 0, std::tan(fov.down)*depth, -depth), -1), "bottom edge");
        }
        require(close(ndc(m, 2, 0, 0, -.05), 0), "near plane");
        require(close(ndc(m, 2, 0, 0, -1000), 1), "far plane");
    }
    TdvrFov fov{-.8, .8, .8, -.8}; TdvrMatrix m{};
    require(tdvr_projection(&fov, 1, 1, &m) == TDVR_INVALID_ARGUMENT, "equal planes accepted");
    fov.left = fov.right;
    require(tdvr_projection(&fov, .05, 100, &m) == TDVR_INVALID_ARGUMENT, "empty frustum accepted");
    fov.left = std::numeric_limits<double>::infinity();
    require(tdvr_projection(&fov, .05, 100, &m) == TDVR_INVALID_ARGUMENT, "infinite angle accepted");
    fov.left = -1.6;
    require(tdvr_projection(&fov, .05, 100, &m) == TDVR_INVALID_ARGUMENT, "angle beyond pi/2 accepted");
    require(tdvr_projection(nullptr, .05, 100, &m) == TDVR_INVALID_ARGUMENT, "null FOV accepted");
    std::cout << "Pose and asymmetric D3D projection checks passed; no VR rendering tested.\n";
}
