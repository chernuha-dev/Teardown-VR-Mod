#pragma once

#ifdef _WIN32
#ifdef TDVR_BUILD
#define TDVR_API __declspec(dllexport)
#else
#define TDVR_API __declspec(dllimport)
#endif
#else
#define TDVR_API
#endif

// Right handed: +X right, +Y up, forward -Z; position in metres, quaternion xyzw.
// This is a standalone math ABI, NOT the v0.22 Lua memory bridge or a DXGI proxy.
struct TdvrPose { double position[3]; double orientation[4]; };
struct TdvrFov { double left, right, up, down; }; // signed angles in radians
struct TdvrMatrix { double m[16]; }; // row major, multiplies a column vector
enum TdvrResult { TDVR_OK = 0, TDVR_INVALID_ARGUMENT = 1 };

extern "C" {
// Scale only tracking translation, then compose base * tracking. Output untouched on failure.
TDVR_API TdvrResult tdvr_pose_to_world(const TdvrPose* base, const TdvrPose* tracking,
                                      double world_scale, TdvrPose* output);
// Asymmetric right handed D3D projection: -Z forward, depth [0,1], finite near/far.
// No monitor/window aspect ratio participates. No renderer or OpenXR session is created.
TDVR_API TdvrResult tdvr_projection(const TdvrFov* fov, double near_z, double far_z,
                                   TdvrMatrix* output);
}
