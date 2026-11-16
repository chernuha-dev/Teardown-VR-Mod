#pragma once
// SPDX-License-Identifier: MIT
#include "tdvr_frame.h"
#include <cstddef>

// Observed Teardown camera record. The engine
// stores projection at +0 and world-to-eye view at +0x40, both column-major.
// Near/far fields at +0x80/+0x84 are written by the engine projection setter.
// Other renderer/frustum caches outside this record are not described here.
struct TdvrEngineCameraMatrices {
    float projection[16];
    float view[16];
    float near_clip, far_clip;
};
static_assert(sizeof(TdvrEngineCameraMatrices) == 0x88);
static_assert(offsetof(TdvrEngineCameraMatrices, view) == 0x40);
static_assert(offsetof(TdvrEngineCameraMatrices, near_clip) == 0x80);
static_assert(offsetof(TdvrEngineCameraMatrices, far_clip) == 0x84);

struct TdvrEngineCameraPair {
    TdvrFrameKey frame;
    TdvrEngineCameraMatrices eyes[2];
};

// Convert BOTH eyes before exposing either one. Finite doubles outside float
// range and degenerate projections after rounding are refused. Output is left
// unchanged on failure. This does not select a game camera or refresh its caches.
TdvrResult tdvr_prepare_engine_cameras(const TdvrStereoFrame& frame,
                                      TdvrEngineCameraPair* output);

// A render-boundary building block, not a hook. The caller must supply an audited,
// exclusively owned camera record and keep all simulation/camera-cache updates
// outside this scope. Restores exactly the 136 bytes it changes, also on unwinding.
// The same camera must not have overlapping/nested scopes. No heap/OS operations.
class TdvrEngineCameraScope {
public:
    TdvrEngineCameraScope(TdvrEngineCameraMatrices& camera,
                          const TdvrEngineCameraMatrices& eye) noexcept;
    ~TdvrEngineCameraScope();
    TdvrEngineCameraScope(const TdvrEngineCameraScope&) = delete;
    TdvrEngineCameraScope& operator=(const TdvrEngineCameraScope&) = delete;
    TdvrEngineCameraScope(TdvrEngineCameraScope&&) = delete;
    TdvrEngineCameraScope& operator=(TdvrEngineCameraScope&&) = delete;
private:
    TdvrEngineCameraMatrices& camera_;
    TdvrEngineCameraMatrices original_;
};
