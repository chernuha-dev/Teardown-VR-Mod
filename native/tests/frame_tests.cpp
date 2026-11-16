#include "tdvr_frame.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

namespace {
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
bool close(double a, double b) { return std::abs(a - b) < 1e-9; }
using Point = std::array<double, 4>;
Point transform(const TdvrMatrix& matrix, const Point& point) {
    Point result{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            result[row] += matrix.m[row * 4 + column] * point[column];
    return result;
}
TdvrFrameInput input_frame() {
    const TdvrFrameKey key{42, 123456789000};
    const double s = std::sqrt(.5);
    // Base yaw +90 degrees; tracking eyes pitch +30 degrees.
    const double pitch = 0.5235987755982988;
    const std::uint32_t flags = TDVR_ORIENTATION_VALID | TDVR_POSITION_VALID
        | TDVR_ORIENTATION_TRACKED | TDVR_POSITION_TRACKED;
    return TdvrFrameInput{
        key, {{10, 3, 20}, {0, s, 0, s}}, 2, .05, 100,
        {
            {key, {{-.032, .15, -.10}, {std::sin(pitch/2), 0, 0, std::cos(pitch/2)}},
                {-.9, .7, .8, -.6}, flags},
            {key, {{.032, .15, -.10}, {std::sin(pitch/2), 0, 0, std::cos(pitch/2)}},
                {-.7, .9, .75, -.65}, flags}
        }
    };
}

// Independent closed-form Ry(90) * Rx(30), with analytically known eye centres.
// Does not use production pose composition or the packet's reported world pose.
Point eye_to_world(int eye, double x, double y, double z) {
    const double s = .5, c = std::sqrt(.75);
    return Point{{9.8 + s*y + c*z, 3.3 + c*y - s*z,
        20 + (eye == 0 ? .064 : -.064) - x, 1}};
}

void expect_rejected(const TdvrFrameInput& invalid, TdvrStereoFrame& output, const char* message) {
    std::array<unsigned char, sizeof(TdvrStereoFrame)> before{};
    std::memcpy(before.data(), &output, sizeof(output));
    require(tdvr_build_stereo_frame(&invalid, &output) == TDVR_INVALID_ARGUMENT, message);
    require(std::memcmp(before.data(), &output, sizeof(output)) == 0,
        "rejection partially changed the published stereo packet");
}
}

int main() {
    static_assert(std::is_same<decltype(std::declval<TdvrStereoFrame&>().left_eye()),
        const TdvrEyeFrame&>::value, "published eyes must be read-only");
    static_assert(std::is_trivially_copyable<TdvrStereoFrame>::value,
        "packet must own its data without heap objects or external references");
    TdvrStereoFrame frame;
    require(!frame.valid(), "default packet should not be renderable");
    auto input = input_frame();
    require(tdvr_build_stereo_frame(&input, &frame) == TDVR_OK, "stereo construction failed");
    require(frame.valid() && frame.key().simulation_id == 42
        && frame.key().predicted_display_time_ns == 123456789000, "frame identity lost");
    require(close(frame.world_scale(), 2), "scale metadata lost");
    require(close(frame.left_eye().world_pose.position[0], 9.8)
        && close(frame.left_eye().world_pose.position[1], 3.3)
        && close(frame.left_eye().world_pose.position[2], 20.064), "eye transform or height doubled");
    require(close(frame.left_eye().world_pose.position[2] - frame.right_eye().world_pose.position[2],
        .128), "scaled eye baseline or heading incorrect");
    require(!close(frame.left_eye().projection.m[2], frame.right_eye().projection.m[2]),
        "independent asymmetric projections collapsed to one eye");

    const TdvrEyeFrame* eyes[2] = {&frame.left_eye(), &frame.right_eye()};
    for (int eye = 0; eye < 2; ++eye) {
        const auto& matrices = *eyes[eye];
        const auto& fov = input.eyes[eye].fov;
        const auto centre = transform(matrices.view, eye_to_world(eye, 0, 0, 0));
        for (int axis = 0; axis < 3; ++axis) require(close(centre[axis], 0), "eye origin not inverted");
        for (double depth : {.1, 2., 200.}) {
            const std::array<Point, 4> local{{
                Point{{std::tan(fov.left)*depth, 0, -depth, 1}},
                Point{{std::tan(fov.right)*depth, 0, -depth, 1}},
                Point{{0, std::tan(fov.up)*depth, -depth, 1}},
                Point{{0, std::tan(fov.down)*depth, -depth, 1}}
            }};
            for (int edge = 0; edge < 4; ++edge) {
                const auto& expected = local[edge];
                const auto world = eye_to_world(eye, expected[0], expected[1], expected[2]);
                const auto camera = transform(matrices.view, world);
                for (int axis = 0; axis < 4; ++axis)
                    require(close(camera[axis], expected[axis]), "inverse view / yaw-pitch order incorrect");
                const auto clip = transform(matrices.projection, camera);
                const int axis = edge < 2 ? 0 : 1;
                const double boundary = edge == 0 || edge == 3 ? -1. : 1.;
                require(close(clip[axis] / clip[3], boundary), "world point missed per-eye frustum edge");
                if (depth == .1) require(close(clip[2] / clip[3], 0), "scaled near plane incorrect");
                if (depth == 200) require(close(clip[2] / clip[3], 1), "scaled far plane incorrect");
            }
        }
    }

    // Input ownership: changing the source after construction cannot alter either eye.
    const double saved_x = frame.left_eye().world_pose.position[0];
    input.eyes[0].tracking_pose.position[0] = 1000;
    input.frame.simulation_id = 1000;
    require(close(frame.left_eye().world_pose.position[0], saved_x)
        && frame.key().simulation_id == 42, "packet retained mutable input");

    auto invalid = input_frame();
    --invalid.eyes[1].frame.simulation_id;
    expect_rejected(invalid, frame, "mixed simulation states accepted");
    invalid = input_frame();
    --invalid.eyes[1].frame.predicted_display_time_ns;
    expect_rejected(invalid, frame, "mixed prediction times accepted");
    invalid = input_frame();
    ++invalid.frame.predicted_display_time_ns;
    expect_rejected(invalid, frame, "stale tagged eyes accepted for a new target frame");
    for (std::uint32_t flag : {1u, 2u, 4u, 8u}) {
        invalid = input_frame();
        invalid.eyes[1].pose_flags &= ~flag;
        expect_rejected(invalid, frame, "invalid or untracked right eye accepted");
    }
    invalid = input_frame();
    invalid.eyes[0].pose_flags |= 16u;
    expect_rejected(invalid, frame, "unknown tracking flags accepted");
    invalid = input_frame();
    invalid.eyes[1].fov.left = invalid.eyes[1].fov.right;
    expect_rejected(invalid, frame, "invalid right frustum accepted");
    invalid = input_frame();
    invalid.eyes[1].tracking_pose.orientation[3] = 2;
    expect_rejected(invalid, frame, "invalid quaternion accepted");
    invalid = input_frame();
    invalid.world_from_tracking.position[0] = std::numeric_limits<double>::quiet_NaN();
    expect_rejected(invalid, frame, "NaN base accepted");
    invalid = input_frame();
    invalid.world_scale = 0;
    expect_rejected(invalid, frame, "zero scale accepted");
    invalid = input_frame();
    invalid.near_clip_m = -1;
    expect_rejected(invalid, frame, "negative clip plane accepted");
    invalid = input_frame();
    invalid.far_clip_m = std::numeric_limits<double>::max();
    expect_rejected(invalid, frame, "overflowing scaled clip plane accepted");
    invalid = input_frame();
    invalid.world_from_tracking.position[0] = std::numeric_limits<double>::max();
    invalid.eyes[1].tracking_pose.position[2] = std::numeric_limits<double>::max();
    expect_rejected(invalid, frame, "overflowing world pose accepted");
    require(tdvr_build_stereo_frame(nullptr, &frame) == TDVR_INVALID_ARGUMENT, "null input accepted");
    require(tdvr_build_stereo_frame(&invalid, nullptr) == TDVR_INVALID_ARGUMENT, "null output accepted");

    // Small runtime quaternion rounding is normalized before rotation, avoiding
    // artificial scaling/skew of the eye baseline and its inverse view matrix.
    input = input_frame();
    for (double& value : input.world_from_tracking.orientation) value *= 1.000001;
    for (auto& eye : input.eyes)
        for (double& value : eye.tracking_pose.orientation) value *= .999999;
    require(tdvr_build_stereo_frame(&input, &frame) == TDVR_OK, "rounding tolerance rejected");
    const auto known = transform(frame.right_eye().view, eye_to_world(1, .4, .2, -3));
    require(close(known[0], .4) && close(known[1], .2) && close(known[2], -3),
        "non-unit rounding introduced view skew");

    // The base is the tracking origin. Floor-origin height and recentered eye
    // height are alternative input conventions; neither receives extra offsets.
    input = input_frame();
    input.world_scale = 1;
    input.world_from_tracking = TdvrPose{{0, 0, 0}, {0, 0, 0, 1}};
    input.eyes[0].tracking_pose = TdvrPose{{-.032, 1.6, 0}, {0, 0, 0, 1}};
    input.eyes[1].tracking_pose = TdvrPose{{.032, 1.6, 0}, {0, 0, 0, 1}};
    require(tdvr_build_stereo_frame(&input, &frame) == TDVR_OK
        && close(frame.left_eye().world_pose.position[1], 1.6), "floor-origin height doubled");
    input.world_from_tracking.position[1] = 1.6;
    for (auto& eye : input.eyes) eye.tracking_pose.position[1] = 0;
    const double s = std::sqrt(.5);
    input.eyes[1].tracking_pose.orientation[1] = s;
    input.eyes[1].tracking_pose.orientation[3] = s;
    require(tdvr_build_stereo_frame(&input, &frame) == TDVR_OK
        && close(frame.right_eye().world_pose.position[1], 1.6), "recentered eye height doubled");
    const auto forward_left = transform(frame.left_eye().view, Point{{-.032, 1.6, -1, 1}});
    const auto forward_right = transform(frame.right_eye().view, Point{{-.968, 1.6, 0, 1}});
    require(close(forward_left[0], 0) && close(forward_left[2], -1)
        && close(forward_right[0], 0) && close(forward_right[2], -1),
        "independent eye orientation discarded or heading doubled");

    std::cout << "Owned stereo packet, tracked poses and view/projection checks passed; "
        "no engine, GPU timing or headset behavior tested.\n";
}
