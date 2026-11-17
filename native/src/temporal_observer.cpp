// SPDX-License-Identifier: MIT
#include "tdvr_temporal_observer.h"
#include <cstring>
#include <limits>

bool tdvr_parse_taa_texture(const void* bytes, size_t size, TdvrTaaTextureLayout& output) {
    if (!bytes || size != 0x1c) return false;
    const auto* p = static_cast<const uint8_t*>(bytes);
    auto word = [&](size_t offset) -> uint32_t {
        return uint32_t(p[offset]) | (uint32_t(p[offset+1]) << 8) | (uint32_t(p[offset+2]) << 16) | (uint32_t(p[offset+3]) << 24);
    };
    const TdvrTaaTextureLayout result{word(4),word(8),word(0x14)};
    if (p[0] != 0 || !result.width || result.width > 16384 || !result.height || result.height > 16384
        || word(0xc) != 1 || word(0x10) != 1 || p[0x1a] != 1
        || (result.engine_format != 0x12 && result.engine_format != 0x26)) return false;
    output = result; return true;
}

bool tdvr_parse_temporal_target(const void* bytes, size_t size, TdvrTemporalTargetLayout& output) {
    if (!bytes || size != 0x38) return false;
    const auto* p = static_cast<const uint8_t*>(bytes);
    auto word = [&](size_t offset) -> uint32_t {
        return uint32_t(p[offset]) | (uint32_t(p[offset+1]) << 8) | (uint32_t(p[offset+2]) << 16) | (uint32_t(p[offset+3]) << 24);
    };
    TdvrTemporalTargetLayout result{};
    result.width = word(0); result.height = word(4); result.color_count = word(0x30);
    result.depth_enabled = p[0x34] ? 1u : 0u;
    result.depth_variant = p[0x35]; result.flag36 = p[0x36]; result.flag37 = p[0x37];
    if (!result.width || result.width > 16384 || !result.height || result.height > 16384
        || result.color_count > 8 || (!result.color_count && !result.depth_enabled)) return false;
    for (uint32_t i = 0; i < result.color_count; ++i) result.color_format_codes[i] = word(0xc+4*i);
    output = result;
    return true;
}

bool tdvr_observe_temporal(uint64_t scene, TdvrTemporalRead read, void* context, TdvrTemporalObservation& output) {
    if (!read || !scene) return false;
    auto field = [&](uint64_t base, uint64_t offset, void* out, size_t size) {
        const auto maximum = std::numeric_limits<uint64_t>::max();
        if (!base || !size || offset > maximum-base || base+offset > maximum-(size-1)) return false;
        return read(base+offset,out,size,context);
    };
    TdvrTemporalObservation result;
    auto& s = result.state;
    uint8_t reset = 0;
    if (!field(scene,0x2208,&s.serial,sizeof(s.serial)) || !field(scene,0x2210,s.history,sizeof(s.history))
        || !field(scene,0x23f1,&reset,sizeof(reset))) return false;
    s.reset_flag = reset;
    constexpr uint64_t fields[] = {0xa50,0xa58,0xaa8,0xab0,0xac0,0xac8,0xae0,0xae8,0xaf8,0xb00};
    for (unsigned i = 0; i < 10; ++i) {
        auto& pointer = result.scene_buffers[i];
        if (!field(scene,fields[i],&pointer,sizeof(pointer))) continue;
        s.scene_slot_mask |= 1u << i;
        auto& descriptor = result.descriptors[i];
        if (!field(pointer,0,descriptor.data(),descriptor.size())) continue;
        s.scene_descriptor_mask |= 1u << i;
        if (tdvr_parse_temporal_target(descriptor.data(),descriptor.size(),result.targets[i])) s.target_layout_mask |= 1u << i;
        uint64_t hash = 14695981039346656037ull;
        for (const auto value : descriptor) { hash ^= value; hash *= 1099511628211ull; }
        s.descriptor_hashes[i] = hash;
    }
    uint64_t taa = 0;
    if (field(scene,0xa30,&taa,sizeof(taa)) && taa) {
        s.taa_word_readable = field(taa,0,&s.taa_word,sizeof(s.taa_word)) ? 1u : 0u;
        for (unsigned i = 0; i < 5; ++i) {
            auto& pointer = result.taa_resources[i];
            if (!field(taa,0x50+8*i,&pointer,sizeof(pointer))) continue;
            s.taa_slot_mask |= 1u << i;
            if (pointer) s.taa_presence_mask |= 1u << i;
        }
    }
    output = result;
    return true;
}
void tdvr_compare_temporal(const TdvrTemporalObservation& before, const TdvrTemporalObservation& after,
    uint32_t& scene_changed, uint32_t& taa_changed, uint32_t& descriptor_changed) {
    scene_changed = taa_changed = descriptor_changed = 0;
    for (unsigned i = 0; i < 10; ++i) {
        const uint32_t bit = 1u << i;
        if ((before.state.scene_slot_mask & after.state.scene_slot_mask & bit)
            && before.scene_buffers[i] != after.scene_buffers[i]) scene_changed |= bit;
        if ((before.state.scene_descriptor_mask & after.state.scene_descriptor_mask & bit)
            && before.descriptors[i] != after.descriptors[i]) descriptor_changed |= bit;
    }
    for (unsigned i = 0; i < 5; ++i)
        if ((before.state.taa_slot_mask & after.state.taa_slot_mask & (1u << i))
            && before.taa_resources[i] != after.taa_resources[i]) taa_changed |= 1u << i;
}
