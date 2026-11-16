#pragma once
// SPDX-License-Identifier: MIT
#include "tdvr_capture.h"

enum TdvrRenderCommand : uint32_t { TDVR_DRAW = 1, TDVR_DRAW_INDEXED = 2, TDVR_DISPATCH = 3 };
struct TdvrRenderStack {
    uint32_t command, depth;
    uint64_t observations, first_present;
    uint32_t executable_rvas[24]; // return addresses, ASLR independent; no other modules/paths
};
struct TdvrRenderTrace {
    uint32_t size, abi;
    uint64_t commands, stack_samples, dropped;
    uint32_t hook_mask, record_count; // mask bits: Draw=1, DrawIndexed=2, Dispatch=4
    int32_t last_hresult;
    uint32_t exhausted;
    uint64_t generation;
    TdvrRenderStack records[64];
};
extern "C" {
// Opt in BEFORE capture startup. No graphics device is created until DXGI gives
// us the child's actual queue. No XR calls. These hooks only forward commands.
TDVR_CAPTURE_API HRESULT WINAPI TdvrEnableRenderTrace(void* unused);
// Rearm a bounded sample window (first 64 commands, then every 64th, at most
// 65536 commands). Call between observations to inspect a later gameplay scene.
TDVR_CAPTURE_API HRESULT WINAPI TdvrResetRenderTrace(void* unused);
TDVR_CAPTURE_API HRESULT WINAPI TdvrGetRenderTrace(TdvrRenderTrace* output);
}

#ifdef TDVR_CAPTURE_BUILD
struct ID3D12CommandQueue;
HRESULT tdvr_trace_queue(ID3D12CommandQueue* queue) noexcept;
void tdvr_trace_present() noexcept;
#endif
