#pragma once

#include "tdvr_math.h"
#include <cstdint>

// Standalone CPU contract, not the legacy Lua bridge or an OpenXR structure.
// The producer must supply a stable input: this API is not a cross-thread mailbox.
struct TdvrFrameKey {
    std::uint64_t simulation_id;
    std::int64_t predicted_display_time_ns;
};

enum TdvrPoseFlags : std::uint32_t {
    TDVR_ORIENTATION_VALID = 1u,
    TDVR_POSITION_VALID = 2u,
    TDVR_ORIENTATION_TRACKED = 4u,
    TDVR_POSITION_TRACKED = 8u
};

struct TdvrEyeSample {
    TdvrFrameKey frame;
    TdvrPose tracking_pose;
    TdvrFov fov;
    std::uint32_t pose_flags;
};

struct TdvrFrameInput {
    TdvrFrameKey frame;
    // World pose of the tracking origin. For floor-origin tracking this is the
    // floor origin, NOT the existing game eye position plus standing HMD height.
    // Tracking yaw remains in each eye pose; body/recenter yaw belongs here once.
    TdvrPose world_from_tracking;
    double world_scale; // world units per tracking metre; applied once to translation
    double near_clip_m;
    double far_clip_m;
    TdvrEyeSample eyes[2]; // left, right; each includes the runtime eye offset once
};

struct TdvrEyeFrame {
    TdvrPose world_pose;
    TdvrMatrix view;       // world -> eye, inverse rigid transform
    TdvrMatrix projection; // eye -> clip, RH D3D depth [0,1]
};

class TdvrStereoFrame;

// Exact key equality rejects mixed/stale tagged eyes. Keys are otherwise opaque:
// this cannot establish source freshness, freeze simulation, or synchronize a GPU.
// Strict initial policy: both valid AND tracked bits are required for both eyes;
// unknown flag bits are rejected. No inferred pose or old-frame fallback is used.
// Near/far metres are scaled to world units. Quaternions within the math ABI's
// 1e-5 squared-norm tolerance are normalized before rigid composition.
// On failure output is untouched. On success it owns all data, without input refs.
TDVR_API TdvrResult tdvr_build_stereo_frame(const TdvrFrameInput* input,
                                           TdvrStereoFrame* output);

// Read-only packet after construction. Assignment replaces a whole packet; there
// is no per-eye mutation API. Publication between threads remains the caller's job.
class TdvrStereoFrame {
public:
    bool valid() const { return valid_; }
    const TdvrFrameKey& key() const { return frame_; }
    const TdvrPose& world_from_tracking() const { return world_from_tracking_; }
    double world_scale() const { return world_scale_; }
    const TdvrEyeFrame& left_eye() const { return eyes_[0]; }
    const TdvrEyeFrame& right_eye() const { return eyes_[1]; }

private:
    friend TDVR_API TdvrResult tdvr_build_stereo_frame(const TdvrFrameInput*, TdvrStereoFrame*);
    bool valid_ = false;
    TdvrFrameKey frame_{};
    TdvrPose world_from_tracking_{};
    double world_scale_ = 0;
    TdvrEyeFrame eyes_[2]{};
};
