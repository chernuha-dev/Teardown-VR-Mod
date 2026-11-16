#pragma once
// SPDX-License-Identifier: MIT
#include "tdvr_capture.h"

struct TdvrTargetView {
    uint64_t creation; // descriptor write serial, NOT a resource lifetime/ownership token
    uint64_t width;
    // Full resource dimensions, not mip/viewport extent. view_format is the raw
    // descriptor field (0 means inherit resource format), or resource format for null desc.
    uint32_t height, resource_format, view_format, dimension, samples, flags;
};
struct TdvrTargetBinding {
    uint32_t count, contiguous, depth, known_mask;
    uint32_t executable_rvas[12];
    TdvrTargetView views[8];
};
struct TdvrTargetTrace {
    uint32_t size, abi;
    uint64_t bindings, descriptor_writes, dropped;
    uint32_t hook_mask, record_count;
    int32_t last_hresult;
    uint32_t reserved;
    TdvrTargetBinding records[32];
};
extern "C" {
TDVR_CAPTURE_API HRESULT WINAPI TdvrGetTargetTrace(TdvrTargetTrace* output);
TDVR_CAPTURE_API HRESULT WINAPI TdvrResetTargetTrace(void* unused);
}
#ifdef TDVR_CAPTURE_BUILD
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
// Opt-in through existing recording trace. Never creates/submits GPU work.
HRESULT tdvr_trace_targets(ID3D12Device*, ID3D12GraphicsCommandList*) noexcept;
#endif
