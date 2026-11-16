#pragma once
// SPDX-License-Identifier: MIT
#include "tdvr_engine_camera.h"

// Explicit diagnostic input, NOT OpenXR tracking or a legacy Lua packet.
struct TdvrCameraOverrideRequest {
    uint32_t size, abi, eye, render_calls, duration_ms, reserved;
    TdvrFrameInput frame;
};

// Caller serializes access. Both cameras are prepared before replacing an active
// request; take() consumes exactly one selected camera. It never renders/replays.
class TdvrCameraOverride {
public:
    bool arm(const TdvrCameraOverrideRequest& request, uint64_t now_ms);
    bool take(uint64_t now_ms, TdvrEngineCameraMatrices& output);
    uint32_t remaining(uint64_t now_ms) const;
private:
    TdvrEngineCameraPair cameras_{};
    uint64_t expires_ = 0;
    uint32_t remaining_ = 0, eye_ = 0;
};
