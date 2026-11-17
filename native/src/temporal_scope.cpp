// SPDX-License-Identifier: MIT
#include "tdvr_temporal_scope.h"
#include <cstring>
#include <limits>

namespace {
constexpr size_t fields[10] = {0xa50,0xa58,0xaa8,0xab0,0xac0,0xac8,0xae0,0xae8,0xaf8,0xb00};
bool valid(const TdvrTemporalMemory& memory) noexcept {
    if (!memory.scene || !memory.taa || memory.scene_size < 0x23f2 || memory.taa_size < 0x78) return false;
    const auto scene = reinterpret_cast<uintptr_t>(memory.scene), taa = reinterpret_cast<uintptr_t>(memory.taa);
    const auto maximum = std::numeric_limits<uintptr_t>::max();
    if (scene > maximum-0x23f2 || taa > maximum-0x78) return false;
    return scene+0x23f2 <= taa || taa+0x78 <= scene;
}
bool resources_valid(const TdvrTemporalCpuState& bank) noexcept {
    uint64_t pointers[15]{};
    for (unsigned i = 0; i < 10; ++i) pointers[i] = bank.targets[i];
    for (unsigned i = 0; i < 5; ++i) pointers[10+i] = bank.taa_textures[i];
    for (unsigned i = 0; i < 15; ++i) {
        if (!pointers[i]) return false;
        for (unsigned j = 0; j < i; ++j) if (pointers[i] == pointers[j]) return false;
    }
    return bank.reset <= 1 && bank.history[0] <= 1 && bank.history[1] <= 1 && bank.history[0] != bank.history[1];
}
void write(const TdvrTemporalMemory& m, const TdvrTemporalCpuState& state) noexcept {
    std::memcpy(m.scene+0x2208,&state.serial,sizeof(state.serial));
    std::memcpy(m.scene+0x2210,state.history,sizeof(state.history));
    std::memcpy(m.scene+0x2218,state.camera_cache.data(),state.camera_cache.size());
    std::memcpy(m.scene+0x23f1,&state.reset,sizeof(state.reset));
    std::memcpy(m.taa,&state.taa_counter,sizeof(state.taa_counter));
    std::memcpy(m.taa+0x18,state.taa_jitter.data(),state.taa_jitter.size());
    for (unsigned i = 0; i < 10; ++i) std::memcpy(m.scene+fields[i],&state.targets[i],sizeof(uint64_t));
    std::memcpy(m.taa+0x50,state.taa_textures.data(),sizeof(uint64_t)*5);
}
}
bool tdvr_capture_temporal_cpu(const TdvrTemporalMemory& m, TdvrTemporalCpuState& output) noexcept {
    if (!valid(m)) return false;
    TdvrTemporalCpuState state;
    std::memcpy(&state.serial,m.scene+0x2208,sizeof(state.serial));
    std::memcpy(state.history,m.scene+0x2210,sizeof(state.history));
    std::memcpy(state.camera_cache.data(),m.scene+0x2218,state.camera_cache.size());
    std::memcpy(&state.reset,m.scene+0x23f1,sizeof(state.reset));
    std::memcpy(&state.taa_counter,m.taa,sizeof(state.taa_counter));
    std::memcpy(state.taa_jitter.data(),m.taa+0x18,state.taa_jitter.size());
    for (unsigned i = 0; i < 10; ++i) std::memcpy(&state.targets[i],m.scene+fields[i],sizeof(uint64_t));
    std::memcpy(state.taa_textures.data(),m.taa+0x50,sizeof(uint64_t)*5);
    output = state; return true;
}
TdvrTemporalScope::TdvrTemporalScope(const TdvrTemporalMemory& memory, TdvrTemporalCpuState& bank, bool& occupied) noexcept
    : memory_(memory),bank_(bank),occupied_(occupied) {
    if (occupied_ || !resources_valid(bank_) || !tdvr_capture_temporal_cpu(memory_,original_)) return;
    occupied_ = true; active_ = true; write(memory_,bank_);
}
bool tdvr_same_temporal_cpu(const TdvrTemporalCpuState& a,const TdvrTemporalCpuState& b) noexcept {
    return a.serial == b.serial && a.history[0] == b.history[0] && a.history[1] == b.history[1]
        && a.taa_counter == b.taa_counter && a.reset == b.reset && a.camera_cache == b.camera_cache
        && a.taa_jitter == b.taa_jitter && a.targets == b.targets && a.taa_textures == b.taa_textures;
}
void TdvrTemporalScope::restore() noexcept {
    if (!active_) return;
    write(memory_,original_); active_ = false; occupied_ = false;
}
TdvrTemporalScope::~TdvrTemporalScope() { restore(); }
bool TdvrTemporalScope::finish() noexcept {
    if (!active_) return false;
    TdvrTemporalCpuState result;
    const bool captured = tdvr_capture_temporal_cpu(memory_,result) && resources_valid(result)
        && result.targets == bank_.targets && result.taa_textures == bank_.taa_textures;
    restore();
    if (captured) bank_ = result;
    return captured;
}
