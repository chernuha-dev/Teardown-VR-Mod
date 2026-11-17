// SPDX-License-Identifier: MIT
#include "tdvr_temporal_scope.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
void require(bool value,const char* message) { if (!value) { std::cerr << message << '\n'; std::exit(1); } }
struct Fixture {
    std::array<uint8_t,0x2400> scene{};
    std::array<uint8_t,0x78> taa{};
    bool occupied = false;
    TdvrTemporalMemory memory() { return {scene.data(),scene.size(),taa.data(),taa.size()}; }
    Fixture() {
        scene.fill(0x7b); taa.fill(0x6a);
        const uint64_t serial = 50; const uint32_t history[2] = {0,1};
        std::memcpy(scene.data()+0x2208,&serial,sizeof(serial));
        std::memcpy(scene.data()+0x2210,history,sizeof(history));
        scene[0x23f1] = 0;
    }
};
TdvrTemporalCpuState capture(Fixture& f) {
    TdvrTemporalCpuState result;
    require(tdvr_capture_temporal_cpu(f.memory(),result),"fixture capture failed"); return result;
}
TdvrTemporalCpuState bank(Fixture& f,unsigned eye) {
    auto b = capture(f);
    for (unsigned i = 0; i < 10; ++i) b.targets[i] = 0x10000+eye*0x1000+i*0x10;
    for (unsigned i = 0; i < 5; ++i) b.taa_textures[i] = 0x20000+eye*0x1000+i*0x10;
    b.taa_counter = 0; b.taa_jitter.fill(0); b.reset = 1;
    b.camera_cache.fill(eye ? 0x52 : 0x4c); return b;
}
void render(Fixture& f,uint8_t tag) {
    auto current = capture(f);
    ++current.serial; ++current.taa_counter;
    const uint32_t history[2] = {current.history[1],current.history[0]};
    std::memcpy(f.scene.data()+0x2208,&current.serial,sizeof(current.serial));
    std::memcpy(f.scene.data()+0x2210,history,sizeof(history));
    std::memset(f.scene.data()+0x2218,tag,0x194);
    std::memcpy(f.taa.data(),&current.taa_counter,sizeof(current.taa_counter));
    std::memset(f.taa.data()+0x18,tag,8); f.scene[0x23f1] = 0;
}
}
int main() {
    Fixture f; const auto scene = f.scene; const auto taa = f.taa;
    auto left = bank(f,0), right = bank(f,1);
    {
        TdvrTemporalScope scope(f.memory(),left,f.occupied);
        require(scope.active() && f.occupied,"left scope inactive");
        require(capture(f).targets == left.targets && capture(f).taa_textures == left.taa_textures,"resource slots not installed");
        auto nested_bank = right;
        TdvrTemporalScope nested(f.memory(),nested_bank,f.occupied);
        require(!nested.active() && capture(f).targets == left.targets,"nested scope overwrote active bank");
        render(f,0x11);
        require(scope.finish() && !scope.finish() && !f.occupied,"finish not exactly once");
    }
    require(f.scene == scene && f.taa == taa,"caller CPU bytes not restored");
    require(left.serial == 51 && left.taa_counter == 1 && left.camera_cache[0] == 0x11 && !left.reset,"left post-render state lost");
    {
        TdvrTemporalScope scope(f.memory(),right,f.occupied);
        require(capture(f).taa_counter == 0 && capture(f).serial == 50 && capture(f).reset == 1,"right inherited left counter/reset");
        render(f,0x22); require(scope.finish(),"right finish failed");
    }
    require(f.scene == scene && f.taa == taa && right.taa_counter == 1,"right restoration failed");
    const auto left_before = left;
    try {
        TdvrTemporalScope scope(f.memory(),left,f.occupied);
        require(capture(f).taa_counter == 1 && capture(f).camera_cache[0] == 0x11,"warm bank inherited other eye history");
        render(f,0x33); throw std::runtime_error("mock render failure");
    } catch (const std::runtime_error&) {}
    require(f.scene == scene && f.taa == taa && !f.occupied && left.serial == left_before.serial
        && left.camera_cache == left_before.camera_cache,"unwinding published partial bank or failed restoration");
    for (unsigned failure = 0; failure < 6; ++failure) {
        auto bad = left; auto memory = f.memory();
        switch (failure) {
        case 0: bad.targets[0] = 0; break;
        case 1: bad.taa_textures[0] = bad.targets[1]; break;
        case 2: bad.history[1] = bad.history[0]; break;
        case 3: memory.scene_size = 0x23f1; break;
        case 4: memory.taa = memory.scene+0x2000; break;
        case 5: memory.scene = reinterpret_cast<uint8_t*>(std::numeric_limits<uintptr_t>::max()-20); break;
        }
        TdvrTemporalScope scope(memory,bad,f.occupied);
        require(!scope.active() && f.scene == scene && f.taa == taa && !f.occupied,"invalid scope changed memory");
    }
    {
        TdvrTemporalScope scope(f.memory(),left,f.occupied);
        const uint64_t foreign = 0x88888;
        std::memcpy(f.taa.data()+0x50,&foreign,sizeof(foreign));
        require(!scope.finish() && left.taa_textures == left_before.taa_textures,"foreign/replaced resource published");
    }
    require(f.scene == scene && f.taa == taa,"failed finish did not restore caller");
    {
        TdvrTemporalScope scope(f.memory(),right,f.occupied);
        f.scene[0x400] = 0x19; f.taa[0x24] = 0x29;
        require(scope.finish(),"scope failed on unrelated engine state");
    }
    auto expected_scene = scene; auto expected_taa = taa;
    expected_scene[0x400] = 0x19; expected_taa[0x24] = 0x29;
    require(f.scene == expected_scene && f.taa == expected_taa,"scope overwrote unrelated engine state");
    std::cout << "Independent CPU histories, exact restoration, unwind, bounds, no nested/foreign resources verified\n";
}
