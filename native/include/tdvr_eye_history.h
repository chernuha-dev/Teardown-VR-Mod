#pragma once
// SPDX-License-Identifier: MIT
#include <d3d12.h>
#include <memory>
#include <cstdint>

namespace tdvr {
struct EyeHistoryView {
    UINT count = 0;
    ID3D12Resource* textures[2][10]{}; // borrowed only for this begin_use/seal_use scope
};
// Own GPU storage, NOT engine wrappers or a state tracker. Caller serializes CPU
// access and submits ALL uses to the supplied DIRECT queue before seal_use().
// Fresh resources start COMMON and have undefined contents. Engine TAA reset /
// initialization, descriptors/views and CPU temporal state belong to the caller.
class EyeHistoryResources {
public:
    explicit EyeHistoryResources(ID3D12CommandQueue* queue);
    ~EyeHistoryResources();
    EyeHistoryResources(const EyeHistoryResources&) = delete;
    EyeHistoryResources& operator=(const EyeHistoryResources&) = delete;
    // Atomic allocation of two independent sets. No replacement while a use is
    // open or its queue fence is pending. No waits, scaling or quality changes.
    HRESULT rebuild(const D3D12_RESOURCE_DESC* descriptions, UINT count) noexcept;
    HRESULT begin_use(EyeHistoryView* output) noexcept;
    // Signal OUR fence on the actual queue, after caller's GPU submissions.
    // Signal failure quarantines the bank; no RemoveDevice or unsafe release.
    HRESULT seal_use() noexcept;
    bool quarantined() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
