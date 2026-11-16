// SPDX-License-Identifier: MIT
#include "tdvr_eye_history.h"
#include <wrl/client.h>
#include <array>
#include <limits>
#include <stdexcept>

namespace tdvr {
using Microsoft::WRL::ComPtr;
struct EyeHistoryResources::Impl {
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence> fence;
    std::array<std::array<ComPtr<ID3D12Resource>,10>,2> textures;
    UINT count = 0;
    uint64_t sequence = 0;
    bool open = false, terminal = false;
    explicit Impl(ID3D12CommandQueue* value) : queue(value) {
        if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT || queue->GetDesc().NodeMask > 1
            || FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))
            || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))))
            throw std::invalid_argument("eye history requires a live node-0 DIRECT queue");
    }
    HRESULT idle() noexcept {
        if (terminal) return E_FAIL;
        if (open) return E_PENDING;
        const auto done = fence->GetCompletedValue();
        if (done == std::numeric_limits<uint64_t>::max() || FAILED(device->GetDeviceRemovedReason())) {
            terminal = true; return DXGI_ERROR_DEVICE_REMOVED;
        }
        return done >= sequence ? S_OK : DXGI_ERROR_WAS_STILL_DRAWING;
    }
};
EyeHistoryResources::EyeHistoryResources(ID3D12CommandQueue* queue) : impl_(std::make_unique<Impl>(queue)) {}
EyeHistoryResources::~EyeHistoryResources() {
    // Pending use may still reference any allocation. Keep queue/fence/resources
    // together until process exit; never remove a game-owned device to clean up.
    if (impl_->idle() != S_OK) impl_.release();
}
bool EyeHistoryResources::quarantined() const noexcept { return impl_->terminal; }
HRESULT EyeHistoryResources::rebuild(const D3D12_RESOURCE_DESC* descriptions, UINT count) noexcept {
    auto& p = *impl_;
    const auto ready = p.idle(); if (ready != S_OK) return ready;
    if (!descriptions || !count || count > 10) return E_INVALIDARG;
    for (UINT i = 0; i < count; ++i) {
        const auto& d = descriptions[i];
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !d.Width || d.Width > 16384 || !d.Height || d.Height > 16384
            || d.DepthOrArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1 || d.SampleDesc.Quality
            || d.Format == DXGI_FORMAT_UNKNOWN || d.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN || d.Alignment) return E_INVALIDARG;
    }
    std::array<std::array<ComPtr<ID3D12Resource>,10>,2> next;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    for (UINT eye = 0; eye < 2; ++eye) for (UINT i = 0; i < count; ++i) {
        const auto hr = p.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&descriptions[i],
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&next[eye][i]));
        if (FAILED(hr)) return hr; // previous complete bank remains intact
    }
    p.textures.swap(next); p.count = count;
    return S_OK;
}
HRESULT EyeHistoryResources::begin_use(EyeHistoryView* output) noexcept {
    auto& p = *impl_;
    const auto ready = p.idle(); if (ready != S_OK) return ready;
    if (!output || !p.count) return E_INVALIDARG;
    EyeHistoryView view; view.count = p.count;
    for (UINT eye = 0; eye < 2; ++eye) for (UINT i = 0; i < p.count; ++i) view.textures[eye][i] = p.textures[eye][i].Get();
    *output = view; p.open = true;
    return S_OK;
}
HRESULT EyeHistoryResources::seal_use() noexcept {
    auto& p = *impl_;
    if (p.terminal || !p.open) return E_UNEXPECTED;
    if (p.sequence == std::numeric_limits<uint64_t>::max()-1) { p.terminal = true; return E_FAIL; }
    const auto hr = p.queue->Signal(p.fence.Get(),++p.sequence);
    if (FAILED(hr)) { p.terminal = true; return hr; }
    p.open = false;
    return S_OK;
}
}
