// SPDX-License-Identifier: MIT
#include "tdvr_tracking_camera.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void require(bool yes, const char* message) { if (!yes) { std::cerr << message << '\n'; std::exit(1); } }
bool close(double a, double b) { return std::abs(a-b) < 2e-5; }
void transform(const float* matrix, const double* in, double* out) {
    for (unsigned row = 0; row < 4; ++row) {
        out[row] = 0;
        for (unsigned column = 0; column < 4; ++column) out[row] += matrix[column*4+row] * in[column];
    }
}
}
int main() {
    TdvrXrViewSnapshot views{sizeof(TdvrXrViewSnapshot), 1, 1, 0, 91, 123456789, 15};
    views.eyes[0] = {{-.032, 1.7, -.25}, {0,0,0,1}};
    views.eyes[1] = {{ .036, 1.7, -.25}, {0,0,0,1}};
    views.fov[0] = {-.9, 1.0, .8, -1.1}; views.fov[1] = {-1.1, .8, 1.0, -.7};
    const double s = std::sqrt(.5);
    TdvrTrackingCameraContext context{91, {77,123456789}, {{10,2,-20},{0,s,0,s}}, 2, .05, 250};
    TdvrEngineCameraPair output{};
    require(tdvr_prepare_tracked_cameras(views, context, &output) == TDVR_OK, "valid located pair refused");
    require(output.frame.simulation_id == 77 && output.frame.predicted_display_time_ns == 123456789,
        "XR frame number replaced actual simulation identity");
    for (unsigned eye = 0; eye < 2; ++eye) {
        // Analytic yaw-90 origin + scaled LOCAL eye: no second IPD/head-height addition.
        const double x = views.eyes[eye].position[0];
        const double world_eye[] = {9.5,5.4,-20-2*x,1}; double eye_point[4]{};
        transform(output.eyes[eye].view, world_eye, eye_point);
        require(close(eye_point[0],0) && close(eye_point[1],0) && close(eye_point[2],0), "scale/origin/eye offset applied incorrectly");
        require(close(output.eyes[eye].near_clip,.1) && close(output.eyes[eye].far_clip,500), "clip scale applied incorrectly");
        // At depth 3, forward is world -X; signed left FOV edge must land at NDC -1.
        const double left_point[] = {world_eye[0]-3,world_eye[1],world_eye[2]-3*std::tan(views.fov[eye].left),1};
        double local[4]{}, clip[4]{};
        transform(output.eyes[eye].view,left_point,local); transform(output.eyes[eye].projection,local,clip);
        require(close(clip[0]/clip[3],-1), "runtime asymmetric FOV lost on engine conversion");
    }
    require(output.eyes[0].projection[8] != output.eyes[1].projection[8], "independent FOV collapsed");
    for (auto& eye : views.eyes) {
        eye.orientation[1] = .5; eye.orientation[3] = std::sqrt(.75); // tracking yaw 60 + origin yaw 90
    }
    require(tdvr_prepare_tracked_cameras(views,context,&output) == TDVR_OK, "rotated head refused");
    for (unsigned eye = 0; eye < 2; ++eye) {
        const double edge = 3*std::tan(views.fov[eye].left);
        const double world_point[] = {9.5-1.5-std::sqrt(.75)*edge,5.4,-20-2*views.eyes[eye].position[0]+3*std::sqrt(.75)-.5*edge,1};
        double local[4]{}, clip[4]{};
        transform(output.eyes[eye].view,world_point,local); transform(output.eyes[eye].projection,local,clip);
        require(close(clip[0]/clip[3],-1), "head/body yaw composed twice or lost");
    }
    const auto original = output;
    for (unsigned scenario = 0; scenario < 13; ++scenario) {
        auto bad = views; auto settings = context;
        switch (scenario) {
        case 0: bad.frame_id--; break;
        case 1: settings.frame.predicted_display_time_ns++; break;
        case 2: bad.available = 0; break;
        case 3: bad.view_state_flags = 3; break;
        case 4: bad.sample_result = -1; break;
        case 5: bad.eyes[1].position[0] = std::numeric_limits<double>::quiet_NaN(); break;
        case 6: bad.fov[1].right = bad.fov[1].left; break;
        case 7: settings.world_scale = 0; break;
        case 8: settings.far_clip_m = 1e12; break; // loses depth in float engine record
        case 9: bad.eyes[1].orientation[3] = 2; break;
        case 10: bad.abi = 2; break;
        case 11: bad.size--; break;
        case 12: bad.predicted_display_time = settings.frame.predicted_display_time_ns = 0; break;
        }
        require(tdvr_prepare_tracked_cameras(bad,settings,&output) == TDVR_INVALID_ARGUMENT
            && !std::memcmp(&output,&original,sizeof(output)), "invalid/stale pair altered camera output");
    }
    require(tdvr_prepare_tracked_cameras(views,context,nullptr) == TDVR_INVALID_ARGUMENT, "null output accepted");
    const TdvrPose anchor{{10,2,-20},{0,s,0,s}};
    TdvrPose origin;
    require(tdvr_calibrate_tracking_origin(views,anchor,2,&origin) == TDVR_OK, "origin calibration refused");
    TdvrPose world_eyes[2];
    for (unsigned eye = 0; eye < 2; ++eye) {
        require(tdvr_pose_to_world(&origin,&views.eyes[eye],2,&world_eyes[eye]) == TDVR_OK, "calibrated eye refused");
        for (unsigned i = 0; i < 4; ++i)
            require(close(world_eyes[eye].orientation[i],anchor.orientation[i]), "head rotation doubled during calibration");
    }
    for (unsigned i = 0; i < 3; ++i)
        require(close(.5*world_eyes[0].position[i]+.5*world_eyes[1].position[i],anchor.position[i]),
            "calibration added HMD height or moved eye midpoint");
    // Do not recalibrate on motion: origin yaw = game 90 - HMD 60 = 30 degrees.
    auto moved = views.eyes[0]; moved.position[0] += .1;
    TdvrPose world_moved;
    require(tdvr_pose_to_world(&origin,&moved,2,&world_moved) == TDVR_OK, "relative movement refused");
    require(close(world_moved.position[0]-world_eyes[0].position[0],std::sqrt(.75)*.2)
        && close(world_moved.position[1]-world_eyes[0].position[1],0)
        && close(world_moved.position[2]-world_eyes[0].position[2],-.1), "roomscale rotation/scale applied twice");
    const auto calibrated = origin;
    auto antipodal = views;
    for (double& value : antipodal.eyes[1].orientation) value = -value;
    require(tdvr_calibrate_tracking_origin(antipodal,anchor,2,&origin) == TDVR_OK, "antipodal eye quaternion refused");
    for (unsigned i = 0; i < 3; ++i) require(close(origin.position[i],calibrated.position[i]), "quaternion sign changed origin");
    for (unsigned i = 0; i < 4; ++i) require(close(origin.orientation[i],calibrated.orientation[i]), "quaternion sign changed rotation");
    for (unsigned failure = 0; failure < 5; ++failure) {
        auto bad = views; auto bad_anchor = anchor; double scale = 2;
        switch (failure) {
        case 0: bad.available = 0; break;
        case 1: bad.eyes[1].position[1] = std::numeric_limits<double>::infinity(); break;
        case 2: bad_anchor.orientation[3] = 2; break;
        case 3: scale = 0; break;
        case 4: bad.view_state_flags = 3; break;
        }
        const auto untouched = origin;
        require(tdvr_calibrate_tracking_origin(bad,bad_anchor,scale,&origin) == TDVR_INVALID_ARGUMENT
            && !std::memcmp(&origin,&untouched,sizeof(origin)), "invalid calibration altered origin");
    }
    require(tdvr_calibrate_tracking_origin(views,anchor,2,nullptr) == TDVR_INVALID_ARGUMENT, "null calibration output accepted");
    // Full 3-axis rotations, not only horizontal head/body yaw.
    auto tilted = views;
    const TdvrPose center{{.002,1.7,-.25},{.2,.3,.1,std::sqrt(.86)}};
    const TdvrPose tilted_anchor{{-5,3,7},{.1,-.2,.3,std::sqrt(.86)}};
    for (auto& eye : tilted.eyes)
        for (unsigned i = 0; i < 4; ++i) eye.orientation[i] = center.orientation[i];
    require(tdvr_calibrate_tracking_origin(tilted,tilted_anchor,.5,&origin) == TDVR_OK, "tilted calibration refused");
    TdvrPose mapped_center;
    require(tdvr_pose_to_world(&origin,&center,.5,&mapped_center) == TDVR_OK, "tilted center refused");
    for (unsigned i = 0; i < 3; ++i) require(close(mapped_center.position[i],tilted_anchor.position[i]), "tilted center drifted");
    for (unsigned i = 0; i < 4; ++i) require(close(mapped_center.orientation[i],tilted_anchor.orientation[i]), "tilted center rotation drifted");
    // Different valid eye orientations use their shortest-arc midpoint.
    tilted.eyes[0].orientation[0] = tilted.eyes[0].orientation[2] = 0;
    tilted.eyes[1].orientation[0] = tilted.eyes[1].orientation[2] = 0;
    tilted.eyes[0].orientation[1] = -.5; tilted.eyes[1].orientation[1] = .5;
    tilted.eyes[0].orientation[3] = tilted.eyes[1].orientation[3] = std::sqrt(.75);
    const TdvrPose identity_center{{.002,1.7,-.25},{0,0,0,1}};
    require(tdvr_calibrate_tracking_origin(tilted,tilted_anchor,1,&origin) == TDVR_OK
        && tdvr_pose_to_world(&origin,&identity_center,1,&mapped_center) == TDVR_OK, "eye orientation midpoint refused");
    for (unsigned i = 0; i < 4; ++i) require(close(mapped_center.orientation[i],tilted_anchor.orientation[i]), "eye midpoint chose wrong rotation");
    std::cout << "Tracked cameras and calibrated origin preserve pose, height, IPD and roomscale\n";
}
