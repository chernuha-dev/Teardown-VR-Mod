#pragma once
// SPDX-License-Identifier: MIT
#include <array>
#include <cstddef>
#include <cstdint>

// Audited render-only CPU fields. Addresses are private native ownership data,
// never an RPC ABI. Does not claim these are all simulation or GPU state.
struct TdvrTemporalCpuState {
    uint64_t serial{};
    uint32_t history[2]{}, taa_counter{};
    uint8_t reset{};
    std::array<uint8_t,0x194> camera_cache{}; // scene [0x2218,0x23ac)
    std::array<uint8_t,8> taa_jitter{};       // TAA [0x18,0x20)
    std::array<uint64_t,10> targets{};
    std::array<uint64_t,5> taa_textures{};
};
struct TdvrTemporalMemory {
    uint8_t* scene; size_t scene_size;
    uint8_t* taa; size_t taa_size;
};
// Caller must guarantee both spans are readable/writable for the whole scope,
// exclusively owned by the audited render thread, and resources remain alive.
// No OS access, pointer dereference of resources, GPU calls, or allocation here.
bool tdvr_capture_temporal_cpu(const TdvrTemporalMemory& memory, TdvrTemporalCpuState& output) noexcept;
bool tdvr_same_temporal_cpu(const TdvrTemporalCpuState& a,const TdvrTemporalCpuState& b) noexcept;

class TdvrTemporalScope {
public:
    TdvrTemporalScope(const TdvrTemporalMemory& memory, TdvrTemporalCpuState& bank, bool& occupied) noexcept;
    ~TdvrTemporalScope();
    bool active() const noexcept { return active_; }
    // Publish post-render CPU state into this bank, then restore the caller.
    // Destructor without finish restores caller without publishing partial work.
    bool finish() noexcept;
    TdvrTemporalScope(const TdvrTemporalScope&) = delete;
    TdvrTemporalScope& operator=(const TdvrTemporalScope&) = delete;
private:
    TdvrTemporalMemory memory_;
    TdvrTemporalCpuState& bank_;
    TdvrTemporalCpuState original_{};
    bool& occupied_;
    bool active_ = false;
    void restore() noexcept;
};
