#pragma once
#include <windows.h>
#include <cstdint>
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
extern "C" {
TDVR_ENGINE_API HRESULT WINAPI TdvrStartEngineProbe(void* unused);
TDVR_ENGINE_API HRESULT WINAPI TdvrGetEngineStats(TdvrEngineStats* stats);
TDVR_ENGINE_API HRESULT WINAPI TdvrGetWorldRenderStats(TdvrWorldRenderStats* stats);
}
