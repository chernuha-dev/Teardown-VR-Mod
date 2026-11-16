#pragma once
// SPDX-License-Identifier: MIT
#include "tdvr_math.h"
#include <cstdint>

// Owned observation of one LocateViews call in LOCAL space. No simulation id:
// frame_id counts XR frames only and does not prove a frozen game state.
struct TdvrXrViewSnapshot {
    uint32_t size, abi, available;
    int32_t sample_result;
    uint64_t frame_id;
    int64_t predicted_display_time;
    uint64_t view_state_flags;
    TdvrPose eyes[2]; // RH metres, xyzw, runtime eye separation already included
    TdvrFov fov[2];  // signed radians; never derived from desktop aspect ratio
};
static_assert(sizeof(TdvrXrViewSnapshot) == 216, "XR view observation ABI");
