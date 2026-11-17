#pragma once
#include <windows.h>
#include <cstdint>
#include "tdvr_tracking.h"
#ifdef TDVR_CAPTURE_BUILD
#define TDVR_CAPTURE_API __declspec(dllexport)
#else
#define TDVR_CAPTURE_API __declspec(dllimport)
#endif

// Own diagnostic ABI, independent of the legacy Lua bridge. XR starts only
// through explicit panel opt-in; reading any stats never starts a runtime.
struct TdvrCaptureStats {
    uint32_t size;
    uint32_t abi;
    uint64_t presents;
    uint64_t associations;
    uint64_t captured_frames;
    uint64_t resizes;
    uint64_t unsupported;
    uint64_t pixel_hash;
    uint32_t width, height;
    int32_t last_hresult;
};
struct TdvrPanelStats {
    uint32_t size, abi, enabled, stage;
    uint64_t frames, submitted;
    int32_t xr_result, gpu_result;
    uint32_t quarantined, reserved;
    uint32_t source_format, swapchain_format, resource_format, color_space, failure_stage;
};
extern "C" {
// Startup on a normal application thread BEFORE creating DXGI factories. No DllMain work.
// Process lifetime only: the module pins itself; unloading hooked code is forbidden.
TDVR_CAPTURE_API HRESULT WINAPI TdvrStartCapture();
// Opt-in before startup. This enables a FLAT quad, not stereo game rendering.
TDVR_CAPTURE_API HRESULT WINAPI TdvrEnableXrPanel();
// Explicit development mode. Requires configured profile; begins each XR frame
// from the audited world-render hook, while Present still submits a flat quad.
TDVR_CAPTURE_API HRESULT WINAPI TdvrEnableXrWorldTiming();
// Optional output is populated under the same lock as begin. S_FALSE clears it;
// only S_OK supplies this begin's observation (available may still be zero).
TDVR_CAPTURE_API HRESULT WINAPI TdvrBeginXrWorldFrame(TdvrXrViewSnapshot* current);
TDVR_CAPTURE_API HRESULT WINAPI TdvrGetPanelStats(TdvrPanelStats* stats);
// Returns only the latest current XR observation. available=0 invalidates old
// poses after no-render/tracking loss/session stop/error. Never starts runtime.
TDVR_CAPTURE_API HRESULT WINAPI TdvrGetXrViews(TdvrXrViewSnapshot* snapshot);
// Optional development profile, configured before startup. Redirects this child's
// AppData/Documents queries and environment; never changes the user's folders.
TDVR_CAPTURE_API HRESULT WINAPI TdvrConfigureDevelopmentProfile(const wchar_t* root);
// Start only folder redirection, without touching DXGI/D3D12 or starting XR.
// Mutually exclusive with capture startup; useful when testing another renderer.
TDVR_CAPTURE_API HRESULT WINAPI TdvrStartDevelopmentProfile();
TDVR_CAPTURE_API HRESULT WINAPI TdvrGetCaptureStats(TdvrCaptureStats* stats);
}
