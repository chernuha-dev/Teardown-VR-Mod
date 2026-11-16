// SPDX-License-Identifier: MIT
#include "tdvr_engine_camera.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
using Point = std::array<double, 4>;
Point multiply(const float* matrix, const Point& point) {
    Point result{};
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col)
        result[row] += matrix[col*4+row] * point[col];
    return result;
}
TdvrFrameInput input() {
    TdvrFrameInput result{};
    result.frame = {21, 987654321};
    // Yaw +90 degrees and a nonzero origin catch row/column and inverse errors.
    const double s = std::sqrt(.5);
    result.world_from_tracking = {{10, 2, -20}, {0, s, 0, s}};
    result.world_scale = 2;
    result.near_clip_m = .05; result.far_clip_m = 250;
    result.eyes[0] = {result.frame, {{-.032, .1, 0}, {0,0,0,1}}, {-.9,.7,.8,-.6}, 15};
    result.eyes[1] = {result.frame, {{ .032, .1, 0}, {0,0,0,1}}, {-.7,.9,.6,-.8}, 15};
    return result;
}
TdvrStereoFrame build(const TdvrFrameInput& data) {
    TdvrStereoFrame result;
    require(tdvr_build_stereo_frame(&data, &result) == TDVR_OK, "could not build valid frame");
    return result;
}
}
int main() {
    auto data = input();
    TdvrEngineCameraPair pair{};
    require(tdvr_prepare_engine_cameras(build(data), &pair) == TDVR_OK, "camera conversion failed");
    require(pair.frame.simulation_id == 21 && pair.frame.predicted_display_time_ns == 987654321,
        "camera pair lost frame identity");
    require(pair.eyes[0].projection[8] != pair.eyes[1].projection[8], "asymmetry lost");
    for (int eye = 0; eye < 2; ++eye) {
        require(std::abs(pair.eyes[eye].near_clip-.1) < 1e-7
            && std::abs(pair.eyes[eye].far_clip-500) < 1e-3, "engine clip fields lost world scale");
        // Analytic Ry(90) * tracking, independently of reported packet world_pose.
        const Point world_origin{{10, 2.2, -20 + (eye == 0 ? .064 : -.064), 1}};
        const auto origin = multiply(pair.eyes[eye].view, world_origin);
        for (int axis = 0; axis < 3; ++axis) require(std::abs(origin[axis]) < 2e-6, "wrong view layout/inverse");
        const auto& fov = data.eyes[eye].fov;
        for (double distance : {.1, 2., 500.}) {
            // Local (x,y,-d) becomes world (-d,y,-x) after base yaw.
            const std::array<Point, 4> local{{
                {{std::tan(fov.left)*distance,0,-distance,1}},
                {{std::tan(fov.right)*distance,0,-distance,1}},
                {{0,std::tan(fov.up)*distance,-distance,1}},
                {{0,std::tan(fov.down)*distance,-distance,1}}
            }};
            for (int edge = 0; edge < 4; ++edge) {
                const Point world{{world_origin[0]+local[edge][2], world_origin[1]+local[edge][1],
                    world_origin[2]-local[edge][0], 1}};
                const auto clip = multiply(pair.eyes[eye].projection, multiply(pair.eyes[eye].view, world));
                require(std::abs(clip[edge < 2 ? 0 : 1]/clip[3] - (edge == 0 || edge == 3 ? -1 : 1)) < 3e-5,
                    "engine matrices fail frustum edge");
                if (distance == .1) require(std::abs(clip[2]/clip[3]) < 2e-5, "near depth wrong");
                if (distance == 500) require(std::abs(clip[2]/clip[3]-1) < 2e-6, "far depth wrong");
            }
        }
    }
    const auto saved_pair = pair;
    auto reject = [&](const TdvrStereoFrame& frame) {
        require(tdvr_prepare_engine_cameras(frame, &pair) == TDVR_INVALID_ARGUMENT, "unsafe camera accepted");
        require(std::memcmp(&saved_pair, &pair, sizeof(pair)) == 0, "partial pair published");
    };
    reject(TdvrStereoFrame{});
    data.world_from_tracking.position[0] = std::numeric_limits<double>::max()/4;
    reject(build(data)); // valid CPU doubles, not representable by engine float camera
    data = input(); data.eyes[1].tracking_pose.position[0] = std::numeric_limits<double>::max()/8;
    reject(build(data)); // left conversion succeeds, right fails; neither may be published
    data = input(); data.near_clip_m = 1e-20; data.far_clip_m = 1e20;
    reject(build(data)); // near/far separation rounded away in float
    require(tdvr_prepare_engine_cameras(build(input()), nullptr) == TDVR_INVALID_ARGUMENT, "null output accepted");

    struct CameraWithGuards { std::array<unsigned char, 17> before; TdvrEngineCameraMatrices camera;
        std::array<unsigned char, 23> after; } memory;
    std::memset(&memory, 0xa5, sizeof(memory)); // includes padding and arbitrary original matrix bits
    CameraWithGuards original;
    std::memcpy(&original, &memory, sizeof(memory));
    for (const auto& eye : pair.eyes) {
        {
            TdvrEngineCameraScope scope(memory.camera, eye);
            require(std::memcmp(&memory.camera, &eye, sizeof(eye)) == 0, "eye not installed");
            require(memory.before == original.before && memory.after == original.after, "camera overrun");
        }
        require(std::memcmp(&memory, &original, sizeof(memory)) == 0, "normal exit did not restore camera");
    }
    try {
        TdvrEngineCameraScope scope(memory.camera, pair.eyes[1]);
        memory.camera.view[12] = 123; // even a failed renderer's write must be restored
        throw std::runtime_error("render failure");
    } catch (const std::runtime_error&) {}
    require(std::memcmp(&memory, &original, sizeof(memory)) == 0, "exception leaked eye into desktop camera");
    std::cout << "Engine camera matrices and scoped restoration passed\n";
}
