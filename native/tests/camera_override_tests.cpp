// SPDX-License-Identifier: MIT
#include "tdvr_camera_override.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
namespace {
void require(bool value, const char* message) { if (!value) { std::cerr << message << '\n'; std::exit(1); } }
TdvrCameraOverrideRequest request() {
    TdvrCameraOverrideRequest r{};
    r.size = sizeof(r); r.abi = 1; r.render_calls = 2; r.duration_ms = 100;
    r.frame.frame = {123, 0};
    r.frame.world_from_tracking = {{3,2,1},{0,0,0,1}};
    r.frame.world_scale = 1; r.frame.near_clip_m = .05; r.frame.far_clip_m = 500;
    r.frame.eyes[0] = {r.frame.frame, {{-.032,0,0},{0,0,0,1}}, {-.9,.7,.8,-.6}, 15};
    r.frame.eyes[1] = {r.frame.frame, {{ .032,0,0},{0,0,0,1}}, {-.7,.9,.6,-.8}, 15};
    return r;
}
}
int main() {
    TdvrCameraOverride control;
    TdvrEngineCameraMatrices out{};
    std::memset(&out, 0xa5, sizeof(out)); const auto before = out;
    require(!control.take(0, out) && !std::memcmp(&out, &before, sizeof(out)), "inactive override wrote output");
    auto r = request();
    require(control.arm(r, 1000) && control.remaining(1001) == 2, "valid override refused");
    auto bad = r; bad.frame.eyes[1].frame.simulation_id++;
    require(!control.arm(bad, 1001) && control.remaining(1001) == 2, "bad right eye replaced pending pair");
    require(control.take(1099, out) && control.remaining(1099) == 1, "selected eye not consumed once");
    const auto left = out;
    require(!control.take(1100, out) && !std::memcmp(&out, &left, sizeof(out)), "expired request applied or changed output");
    r.eye = 1;
    require(control.arm(r, 2000) && control.take(2001, out), "right eye refused");
    require(out.projection[8] != left.projection[8] && out.view[12] != left.view[12], "selected eye projection/pose ignored");
    require(control.take(2002, out) && !control.take(2003, out), "render budget not enforced");
    r.render_calls = 601; require(!control.arm(r, 0), "excessive render budget accepted");
    r = request(); r.duration_ms = 10001; require(!control.arm(r, 0), "excessive lifetime accepted");
    r.duration_ms = 0; require(!control.arm(r, 0), "zero lifetime accepted");
    r = request(); require(!control.arm(r, std::numeric_limits<uint64_t>::max()-50), "expiry overflow accepted");
    require(control.arm(r, 3000), "valid rearm failed");
    r.render_calls = 0; r.frame = {}; require(control.arm(r, 3001) && !control.take(3002, out), "explicit cancellation failed");
    r.abi = 0; require(!control.arm(r, 0), "unknown ABI accepted");
    std::cout << "Bounded synthetic camera requests, per-eye selection and expiry passed\n";
}
