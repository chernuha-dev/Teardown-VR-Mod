#pragma once
// SPDX-License-Identifier: MIT
#include "tdvr_engine_camera.h"
#include "tdvr_tracking_camera.h"

// Explicit diagnostic input, NOT OpenXR tracking or a legacy Lua packet.
struct TdvrCameraOverrideRequest {
    uint32_t size, abi, eye, render_calls, duration_ms, reserved;
    TdvrFrameInput frame;
};

struct TdvrTrackedCameraRequest {
    uint32_t size, abi, eye, render_calls, duration_ms, reserved;
    TdvrPose world_anchor;
    double world_scale, near_clip_m, far_clip_m;
};
static_assert(sizeof(TdvrTrackedCameraRequest) == 104, "tracked preview request ABI");
// Bounded single-eye diagnostic. No replay, locomotion, or simulation identity.
// Freshness requires views returned by a successful CURRENT world-frame begin.
class TdvrTrackedCameraPreview {
public:
    bool arm(const TdvrTrackedCameraRequest& request, uint64_t now_ms);
    bool take(const TdvrXrViewSnapshot& current, uint64_t now_ms, TdvrEngineCameraMatrices& output);
    uint32_t remaining(uint64_t now_ms) const;
private:
    TdvrTrackedCameraRequest request_{};
    TdvrPose origin_{};
    uint64_t expires_ = 0, last_frame_ = 0;
    int64_t last_time_ = 0;
    uint32_t remaining_ = 0;
    bool calibrated_ = false;
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
