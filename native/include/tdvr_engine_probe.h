#pragma once
#include <windows.h>
#include <cstdint>
#include "tdvr_camera_override.h"
#include "tdvr_temporal_observer.h"
#ifdef TDVR_ENGINE_BUILD
#define TDVR_ENGINE_API __declspec(dllexport)
#else
#define TDVR_ENGINE_API __declspec(dllimport)
#endif
struct TdvrEngineStats {
    uint32_t size, abi;
    uint64_t camera_calls, valid_samples, rejected_samples;
    double position[3], orientation[4];
    double maximum_view_error;
    float projection[16];
    uint32_t identity_matched, signature_matched, hook_installed;
};
struct TdvrWorldRenderStats {
    uint32_t size, abi;
    uint64_t calls, completed_calls, valid_samples, invalid_samples;
    uint64_t main_camera_matches, camera_mutations, serial_steps, history_swaps;
    uint32_t first_thread, thread_changes;
    uint32_t signature_matched, hook_installed;
    float camera[34]; // projection/view/near/far from the last valid input; no pointers
};
struct TdvrCameraOverrideStats {
    uint32_t size, abi;
    uint64_t accepted_requests, applied_calls, restored_calls, rejected_calls;
    uint32_t remaining_calls, reserved;
    float last_camera[34];
};
struct TdvrEngineResourceInfo {
    uint64_t width;
    uint32_t height, format, flags, samples, dimension, mips, layers, reserved;
};
struct TdvrEngineTargetStats {
    uint32_t size, abi;
    uint64_t samples, valid_samples, rejected_samples;
    uint32_t width, height, count, game_state, known_mask, matching_extent_mask;
    TdvrEngineResourceInfo colors[8]; // GetDesc metadata only; no exported pointers/ownership
};
static_assert(sizeof(TdvrEngineResourceInfo) == 40 && sizeof(TdvrEngineTargetStats) == 376, "target diagnostic ABI");
struct TdvrEyeTargetProbeStats {
    uint32_t size, abi, phase, created, distinct_resources, failure;
    TdvrEngineTargetStats eyes[2];
};
static_assert(sizeof(TdvrEyeTargetProbeStats) == 776);
struct TdvrEyeHistoryProbeStats {
    uint32_t size, abi, phase, created, failure, distinct_resources;
    uint64_t required_bytes, budget_bytes, usage_bytes;
    TdvrEngineTargetStats targets[2][10];
};
static_assert(sizeof(TdvrEyeHistoryProbeStats) == 7568);
struct TdvrEyeTaaProbeStats {
    uint32_t size, abi, phase, created, failure, distinct_resources;
    uint64_t required_bytes, budget_bytes, usage_bytes;
    TdvrEngineResourceInfo textures[2][5];
};
static_assert(sizeof(TdvrEyeTaaProbeStats) == 448);
extern "C" {
TDVR_ENGINE_API HRESULT WINAPI TdvrStartEngineProbe(void* unused);
TDVR_ENGINE_API HRESULT WINAPI TdvrGetEngineStats(TdvrEngineStats* stats);
TDVR_ENGINE_API HRESULT WINAPI TdvrGetWorldRenderStats(TdvrWorldRenderStats* stats);
TDVR_ENGINE_API HRESULT WINAPI TdvrGetEngineTargetStats(TdvrEngineTargetStats* stats);
TDVR_ENGINE_API HRESULT WINAPI TdvrGetTemporalStats(TdvrTemporalStats* stats);
// One pair of 64x64 engine targets, allocated on the audited render thread.
// Never installed into scene slots or rendered; retained until process exit.
TDVR_ENGINE_API HRESULT WINAPI TdvrRequestEyeTargetProbe(void* unused);
TDVR_ENGINE_API HRESULT WINAPI TdvrGetEyeTargetProbeStats(TdvrEyeTargetProbeStats* stats);
TDVR_ENGINE_API HRESULT WINAPI TdvrRequestEyeHistoryProbe(void* unused);
TDVR_ENGINE_API HRESULT WINAPI TdvrGetEyeHistoryProbeStats(TdvrEyeHistoryProbeStats* stats);
TDVR_ENGINE_API HRESULT WINAPI TdvrRequestEyeTaaProbe(void* unused);
TDVR_ENGINE_API HRESULT WINAPI TdvrGetEyeTaaProbeStats(TdvrEyeTaaProbeStats* stats);
// Development-only, bounded synthetic camera input. Zero render_calls cancels.
// Requires a valid audited main-camera observation first. No simulation replay.
TDVR_ENGINE_API HRESULT WINAPI TdvrSubmitCameraOverride(const TdvrCameraOverrideRequest* request);
TDVR_ENGINE_API HRESULT WINAPI TdvrSubmitTrackedCameraPreview(const TdvrTrackedCameraRequest* request);
TDVR_ENGINE_API HRESULT WINAPI TdvrGetCameraOverrideStats(TdvrCameraOverrideStats* stats);
}
