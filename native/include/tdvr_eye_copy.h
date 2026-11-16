#pragma once
// SPDX-License-Identifier: MIT
#include <d3d12.h>

namespace tdvr {
// Record a full, bit-preserving 2D RGBA/BGRA8 copy on an OPEN direct list.
// Exact format/extent, one mip/layer/sample, distinct resources, same device.
// Caller owns truthful StateBefore values and resource lifetimes. This neither
// submits nor waits: caller must fence the actual queue before reuse/XR release.
// All arguments are validated before recording. Both states are restored.
// No scaling, gamma conversion, ownership transfer or implicit GPU state tracking.
HRESULT record_eye_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_before, ID3D12Resource* destination,
    D3D12_RESOURCE_STATES destination_before) noexcept;
}
