#pragma once
#include <windows.h>
#include <cstdint>
#ifdef TDVR_CAPTURE_BUILD
#define TDVR_CAPTURE_API __declspec(dllexport)
#else
#define TDVR_CAPTURE_API __declspec(dllimport)
#endif

// Standalone diagnostic ABI. No legacy Lua bridge, runtime, camera or engine hooks.
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
extern "C" {
// Startup on a normal application thread BEFORE creating DXGI factories. No DllMain work.
// Process lifetime only: the module pins itself; unloading hooked code is forbidden.
TDVR_CAPTURE_API HRESULT WINAPI TdvrStartCapture();
// Optional development profile, configured before startup. Redirects this child's
// AppData/Documents queries and environment; never changes the user's folders.
TDVR_CAPTURE_API HRESULT WINAPI TdvrConfigureDevelopmentProfile(const wchar_t* root);
TDVR_CAPTURE_API HRESULT WINAPI TdvrGetCaptureStats(TdvrCaptureStats* stats);
}
