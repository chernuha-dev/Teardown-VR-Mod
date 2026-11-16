// SPDX-License-Identifier: MIT
#include "tdvr_temporal_observer.h"
#include <map>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
namespace {
void require(bool yes, const char* text) { if (!yes) { std::cerr << text << '\n'; std::exit(1); } }
struct Memory {
    std::map<uint64_t,std::vector<uint8_t>> regions;
    uint64_t fail_address = 0;
    unsigned reads = 0, opaque_resource_reads = 0;
    template<class T> void put(uint64_t base, size_t offset, T value) {
        auto& region = regions.at(base);
        require(offset+sizeof(value) <= region.size(),"fixture write bounds");
        std::memcpy(region.data()+offset,&value,sizeof(value));
    }
    static bool read(uint64_t address, void* out, size_t size, void* context) {
        auto& m = *static_cast<Memory*>(context); ++m.reads;
        if (address >= 0x90000) ++m.opaque_resource_reads;
        if (address == m.fail_address) return false;
        for (const auto& [base,bytes] : m.regions) {
            if (address < base || address-base > bytes.size() || size > bytes.size()-(address-base)) continue;
            std::memcpy(out,bytes.data()+static_cast<size_t>(address-base),size); return true;
        }
        return false;
    }
};
}
int main() {
    Memory memory; memory.regions[0x10000].resize(0x2400); memory.regions[0x20000].resize(0x78);
    memory.put<uint64_t>(0x10000,0x2208,9); memory.put<uint32_t>(0x10000,0x2210,2);
    memory.put<uint32_t>(0x10000,0x2214,3); memory.put<uint8_t>(0x10000,0x23f1,1);
    memory.put<uint64_t>(0x10000,0xa30,0x20000); memory.put<uint32_t>(0x20000,0,7);
    constexpr size_t fields[] = {0xa50,0xa58,0xaa8,0xab0,0xac0,0xac8,0xae0,0xae8,0xaf8,0xb00};
    for (unsigned i = 0; i < 10; ++i) {
        const uint64_t texture = 0x30000+i*0x100;
        memory.regions[texture] = std::vector<uint8_t>(0x38,static_cast<uint8_t>(i+1));
        memory.put<uint64_t>(0x10000,fields[i],texture);
    }
    for (unsigned i = 0; i < 5; ++i) memory.put<uint64_t>(0x20000,0x50+i*8,0x90000+i*0x100);
    const auto untouched = memory.regions;
    TdvrTemporalObservation before, after;
    require(tdvr_observe_temporal(0x10000,Memory::read,&memory,before),"valid CPU observation rejected");
    const auto& b = before.state;
    require(b.serial == 9 && b.history[0] == 2 && b.history[1] == 3 && b.reset_flag == 1
        && b.taa_word == 7 && b.taa_word_readable && b.scene_slot_mask == 1023 && b.scene_descriptor_mask == 1023
        && b.taa_slot_mask == 31 && b.taa_presence_mask == 31,"observed masks/history/counter incorrect");
    require(memory.regions == untouched && memory.opaque_resource_reads == 0,"observer modified memory or dereferenced opaque TAA resources");
    memory.put<uint64_t>(0x10000,fields[0],0x30100); memory.put<uint64_t>(0x10000,fields[1],0x30000);
    memory.regions[0x30200][55] ^= 0xff;
    memory.put<uint64_t>(0x20000,0x50,0);
    memory.put<uint64_t>(0x10000,0x2208,10); memory.put<uint32_t>(0x10000,0x2210,3);
    memory.put<uint32_t>(0x10000,0x2214,2); memory.put<uint8_t>(0x10000,0x23f1,0);
    require(tdvr_observe_temporal(0x10000,Memory::read,&memory,after),"after observation failed");
    uint32_t scene,taa,descriptor;
    tdvr_compare_temporal(before,after,scene,taa,descriptor);
    require(scene == 3 && taa == 1 && descriptor == 7 && after.state.taa_presence_mask == 30,
        "pointer swap, descriptor mutation or resource disappearance missed");
    require(before.state.descriptor_hashes[2] != after.state.descriptor_hashes[2],"descriptor fingerprint unchanged");
    const auto saved = after;
    memory.fail_address = 0x10000+0x2210;
    require(!tdvr_observe_temporal(0x10000,Memory::read,&memory,after)
        && !std::memcmp(&saved,&after,sizeof(after)),"failed basic read published partial output");
    memory.fail_address = 0;
    memory.regions.erase(0x30200); memory.regions.erase(0x20000);
    require(tdvr_observe_temporal(0x10000,Memory::read,&memory,after) && !(after.state.scene_descriptor_mask&4)
        && !after.state.taa_slot_mask && !after.state.taa_word_readable,"unreadable optional resources treated as valid");
    tdvr_compare_temporal(before,after,scene,taa,descriptor);
    require(!(descriptor&4) && taa == 0,"unreadable resources reported as comparable");
    const auto reads = memory.reads;
    require(!tdvr_observe_temporal(0,Memory::read,&memory,after)
        && !tdvr_observe_temporal(std::numeric_limits<uint64_t>::max()-32,Memory::read,&memory,after)
        && !tdvr_observe_temporal(0x10000,nullptr,&memory,after) && memory.reads == reads,"overflow/null address reached reader");
    std::cout << "Read-only temporal state, pointer-free masks/fingerprints and failure bounds passed\n";
}
