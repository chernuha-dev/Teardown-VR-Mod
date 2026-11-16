#pragma once
// SPDX-License-Identifier: MIT
#include <array>
#include <cstddef>
#include <cstdint>

struct TdvrTemporalState {
    uint64_t serial;
    uint32_t history[2], reset_flag, taa_word;
    uint32_t scene_slot_mask, scene_descriptor_mask, taa_slot_mask, taa_presence_mask;
    uint32_t taa_word_readable, reserved;
    uint64_t descriptor_hashes[10]; // fingerprints only; no native pointers
};
static_assert(sizeof(TdvrTemporalState) == 128);
struct TdvrTemporalStats {
    uint32_t size, abi;
    uint64_t samples, rejected;
    uint32_t scene_changed_mask, taa_changed_mask, descriptor_changed_mask, last_sample_valid;
    TdvrTemporalState before, after; // meaningful only if last_sample_valid=1
};
static_assert(sizeof(TdvrTemporalStats) == 296);

// Internal observations retain addresses only for comparison, never export them.
struct TdvrTemporalObservation {
    TdvrTemporalState state{};
    std::array<uint64_t,10> scene_buffers{};
    std::array<uint64_t,5> taa_resources{};
    std::array<std::array<uint8_t,0x38>,10> descriptors{};
};
using TdvrTemporalRead = bool (*)(uint64_t address, void* out, size_t bytes, void* context);
// Callback must copy bytes safely; this helper never dereferences an address,
// writes game memory, calls COM, allocates resources, or assumes GPU ownership.
// False if basic serial/history/reset cannot be read; output stays untouched.
bool tdvr_observe_temporal(uint64_t scene, TdvrTemporalRead read, void* context, TdvrTemporalObservation& output);
void tdvr_compare_temporal(const TdvrTemporalObservation& before, const TdvrTemporalObservation& after,
    uint32_t& scene_changed, uint32_t& taa_changed, uint32_t& descriptor_changed);
