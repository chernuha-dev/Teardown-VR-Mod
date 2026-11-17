// SPDX-License-Identifier: MIT
#include "tdvr_camera_override.h"
#include <cstdlib>
#include <cmath>
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
    TdvrTrackedCameraRequest tracked{sizeof(TdvrTrackedCameraRequest),1,0,3,100,0,
        {{10,2,-20},{0,0,0,1}},1,.05,500};
    TdvrTrackedCameraPreview preview;
    TdvrXrViewSnapshot views{sizeof(TdvrXrViewSnapshot),1,1,0,10,100,15};
    views.eyes[0] = {{-.032,1.7,0},{0,0,0,1}}; views.eyes[1] = {{.032,1.7,0},{0,0,0,1}};
    views.fov[0] = {-.9,.7,.8,-.8}; views.fov[1] = {-.7,.9,.8,-.8};
    require(preview.arm(tracked,1000) && preview.take(views,1001,out), "tracked preview refused");
    const auto first = out;
    require(std::abs(out.view[13]+2) < 1e-5, "HMD height added to game eye");
    for (auto& eye : views.eyes) eye.position[0] += .25;
    ++views.frame_id; ++views.predicted_display_time;
    require(preview.take(views,1002,out) && std::abs(out.view[12]-first.view[12]+.25) < 1e-5,
        "preview recalibrated on movement instead of tracking");
    const auto second = out;
    require(!preview.take(views,1003,out) && !preview.remaining(1003)
        && !std::memcmp(&second,&out,sizeof(out)), "duplicate pose applied or output changed");
    require(preview.arm(tracked,2000), "tracked rearm refused");
    ++views.frame_id; ++views.predicted_display_time; views.available = 0;
    require(!preview.take(views,2001,out) && !preview.remaining(2001), "tracking loss did not stop preview");
    views.available = 1;
    require(!preview.take(views,2002,out), "tracking recovery silently rearmed preview");
    require(preview.arm(tracked,3000) && !preview.take(views,3100,out), "tracked expiry not enforced");
    tracked.render_calls = 1; tracked.eye = 1;
    require(preview.arm(tracked,4000) && preview.take(views,4001,out) && !preview.take(views,4002,out)
        && out.projection[8] != first.projection[8], "tracked eye selection/budget ignored");
    tracked.render_calls = 601; require(!preview.arm(tracked,0), "tracked budget overflow accepted");
    tracked.render_calls = 1; tracked.duration_ms = 10001; require(!preview.arm(tracked,0), "tracked duration overflow accepted");
    tracked.duration_ms = 100;
    require(!preview.arm(tracked,std::numeric_limits<uint64_t>::max()-50), "tracked expiry overflow accepted");
    require(preview.arm(tracked,5000), "tracked request refused before cancellation test");
    auto bad_tracked = tracked; bad_tracked.world_anchor.orientation[3] = 2;
    require(!preview.arm(bad_tracked,5001) && preview.remaining(5001) == 1, "invalid request replaced tracked preview");
    tracked.render_calls = 0; tracked.world_anchor = {};
    require(preview.arm(tracked,5002) && !preview.take(views,5003,out), "tracked cancellation failed");
    std::cout << "Synthetic and live-pose preview budgets, origin, freshness, loss and expiry passed\n";
}
