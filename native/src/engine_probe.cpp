#include "tdvr_engine_probe.h"
#include "tdvr_frame.h"
#include <MinHook.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>

namespace {
constexpr uintptr_t camera_rva = 0x5ded80;
constexpr uintptr_t world_render_rva = 0x865b0;
constexpr uintptr_t game_global_rva = 0xc50030;
constexpr std::array<unsigned char, 22> world_prefix{{0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,
    0x48,0x89,0x50,0x10,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57}};
constexpr std::array<unsigned char, 19> camera_prefix{{0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,
    0x10,0x57,0x48,0x81,0xec,0xd0,0,0,0}};
constexpr std::array<unsigned char, 32> observed_hash{{
    0x27,0x33,0xe3,0x79,0x69,0xfd,0x6d,0x45,0xc5,0xc2,0x13,0x08,0x72,0xb0,0xd4,0x09,
    0x61,0x47,0xc3,0x04,0x26,0x6b,0x4b,0x0a,0xdd,0xa0,0x1b,0xd9,0x8c,0xaa,0x2c,0x4d}};
using SetCamera = void (*)(void*, const float*, const float*);
using WorldRender = void (*)(void*, void*, void*, void*);
struct State {
    std::mutex mutex;
    std::atomic<uint64_t> calls{0};
    TdvrEngineStats stats{sizeof(TdvrEngineStats), 1};
    SetCamera original = nullptr;
    WorldRender world_original = nullptr;
    std::atomic<uint64_t> world_calls{0}, world_completed{0};
    TdvrWorldRenderStats world_stats{sizeof(TdvrWorldRenderStats), 1};
    bool attempted = false;
};
State& state() { static auto* s = new State; return *s; }
bool read(const void* address, void* out, size_t size) {
    SIZE_T bytes = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &bytes) && bytes == size;
}
bool match_hash(const wchar_t* path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::ifstream file(path, std::ios::binary);
    ok = ok && file.good();
    std::array<char, 65536> buffer{};
    uint64_t size = 0;
    while (ok && file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = file.gcount();
        size += static_cast<uint64_t>(count);
        if (count && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0) ok = false;
    }
    std::array<unsigned char, 32> digest{};
    ok = ok && file.eof() && size == 13655624 && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok && digest == observed_hash;
}
void observe(void* object, const float* position, const float* orientation) noexcept {
    auto& s = state();
    const auto count = s.calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((count & 15) != 1) return; // observer only; do not add a mutex to every camera call
    std::lock_guard<std::mutex> lock(s.mutex);
    try {
        float p[3]{}, q[4]{}, matrix[16]{}, projection[16]{};
        if (!read(position, p, sizeof(p)) || !read(orientation, q, sizeof(q))
            || !read(static_cast<char*>(object) + 0x40, matrix, sizeof(matrix))
            || !read(object, projection, sizeof(projection))) { ++s.stats.rejected_samples; return; }
        TdvrFrameInput input{};
        input.frame = {count, 1};
        input.world_from_tracking = {{0,0,0},{0,0,0,1}};
        input.world_scale = 1; input.near_clip_m = .05; input.far_clip_m = 100;
        for (auto& eye : input.eyes)
            eye = {input.frame, {{p[0],p[1],p[2]},{q[0],q[1],q[2],q[3]}}, {-.8,.8,.8,-.8}, 15};
        TdvrStereoFrame frame;
        if (tdvr_build_stereo_frame(&input, &frame) != TDVR_OK) { ++s.stats.rejected_samples; return; }
        double error = 0;
        for (int row = 0; row != 4; ++row) for (int col = 0; col != 4; ++col) {
            const auto actual = matrix[col*4+row]; // engine column-major -> our row-major
            if (!std::isfinite(actual)) { ++s.stats.rejected_samples; return; }
            const double difference = std::abs(actual - frame.left_eye().view.m[row*4+col]);
            if (difference > error) error = difference;
        }
        ++s.stats.valid_samples;
        for (int i = 0; i != 3; ++i) s.stats.position[i] = p[i];
        for (int i = 0; i != 4; ++i) s.stats.orientation[i] = q[i];
        for (int i = 0; i != 16; ++i) s.stats.projection[i] = projection[i];
        s.stats.maximum_view_error = error;
    } catch (...) { ++s.stats.rejected_samples; }
}
void camera(void* object, const float* position, const float* orientation) {
    // Observational first slice: all original arguments and rendering behavior stay
    // with the engine. Read its completed view matrix only AFTER the original call.
    state().original(object, position, orientation);
    observe(object, position, orientation);
}
struct RenderState { uint64_t serial; uint32_t history_a, history_b; };
void world_render(void* scene, void* target, void* player, void* camera_object) {
    auto& s = state();
    const auto count = s.world_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool sample = (count & 15) == 1;
    float before[34]{}, after[34]{};
    RenderState state_before{}, state_after{};
    uintptr_t game = 0;
    bool valid = false, main_camera = false;
    if (sample) {
        valid = read(camera_object, before, sizeof(before))
            && read(static_cast<char*>(scene)+0x2208, &state_before, sizeof(state_before));
        for (const float value : before) if (!std::isfinite(value)) valid = false;
        valid = valid && before[11] == -1 && before[10] < -1 && before[32] > 0 && before[33] > before[32];
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        main_camera = read(reinterpret_cast<void*>(base+game_global_rva), &game, sizeof(game))
            && game && reinterpret_cast<uintptr_t>(camera_object) == game+0x240;
    }
    // No replay, no matrix writes, no skipped simulation: preserve all arguments.
    // Never hold the stats mutex across engine code (which may set other cameras).
    s.world_original(scene, target, player, camera_object);
    s.world_completed.fetch_add(1, std::memory_order_relaxed);
    if (!sample) return;
    valid = valid && read(camera_object, after, sizeof(after))
        && read(static_cast<char*>(scene)+0x2208, &state_after, sizeof(state_after));
    std::lock_guard<std::mutex> lock(s.mutex);
    auto& stats = s.world_stats;
    if (!valid) { ++stats.invalid_samples; return; }
    ++stats.valid_samples;
    if (main_camera) ++stats.main_camera_matches;
    if (std::memcmp(before, after, sizeof(before))) ++stats.camera_mutations;
    if (state_after.serial == state_before.serial+1) ++stats.serial_steps;
    if (state_after.history_a == state_before.history_b && state_after.history_b == state_before.history_a) ++stats.history_swaps;
    const auto thread = GetCurrentThreadId();
    if (!stats.first_thread) stats.first_thread = thread;
    else if (stats.first_thread != thread) ++stats.thread_changes;
    std::memcpy(stats.camera, before, sizeof(before));
}
}
HRESULT WINAPI TdvrStartEngineProbe(void*) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.attempted) return s.stats.hook_installed ? S_FALSE : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    s.attempted = true;
    wchar_t filename[32768]{};
    if (!GetModuleFileNameW(nullptr, filename, static_cast<DWORD>(std::size(filename))) || !match_hash(filename))
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    s.stats.identity_matched = 1;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* target = reinterpret_cast<void*>(base + camera_rva);
    std::array<unsigned char, camera_prefix.size()> bytes{};
    if (!read(target, bytes.data(), bytes.size()) || bytes != camera_prefix)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    s.stats.signature_matched = 1;
    auto* world_target = reinterpret_cast<void*>(base + world_render_rva);
    std::array<unsigned char, world_prefix.size()> world_bytes{};
    if (!read(world_target, world_bytes.data(), world_bytes.size()) || world_bytes != world_prefix)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    s.world_stats.signature_matched = 1;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&TdvrStartEngineProbe), &module)) return HRESULT_FROM_WIN32(GetLastError());
    if (MH_Initialize() != MH_OK) return E_FAIL;
    if (MH_CreateHook(target, reinterpret_cast<void*>(camera), reinterpret_cast<void**>(&s.original)) != MH_OK) {
        MH_Uninitialize(); return E_FAIL;
    }
    if (MH_CreateHook(world_target, reinterpret_cast<void*>(world_render), reinterpret_cast<void**>(&s.world_original)) != MH_OK
        || MH_QueueEnableHook(target) != MH_OK || MH_QueueEnableHook(world_target) != MH_OK
        || MH_ApplyQueued() != MH_OK) { MH_Uninitialize(); return E_FAIL; }
    s.stats.hook_installed = 1;
    s.world_stats.hook_installed = 1;
    return S_OK;
}
HRESULT WINAPI TdvrGetEngineStats(TdvrEngineStats* stats) {
    if (!stats || stats->size != sizeof(*stats) || stats->abi != 1) return E_INVALIDARG;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    *stats = s.stats;
    stats->camera_calls = s.calls.load(std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI TdvrGetWorldRenderStats(TdvrWorldRenderStats* stats) {
    if (!stats || stats->size != sizeof(*stats) || stats->abi != 1) return E_INVALIDARG;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    *stats = s.world_stats;
    stats->calls = s.world_calls.load(std::memory_order_relaxed);
    stats->completed_calls = s.world_completed.load(std::memory_order_relaxed);
    return S_OK;
}
