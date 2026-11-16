// SPDX-License-Identifier: MIT
#include "tdvr_render_trace.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <psapi.h>
#include <atomic>
#include <cstring>
#include <mutex>

namespace {
using Microsoft::WRL::ComPtr;
using Draw = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawIndexed = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using Dispatch = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
struct TraceState {
    std::mutex mutex;
    std::atomic<bool> enabled{false};
    std::atomic<uint64_t> commands{0}, presents{0};
    std::atomic<uint64_t> generation{0};
    TdvrRenderTrace report{sizeof(TdvrRenderTrace), 1};
    uintptr_t base = 0, end = 0;
    bool attempted = false;
    Draw draw = nullptr;
    DrawIndexed indexed = nullptr;
    Dispatch dispatch = nullptr;
};
TraceState& trace() { static auto* s = new TraceState; return *s; }
void record(uint32_t kind) noexcept {
    auto& s = trace();
    if (!s.enabled.load(std::memory_order_relaxed)) return;
    const auto generation = s.generation.load(std::memory_order_relaxed);
    const auto count = s.commands.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 65536 || (count > 64 && (count & 63))) return;
    void* stack[32]{};
    const auto depth = CaptureStackBackTrace(0, 32, stack, nullptr);
    TdvrRenderStack sample{};
    sample.command = kind;
    sample.first_present = s.presents.load(std::memory_order_relaxed);
    for (USHORT i = 0; i < depth && sample.depth < 24; ++i) {
        const auto address = reinterpret_cast<uintptr_t>(stack[i]);
        if (address >= s.base && address < s.end)
            sample.executable_rvas[sample.depth++] = static_cast<uint32_t>(address - s.base);
    }
    std::lock_guard<std::mutex> lock(s.mutex);
    if (generation != s.generation.load(std::memory_order_relaxed)) return;
    ++s.report.stack_samples;
    if (!sample.depth) { ++s.report.dropped; return; }
    for (uint32_t i = 0; i < s.report.record_count; ++i) {
        auto& entry = s.report.records[i];
        if (entry.command == kind && entry.depth == sample.depth
            && std::memcmp(entry.executable_rvas, sample.executable_rvas, sizeof(sample.executable_rvas)) == 0) {
            ++entry.observations; return;
        }
    }
    if (s.report.record_count == 64) { ++s.report.dropped; return; }
    sample.observations = 1;
    s.report.records[s.report.record_count++] = sample;
}
void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList* list, UINT a, UINT b, UINT c, UINT d) {
    record(TDVR_DRAW); trace().draw(list, a, b, c, d);
}
void STDMETHODCALLTYPE indexed(ID3D12GraphicsCommandList* list, UINT a, UINT b, UINT c, INT d, UINT e) {
    record(TDVR_DRAW_INDEXED); trace().indexed(list, a, b, c, d, e);
}
void STDMETHODCALLTYPE dispatch(ID3D12GraphicsCommandList* list, UINT a, UINT b, UINT c) {
    record(TDVR_DISPATCH); trace().dispatch(list, a, b, c);
}
template<class T> bool install(void* target, void* detour, T& original) {
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(&original)) != MH_OK) return false;
    if (MH_EnableHook(target) == MH_OK) return true;
    MH_RemoveHook(target); original = nullptr; return false;
}
}
HRESULT WINAPI TdvrEnableRenderTrace(void*) {
    auto& s = trace();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.attempted) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    MODULEINFO module{};
    if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &module, sizeof(module)))
        return HRESULT_FROM_WIN32(GetLastError());
    s.base = reinterpret_cast<uintptr_t>(module.lpBaseOfDll);
    s.end = s.base + module.SizeOfImage;
    s.enabled.store(true, std::memory_order_release);
    return S_OK;
}
HRESULT WINAPI TdvrResetRenderTrace(void*) {
    auto& s = trace();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled.load(std::memory_order_acquire)) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    const auto mask = s.report.hook_mask;
    const auto hr = s.report.last_hresult;
    s.report = TdvrRenderTrace{sizeof(TdvrRenderTrace), 1};
    s.report.hook_mask = mask; s.report.last_hresult = hr;
    s.report.generation = s.generation.fetch_add(1, std::memory_order_relaxed) + 1;
    s.commands.store(0, std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI TdvrGetRenderTrace(TdvrRenderTrace* output) {
    if (!output || output->size != sizeof(*output) || output->abi != 1) return E_INVALIDARG;
    auto& s = trace();
    std::lock_guard<std::mutex> lock(s.mutex);
    *output = s.report;
    output->commands = s.commands.load(std::memory_order_relaxed);
    output->exhausted = output->commands >= 65536;
    return S_OK;
}
void tdvr_trace_present() noexcept {
    auto& s = trace();
    if (s.enabled.load(std::memory_order_relaxed)) s.presents.fetch_add(1, std::memory_order_relaxed);
}
HRESULT tdvr_trace_queue(ID3D12CommandQueue* queue) noexcept {
    auto& s = trace();
    if (!s.enabled.load(std::memory_order_acquire)) return S_FALSE;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.attempted) return s.report.last_hresult;
    s.attempted = true;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&device));
    if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        hr = list->Close(); // never submitted; used only to obtain actual backend methods
        if (SUCCEEDED(hr)) {
            const auto table = *reinterpret_cast<void***>(list.Get());
            if (install(table[12], reinterpret_cast<void*>(draw), s.draw)) s.report.hook_mask |= 1;
            if (install(table[13], reinterpret_cast<void*>(indexed), s.indexed)) s.report.hook_mask |= 2;
            if (install(table[14], reinterpret_cast<void*>(dispatch), s.dispatch)) s.report.hook_mask |= 4;
            if (s.report.hook_mask != 7) hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
    }
    s.report.last_hresult = hr;
    return hr;
}
