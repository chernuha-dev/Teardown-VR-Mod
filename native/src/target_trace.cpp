// SPDX-License-Identifier: MIT
#include "tdvr_target_trace.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <psapi.h>
#include <array>
#include <mutex>
#include <cstring>

namespace {
using CreateRTV = void (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CopySimple = void (STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopyRanges = void (STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using BindRTV = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
struct Entry { SIZE_T handle = 0; TdvrTargetView view{}; };
struct State {
    std::mutex mutex;
    TdvrTargetTrace report{sizeof(TdvrTargetTrace), 1};
    std::array<Entry, 512> descriptors{};
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    UINT increment = 0;
    uintptr_t base = 0, end = 0;
    bool attempted = false;
    CreateRTV create = nullptr;
    CopySimple simple = nullptr;
    CopyRanges ranges = nullptr;
    BindRTV bind = nullptr;
};
State& state() { static auto* s = new State; return *s; }
void invalidate(D3D12_CPU_DESCRIPTOR_HANDLE start, UINT count) {
    auto& s = state();
    for (auto& entry : s.descriptors) {
        if (entry.handle >= start.ptr && s.increment && (entry.handle-start.ptr)%s.increment == 0
            && (entry.handle-start.ptr)/s.increment < count) entry = {};
    }
}
void STDMETHODCALLTYPE create(ID3D12Device* device, ID3D12Resource* resource,
    const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    auto& s = state();
    s.create(device, resource, desc, destination);
    if (device != s.device.Get()) return;
    TdvrTargetView view{};
    if (resource) {
        const auto rd = resource->GetDesc();
        view.width = rd.Width; view.height = rd.Height;
        view.resource_format = rd.Format; view.view_format = desc ? desc->Format : rd.Format;
        view.dimension = rd.Dimension; view.samples = rd.SampleDesc.Count; view.flags = rd.Flags;
    }
    std::lock_guard<std::mutex> lock(s.mutex);
    view.creation = ++s.report.descriptor_writes;
    Entry* available = nullptr;
    for (auto& entry : s.descriptors) {
        if (entry.handle == destination.ptr) { available = &entry; break; }
        if (!entry.handle && !available) available = &entry;
    }
    if (!available) { ++s.report.dropped; return; }
    *available = {destination.ptr, resource ? view : TdvrTargetView{}};
}
void STDMETHODCALLTYPE simple(ID3D12Device* device, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination,
    D3D12_CPU_DESCRIPTOR_HANDLE source, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    auto& s = state(); s.simple(device, count, destination, source, type);
    if (device != s.device.Get() || type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV) return;
    std::lock_guard<std::mutex> lock(s.mutex);
    invalidate(destination, count); // deliberately unknown; never retain stale destination metadata
}
void STDMETHODCALLTYPE ranges(ID3D12Device* device, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* destination,
    const UINT* sizes, UINT sources, const D3D12_CPU_DESCRIPTOR_HANDLE* source,
    const UINT* source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    auto& s = state(); s.ranges(device, count, destination, sizes, sources, source, source_sizes, type);
    if (device != s.device.Get() || type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV) return;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (count > 512) { s.descriptors = {}; return; }
    for (UINT i = 0; i < count; ++i) invalidate(destination[i], sizes ? sizes[i] : 1);
}
void STDMETHODCALLTYPE bind(ID3D12GraphicsCommandList* list, UINT count,
    const D3D12_CPU_DESCRIPTOR_HANDLE* handles, BOOL contiguous, const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    auto& s = state();
    // Forward first, unchanged; observations never influence the application's command stream.
    s.bind(list, count, handles, contiguous, depth);
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto serial = ++s.report.bindings;
    if (serial > 65536 || (serial > 32 && (serial & 63))) return;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || device.Get() != s.device.Get()) return;
    TdvrTargetBinding record{};
    record.count = count; record.contiguous = contiguous != FALSE;
    if (count > 8 || (count && !handles)) { ++s.report.dropped; return; }
    for (UINT i = 0; i < count; ++i) {
        const auto handle = contiguous ? handles[0].ptr + static_cast<SIZE_T>(i)*s.increment : handles[i].ptr;
        for (const auto& entry : s.descriptors) if (entry.handle == handle && entry.view.creation) {
            record.known_mask |= 1u << i; record.views[i] = entry.view; break;
        }
    }
    void* stack[32]{};
    const auto frames = CaptureStackBackTrace(0, 32, stack, nullptr);
    for (USHORT i = 0; i < frames && record.depth < 12; ++i) {
        const auto address = reinterpret_cast<uintptr_t>(stack[i]);
        if (address >= s.base && address < s.end) record.executable_rvas[record.depth++] = static_cast<uint32_t>(address-s.base);
    }
    // Engines can rewrite the same descriptor every pass. Group by shape/caller,
    // not write serial; creation in the saved record belongs to its FIRST sample.
    auto key = record;
    for (auto& view : key.views) view.creation = 0;
    for (uint32_t i = 0; i < s.report.record_count; ++i) {
        auto previous = s.report.records[i];
        for (auto& view : previous.views) view.creation = 0;
        if (!std::memcmp(&key, &previous, sizeof(key))) return;
    }
    if (s.report.record_count == 32) { ++s.report.dropped; return; }
    s.report.records[s.report.record_count++] = record;
}
template<class T> bool install(void* address, void* detour, T& original) {
    if (MH_CreateHook(address, detour, reinterpret_cast<void**>(&original)) != MH_OK) return false;
    if (MH_EnableHook(address) == MH_OK) return true;
    MH_RemoveHook(address); original = nullptr; return false;
}
}
HRESULT tdvr_trace_targets(ID3D12Device* device, ID3D12GraphicsCommandList* list) noexcept {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.attempted) return s.report.last_hresult;
    s.attempted = true;
    MODULEINFO module{};
    if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &module, sizeof(module)))
        return s.report.last_hresult = HRESULT_FROM_WIN32(GetLastError());
    s.base = reinterpret_cast<uintptr_t>(module.lpBaseOfDll); s.end = s.base+module.SizeOfImage;
    s.device = device; s.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto d = *reinterpret_cast<void***>(device);
    const auto l = *reinterpret_cast<void***>(list);
    if (install(d[20], reinterpret_cast<void*>(create), s.create)) s.report.hook_mask |= 1;
    if (install(d[23], reinterpret_cast<void*>(ranges), s.ranges)) s.report.hook_mask |= 2;
    if (install(d[24], reinterpret_cast<void*>(simple), s.simple)) s.report.hook_mask |= 4;
    if (s.report.hook_mask == 7 && install(l[46], reinterpret_cast<void*>(bind), s.bind)) s.report.hook_mask |= 8;
    return s.report.last_hresult = s.report.hook_mask == 15 ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}
HRESULT WINAPI TdvrGetTargetTrace(TdvrTargetTrace* output) {
    if (!output || output->size != sizeof(*output) || output->abi != 1) return E_INVALIDARG;
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex); *output = s.report; return S_OK;
}
HRESULT WINAPI TdvrResetTargetTrace(void*) {
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.attempted) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    const auto mask = s.report.hook_mask; const auto hr = s.report.last_hresult;
    const auto writes = s.report.descriptor_writes;
    s.report = {sizeof(TdvrTargetTrace), 1}; s.report.hook_mask = mask;
    s.report.last_hresult = hr; s.report.descriptor_writes = writes; return S_OK;
}
