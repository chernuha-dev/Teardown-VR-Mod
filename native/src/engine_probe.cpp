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
#include <cwctype>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <set>
#include <limits>
#include <wrl/client.h>

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
using BeginXrWorld = HRESULT (WINAPI*)(TdvrXrViewSnapshot*);
struct State {
    std::mutex mutex;
    std::atomic<uint64_t> calls{0};
    TdvrEngineStats stats{sizeof(TdvrEngineStats), 1};
    SetCamera original = nullptr;
    WorldRender world_original = nullptr;
    BeginXrWorld begin_xr_world = nullptr; // immutable before hooks are enabled
    std::atomic<uint64_t> world_calls{0}, world_completed{0};
    TdvrWorldRenderStats world_stats{sizeof(TdvrWorldRenderStats), 1};
    TdvrEngineTargetStats target_stats{sizeof(TdvrEngineTargetStats), 1};
    TdvrTemporalStats temporal_stats{sizeof(TdvrTemporalStats), 1};
    TdvrEyeTargetProbeStats eye_target_stats{sizeof(TdvrEyeTargetProbeStats),1};
    void* retained_eye_targets[2]{}; // bounded diagnostic ownership; no guessed destructor
    TdvrEyeHistoryProbeStats eye_history_stats{sizeof(TdvrEyeHistoryProbeStats),1};
    void* retained_eye_histories[2][10]{};
    TdvrEyeTaaProbeStats eye_taa_stats{sizeof(TdvrEyeTaaProbeStats),1};
    void* retained_eye_taa[2][5]{};
    TdvrCameraOverride override_camera;
    TdvrTrackedCameraPreview tracked_camera;
    std::atomic<bool> tracked_pending{false};
    std::atomic<bool> override_pending{false};
    TdvrCameraOverrideStats override_stats{sizeof(TdvrCameraOverrideStats), 1};
    bool attempted = false;
};
State& state() { static auto* s = new State; return *s; }
bool read(const void* address, void* out, size_t size) {
    SIZE_T bytes = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &bytes) && bytes == size;
}
bool temporal_read(uint64_t address, void* out, size_t size, void*) {
    return read(reinterpret_cast<const void*>(address),out,size);
}
bool development_process() {
    wchar_t executable[32768]{}, module_path[32768]{}, profile[32768]{};
    HMODULE module = nullptr;
    if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))
        || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&TdvrSubmitCameraOverride), &module)
        || !GetModuleFileNameW(module, module_path, static_cast<DWORD>(std::size(module_path)))) return false;
    const auto profile_length = GetEnvironmentVariableW(L"TDVR_DEV_PROFILE", profile, static_cast<DWORD>(std::size(profile)));
    if (!profile_length || profile_length >= std::size(profile)) return false;
    std::wstring exe(executable), path(module_path);
    for (auto& c : exe) c = static_cast<wchar_t>(std::towlower(c));
    for (auto& c : path) c = static_cast<wchar_t>(std::towlower(c));
    const auto build = path.rfind(L"\\build\\");
    return build != std::wstring::npos && exe.find(path.substr(0, build)+L"\\local\\") == 0;
}
bool writable_camera(void* object) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(object, &region, sizeof(region)) || region.State != MEM_COMMIT
        || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto access = region.Protect & 0xff;
    const auto address = reinterpret_cast<uintptr_t>(object);
    const auto start = reinterpret_cast<uintptr_t>(region.BaseAddress);
    return (access == PAGE_READWRITE || access == PAGE_EXECUTE_READWRITE || access == PAGE_WRITECOPY
        || access == PAGE_EXECUTE_WRITECOPY) && address >= start
        && address-start <= region.RegionSize && sizeof(TdvrEngineCameraMatrices) <= region.RegionSize-(address-start);
}
bool match_file_hash(const wchar_t* path, uint64_t expected_size, const std::array<unsigned char, 32>& expected_hash) {
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
    ok = ok && file.eof() && size == expected_size && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok && digest == expected_hash;
}
bool match_hash(const wchar_t* path) { return match_file_hash(path, 13655624, observed_hash); }
HMODULE trusted_d3d12_core() {
    // Resolve once: application-local Agility SDK is not the System32 loader.
    static const HMODULE trusted = []() -> HMODULE {
        const auto core = GetModuleHandleW(L"D3D12Core.dll");
        wchar_t filename[32768]{}, system[32768]{}, executable[32768]{};
        if (!core || !GetModuleFileNameW(core, filename, static_cast<DWORD>(std::size(filename)))
            || !GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)))) return nullptr;
        if (!_wcsicmp(filename, (std::wstring(system)+L"\\D3D12Core.dll").c_str())) return core;
        if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))) return nullptr;
        const std::wstring exe(executable);
        const auto slash = exe.find_last_of(L"\\/");
        if (slash == std::wstring::npos || _wcsicmp(filename, (exe.substr(0, slash)+L"\\D3D12\\D3D12Core.dll").c_str())) return nullptr;
        constexpr std::array<unsigned char, 32> agility_hash{{
            0x87,0x10,0x11,0x0e,0xcc,0xf4,0x3a,0x32,0xe5,0xe1,0xa0,0x45,0x52,0xcb,0x34,0x48,
            0x32,0x92,0x2a,0x0c,0xd2,0x3c,0xae,0xec,0xb7,0x64,0xe1,0x8b,0xd5,0xcd,0xff,0xa8}};
        return match_file_hash(filename, 5617048, agility_hash) ? core : nullptr;
    }();
    return trusted;
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
Microsoft::WRL::ComPtr<ID3D12Resource> trusted_resource(uintptr_t wrapper) {
    Microsoft::WRL::ComPtr<ID3D12Resource> result;
    const auto core = trusted_d3d12_core();
    uintptr_t resource = 0, vtable = 0, table[11]{};
    if (!core || !wrapper || !read(reinterpret_cast<void*>(wrapper+0x38),&resource,sizeof(resource)) || !resource
        || !read(reinterpret_cast<void*>(resource),&vtable,sizeof(vtable))
        || !read(reinterpret_cast<void*>(vtable),table,sizeof(table))) return result;
    for (const auto slot : {0,7,10}) {
        HMODULE owner = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(table[slot]),&owner) || owner != core) return result;
    }
    reinterpret_cast<IUnknown*>(resource)->QueryInterface(IID_PPV_ARGS(&result));
    return result;
}
using CreateTarget = void* (*)(void*,const void*,const char*);
CreateTarget target_factory(void* scene, uintptr_t& renderer) {
    uintptr_t vtable = 0, factory = 0;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    constexpr std::array<uint8_t,21> prefix{{0x48,0x89,0x5c,0x24,0x08,0x55,0x56,0x57,
        0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0xf0}};
    std::array<uint8_t,prefix.size()> observed{};
    if (!read(static_cast<char*>(scene)+0x9c0,&renderer,sizeof(renderer)) || !renderer
        || !read(reinterpret_cast<void*>(renderer),&vtable,sizeof(vtable))
        || !read(reinterpret_cast<void*>(vtable+0x138),&factory,sizeof(factory)) || factory != base+0x5a8610
        || !read(reinterpret_cast<void*>(factory),observed.data(),observed.size()) || observed != prefix) return nullptr;
    return reinterpret_cast<CreateTarget>(factory);
}
// Only the audited target/wrapper layout, on the render thread after the engine
// used it. Never invoke arbitrary candidate vtables or dereference from an RPC.
TdvrEngineTargetStats observe_target(void* target, uintptr_t game) {
    TdvrEngineTargetStats result{sizeof(TdvrEngineTargetStats), 1};
    read(reinterpret_cast<void*>(game+8), &result.game_state, sizeof(result.game_state));
    if (!read(target, &result.width, 2*sizeof(uint32_t))
        || !read(static_cast<char*>(target)+0x30, &result.count, sizeof(result.count))
        || !result.count || result.count > 8 || !result.width || !result.height) return result;
    for (uint32_t i = 0; i < result.count; ++i) {
        uintptr_t wrapper = 0;
        if (!read(static_cast<char*>(target)+0x38+i*sizeof(uintptr_t),&wrapper,sizeof(wrapper))) continue;
        auto confirmed = trusted_resource(wrapper);
        if (!confirmed) continue;
        const auto desc = confirmed->GetDesc();
        result.colors[i] = {desc.Width, desc.Height, static_cast<uint32_t>(desc.Format),
            static_cast<uint32_t>(desc.Flags), desc.SampleDesc.Count, static_cast<uint32_t>(desc.Dimension),
            desc.MipLevels, desc.DepthOrArraySize, 0};
        result.known_mask |= 1u << i;
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
            && desc.Width == result.width && desc.Height == result.height) result.matching_extent_mask |= 1u << i;
    }
    return result;
}
void probe_eye_targets(void* scene, void* target, uintptr_t game) {
    auto& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.eye_target_stats.phase != 1 || GetCurrentThreadId() != s.world_stats.first_thread) return;
        s.eye_target_stats.phase = 4; // running; never repeat a partial allocation
    }
    TdvrEyeTargetProbeStats result{sizeof(TdvrEyeTargetProbeStats),1,3};
    do {
        uint32_t game_state = 0;
        if (!read(reinterpret_cast<void*>(game+8),&game_state,sizeof(game_state)) || game_state != 6) { result.failure = 1; break; }
        std::array<uint8_t,0x38> descriptor{};
        TdvrTemporalTargetLayout layout{};
        if (!read(target,descriptor.data(),descriptor.size())
            || !tdvr_parse_temporal_target(descriptor.data(),descriptor.size(),layout) || layout.color_count != 1) {
            result.failure = 2; break;
        }
        uintptr_t renderer = 0;
        const auto create = target_factory(scene,renderer);
        if (!create) {
            result.failure = 3; break;
        }
        // Preserve all opaque descriptor fields; change only the audited extents.
        const uint32_t extent = 64;
        std::memcpy(descriptor.data(),&extent,sizeof(extent));
        std::memcpy(descriptor.data()+4,&extent,sizeof(extent));
        bool valid = true;
        for (unsigned eye = 0; eye < 2; ++eye) {
            s.retained_eye_targets[eye] = create(reinterpret_cast<void*>(renderer),descriptor.data(),
                eye ? "tdvr-probe-right" : "tdvr-probe-left");
            if (!s.retained_eye_targets[eye]) { result.failure = 4; valid = false; break; }
            ++result.created;
            result.eyes[eye] = observe_target(s.retained_eye_targets[eye],game);
            const auto& meta = result.eyes[eye];
            if (meta.width != 64 || meta.height != 64 || meta.count != 1 || meta.known_mask != 1 || meta.matching_extent_mask != 1) {
                result.failure = 5; valid = false; break;
            }
        }
        if (!valid) break;
        uintptr_t resources[2]{};
        for (unsigned eye = 0; eye < 2; ++eye) {
            uintptr_t wrapper = 0;
            valid = valid && read(static_cast<char*>(s.retained_eye_targets[eye])+0x38,&wrapper,sizeof(wrapper)) && wrapper
                && read(reinterpret_cast<void*>(wrapper+0x38),&resources[eye],sizeof(resources[eye])) && resources[eye];
        }
        if (!valid || resources[0] == resources[1] || s.retained_eye_targets[0] == s.retained_eye_targets[1]) { result.failure = 6; break; }
        result.distinct_resources = 1; result.phase = 2;
    } while (false);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.eye_target_stats = result;
}
bool same_resource_description(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
    return a.Dimension == b.Dimension && a.Alignment == b.Alignment && a.Width == b.Width && a.Height == b.Height
        && a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels && a.Format == b.Format
        && a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality
        && a.Layout == b.Layout && a.Flags == b.Flags;
}
bool allocation_budget(ID3D12Device* device, uint64_t required, uint64_t& budget, uint64_t& usage, uint32_t& failure) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter3> adapter;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if (!device || device->GetNodeCount() != 1 || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))
        || FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)))
        || FAILED(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory))) { failure = 7; return false; }
    budget = memory.Budget; usage = memory.CurrentUsage;
    constexpr uint64_t reserve = 384ull*1024*1024;
    if (usage >= budget || budget-usage <= reserve || required > budget-usage-reserve) { failure = 8; return false; }
    return true;
}
void probe_eye_histories(void* scene, uintptr_t game) {
    auto& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.eye_history_stats.phase != 1 || GetCurrentThreadId() != s.world_stats.first_thread) return;
        s.eye_history_stats.phase = 4;
    }
    TdvrEyeHistoryProbeStats result{sizeof(TdvrEyeHistoryProbeStats),1,3};
    do {
        uint32_t game_state = 0;
        if (!read(reinterpret_cast<void*>(game+8),&game_state,sizeof(game_state)) || game_state != 6) { result.failure = 1; break; }
        uintptr_t renderer = 0;
        const auto create = target_factory(scene,renderer);
        if (!create) { result.failure = 3; break; }
        TdvrTemporalObservation source;
        if (!tdvr_observe_temporal(reinterpret_cast<uintptr_t>(scene),temporal_read,nullptr,source)
            || source.state.target_layout_mask != 0x3ff) { result.failure = 2; break; }
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D12Device> device;
        std::set<IUnknown*> identities;
        std::set<void*> targets;
        std::array<std::array<D3D12_RESOURCE_DESC,9>,10> descriptions{};
        uint64_t source_bytes = 0;
        // Validate every nested attachment before any allocation. No unknown
        // auxiliary target, cross-device allocation, or guessed DXGI format.
        auto inspect = [&](void* target, unsigned slot, bool is_new) {
            const auto& layout = source.targets[slot];
            uintptr_t auxiliary = 0;
            if (!read(static_cast<char*>(target)+0x80,&auxiliary,sizeof(auxiliary)) || auxiliary) return false;
            const auto count = layout.color_count + (layout.depth_enabled ? 1u : 0u);
            for (unsigned attachment = 0; attachment < count; ++attachment) {
                uintptr_t wrapper = 0;
                const auto offset = attachment < layout.color_count ? 0x38+attachment*8 : 0x78;
                if (!read(static_cast<char*>(target)+offset,&wrapper,sizeof(wrapper))) return false;
                auto resource = trusted_resource(wrapper);
                if (!resource) return false;
                ComPtr<ID3D12Device> owner;
                if (FAILED(resource->GetDevice(IID_PPV_ARGS(&owner)))) return false;
                if (!device) device = owner;
                if (device.Get() != owner.Get()) return false;
                ComPtr<IUnknown> identity;
                if (FAILED(resource.As(&identity))) return false;
                const auto inserted = identities.insert(identity.Get()).second;
                if (is_new && !inserted) return false;
                const auto desc = resource->GetDesc();
                if (!is_new) {
                    descriptions[slot][attachment] = desc;
                    const auto info = device->GetResourceAllocationInfo(0,1,&desc);
                    if (!info.SizeInBytes || info.SizeInBytes == std::numeric_limits<uint64_t>::max()
                        || source_bytes > std::numeric_limits<uint64_t>::max()-info.SizeInBytes) return false;
                    source_bytes += info.SizeInBytes;
                } else {
                    const auto& expected = descriptions[slot][attachment];
                    if (!same_resource_description(desc,expected)) return false;
                }
            }
            return true;
        };
        bool valid = true;
        for (unsigned slot = 0; slot < 10; ++slot) {
            auto* target = reinterpret_cast<void*>(source.scene_buffers[slot]);
            targets.insert(target);
            if (!inspect(target,slot,false)) { valid = false; break; }
        }
        if (!valid || !device || device->GetNodeCount() != 1 || source_bytes > std::numeric_limits<uint64_t>::max()/2) { result.failure = 5; break; }
        result.required_bytes = source_bytes*2;
        if (!allocation_budget(device.Get(),result.required_bytes,result.budget_bytes,result.usage_bytes,result.failure)) break;
        for (unsigned eye = 0; eye < 2 && valid; ++eye) for (unsigned slot = 0; slot < 10 && valid; ++slot) {
            const std::string name = std::string("tdvr-history-")+(eye ? "right-" : "left-")+std::to_string(slot);
            auto*& target = s.retained_eye_histories[eye][slot];
            target = create(reinterpret_cast<void*>(renderer),source.descriptors[slot].data(),name.c_str());
            if (!target) { result.failure = 4; valid = false; break; }
            ++result.created;
            if (!targets.insert(target).second) { result.failure = 6; valid = false; break; }
            std::array<uint8_t,0x38> actual{};
            if (!read(target,actual.data(),actual.size()) || actual != source.descriptors[slot]
                || !inspect(target,slot,true)) { result.failure = 5; valid = false; break; }
            result.targets[eye][slot] = observe_target(target,game);
        }
        if (!valid) break;
        TdvrTemporalObservation after;
        if (!tdvr_observe_temporal(reinterpret_cast<uintptr_t>(scene),temporal_read,nullptr,after)
            || after.state.target_layout_mask != 0x3ff || source.scene_buffers != after.scene_buffers
            || source.taa_resources != after.taa_resources || source.descriptors != after.descriptors
            || source.state.serial != after.state.serial || source.state.history[0] != after.state.history[0]
            || source.state.history[1] != after.state.history[1] || source.state.reset_flag != after.state.reset_flag
            || source.state.taa_word != after.state.taa_word) { result.failure = 9; break; }
        result.distinct_resources = 1; result.phase = 2;
    } while (false);
    std::lock_guard<std::mutex> lock(s.mutex); s.eye_history_stats = result;
}
void probe_eye_taa(void* scene, uintptr_t game) {
    auto& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.eye_taa_stats.phase != 1 || GetCurrentThreadId() != s.world_stats.first_thread) return;
        s.eye_taa_stats.phase = 4;
    }
    TdvrEyeTaaProbeStats result{sizeof(TdvrEyeTaaProbeStats),1,3};
    do {
        uint32_t game_state = 0;
        if (!read(reinterpret_cast<void*>(game+8),&game_state,sizeof(game_state)) || game_state != 6) { result.failure = 1; break; }
        uintptr_t renderer = 0, taa = 0, taa_renderer = 0, vtable = 0, entry = 0;
        constexpr std::array<uint8_t,26> prefix{{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,
            0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20,0x41,0x56,0x48,0x83,0xec,0x70}};
        std::array<uint8_t,prefix.size()> observed{};
        if (!target_factory(scene,renderer) || !read(static_cast<char*>(scene)+0xa30,&taa,sizeof(taa)) || !taa
            || !read(reinterpret_cast<void*>(taa+8),&taa_renderer,sizeof(taa_renderer)) || taa_renderer != renderer
            || !read(reinterpret_cast<void*>(renderer),&vtable,sizeof(vtable))
            || !read(reinterpret_cast<void*>(vtable+0xe8),&entry,sizeof(entry))
            || entry != reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x5a9000
            || !read(reinterpret_cast<void*>(entry),observed.data(),observed.size()) || observed != prefix) { result.failure = 3; break; }
        std::array<uint8_t,0x78> taa_before{},taa_after{};
        TdvrTemporalObservation source;
        if (!read(reinterpret_cast<void*>(taa),taa_before.data(),taa_before.size())
            || !tdvr_observe_temporal(reinterpret_cast<uintptr_t>(scene),temporal_read,nullptr,source)
            || source.state.taa_presence_mask != 31 || source.state.target_layout_mask != 0x3ff) { result.failure = 2; break; }
        uint32_t taa_width = 0, taa_height = 0;
        std::memcpy(&taa_width,taa_before.data()+0x10,sizeof(taa_width));
        std::memcpy(&taa_height,taa_before.data()+0x14,sizeof(taa_height));
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D12Device> device;
        std::set<IUnknown*> identities;
        std::set<uintptr_t> wrappers;
        auto identify = [&](uintptr_t wrapper, bool unique) {
            auto resource = trusted_resource(wrapper);
            if (!resource) return ComPtr<ID3D12Resource>{};
            ComPtr<ID3D12Device> owner;
            ComPtr<IUnknown> identity;
            if (FAILED(resource->GetDevice(IID_PPV_ARGS(&owner))) || FAILED(resource.As(&identity))) return ComPtr<ID3D12Resource>{};
            if (!device) device = owner;
            if (owner.Get() != device.Get()) return ComPtr<ID3D12Resource>{};
            const bool new_wrapper = wrappers.insert(wrapper).second;
            const bool new_resource = identities.insert(identity.Get()).second;
            if (unique && (!new_wrapper || !new_resource)) return ComPtr<ID3D12Resource>{};
            return resource;
        };
        // Exclude aliases with original framebuffer banks and both owned banks,
        // not just with the five original TAA objects.
        bool valid = true;
        for (unsigned bank = 0; bank < 3 && valid; ++bank) for (unsigned slot = 0; slot < 10 && valid; ++slot) {
            auto* target = bank ? s.retained_eye_histories[bank-1][slot] : reinterpret_cast<void*>(source.scene_buffers[slot]);
            const auto& layout = source.targets[slot];
            std::array<uint8_t,0x38> actual{};
            if (!target || !read(target,actual.data(),actual.size()) || actual != source.descriptors[slot]) { valid = false; break; }
            for (unsigned attachment = 0; attachment < layout.color_count+(layout.depth_enabled ? 1u : 0u); ++attachment) {
                uintptr_t wrapper = 0;
                const auto offset = attachment < layout.color_count ? 0x38+attachment*8 : 0x78;
                if (!target || !read(static_cast<char*>(target)+offset,&wrapper,sizeof(wrapper)) || !identify(wrapper,false)) { valid = false; break; }
            }
        }
        if (!valid) { result.failure = 5; break; }
        std::array<std::array<uint8_t,0x1c>,5> descriptors{};
        std::array<D3D12_RESOURCE_DESC,5> descriptions{};
        constexpr uint32_t formats[5] = {0x12,0x12,0x26,0x26,0x12};
        uint64_t source_bytes = 0;
        for (unsigned slot = 0; slot < 5; ++slot) {
            TdvrTaaTextureLayout layout{};
            const auto wrapper = source.taa_resources[slot];
            auto resource = identify(wrapper,false);
            if (!resource || !read(reinterpret_cast<void*>(wrapper),descriptors[slot].data(),descriptors[slot].size())
                || !tdvr_parse_taa_texture(descriptors[slot].data(),descriptors[slot].size(),layout)
                || layout.engine_format != formats[slot] || layout.width != taa_width || layout.height != taa_height) { valid = false; break; }
            const auto desc = resource->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != layout.width || desc.Height != layout.height
                || desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1) { valid = false; break; }
            descriptions[slot] = desc;
            const auto info = device->GetResourceAllocationInfo(0,1,&desc);
            if (!info.SizeInBytes || info.SizeInBytes == std::numeric_limits<uint64_t>::max()
                || source_bytes > std::numeric_limits<uint64_t>::max()-info.SizeInBytes) { valid = false; break; }
            source_bytes += info.SizeInBytes;
        }
        if (!valid || source_bytes > std::numeric_limits<uint64_t>::max()/2) { result.failure = 5; break; }
        result.required_bytes = source_bytes*2;
        if (!allocation_budget(device.Get(),result.required_bytes,result.budget_bytes,result.usage_bytes,result.failure)) break;
        using CreateTexture = void* (*)(void*,const void*,const void*,const char*);
        const auto create = reinterpret_cast<CreateTexture>(entry);
        for (unsigned eye = 0; eye < 2 && valid; ++eye) for (unsigned slot = 0; slot < 5 && valid; ++slot) {
            const std::string name = std::string("tdvr-taa-")+(eye ? "right-" : "left-")+std::to_string(slot);
            auto*& wrapper = s.retained_eye_taa[eye][slot];
            wrapper = create(reinterpret_cast<void*>(renderer),descriptors[slot].data(),nullptr,name.c_str());
            if (!wrapper) { result.failure = 4; valid = false; break; }
            ++result.created;
            auto resource = identify(reinterpret_cast<uintptr_t>(wrapper),true);
            std::array<uint8_t,0x1c> actual{};
            if (!resource || !read(wrapper,actual.data(),actual.size()) || actual != descriptors[slot]) { result.failure = 6; valid = false; break; }
            const auto desc = resource->GetDesc();
            if (!same_resource_description(desc,descriptions[slot])) { result.failure = 5; valid = false; break; }
            result.textures[eye][slot] = {desc.Width,desc.Height,static_cast<uint32_t>(desc.Format),static_cast<uint32_t>(desc.Flags),
                desc.SampleDesc.Count,static_cast<uint32_t>(desc.Dimension),desc.MipLevels,desc.DepthOrArraySize,0};
        }
        if (!valid) break;
        for (unsigned slot = 0; slot < 5; ++slot) {
            std::array<uint8_t,0x1c> actual{};
            if (!read(reinterpret_cast<void*>(source.taa_resources[slot]),actual.data(),actual.size()) || actual != descriptors[slot]) { valid = false; break; }
        }
        if (!valid) { result.failure = 9; break; }
        TdvrTemporalObservation after;
        if (!read(reinterpret_cast<void*>(taa),taa_after.data(),taa_after.size()) || taa_before != taa_after
            || !tdvr_observe_temporal(reinterpret_cast<uintptr_t>(scene),temporal_read,nullptr,after)
            || source.scene_buffers != after.scene_buffers || source.taa_resources != after.taa_resources
            || source.descriptors != after.descriptors || source.state.serial != after.state.serial
            || source.state.history[0] != after.state.history[0] || source.state.history[1] != after.state.history[1]
            || source.state.reset_flag != after.state.reset_flag) { result.failure = 9; break; }
        result.distinct_resources = 1; result.phase = 2;
    } while (false);
    std::lock_guard<std::mutex> lock(s.mutex); s.eye_taa_stats = result;
}
void world_render(void* scene, void* target, void* player, void* camera_object) {
    auto& s = state();
    const auto count = s.world_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool sample = (count & 15) == 1;
    float before[34]{}, after[34]{};
    RenderState state_before{}, state_after{};
    uintptr_t game = 0;
    bool valid = false, main_camera = false;
    const bool pending = s.override_pending.load(std::memory_order_acquire);
    const bool tracked_pending = s.tracked_pending.load(std::memory_order_acquire);
    if (sample || pending || tracked_pending || s.begin_xr_world) {
        valid = read(camera_object, before, sizeof(before))
            && read(static_cast<char*>(scene)+0x2208, &state_before, sizeof(state_before));
        for (const float value : before) if (!std::isfinite(value)) valid = false;
        valid = valid && before[11] == -1 && before[10] < -1 && before[32] > 0 && before[33] > before[32];
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        main_camera = read(reinterpret_cast<void*>(base+game_global_rva), &game, sizeof(game))
            && game && reinterpret_cast<uintptr_t>(camera_object) == game+0x240;
    }
    TdvrEngineCameraMatrices selected{};
    TdvrTemporalObservation temporal_before, temporal_after;
    const bool temporal_sample = sample && valid && main_camera;
    const bool temporal_before_valid = temporal_sample
        && tdvr_observe_temporal(reinterpret_cast<uintptr_t>(scene),temporal_read,nullptr,temporal_before);
    bool apply = false;
    if (pending) {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (valid && main_camera && GetCurrentThreadId() == s.world_stats.first_thread && writable_camera(camera_object)) {
            apply = s.override_camera.take(GetTickCount64(), selected);
            if (apply) {
                ++s.override_stats.applied_calls;
                std::memcpy(s.override_stats.last_camera, &selected, sizeof(selected));
            }
        } else ++s.override_stats.rejected_calls;
        s.override_pending.store(s.override_camera.remaining(GetTickCount64()) != 0, std::memory_order_release);
    }
    if (s.begin_xr_world && valid && main_camera) {
        bool same_thread = false;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            same_thread = !s.world_stats.first_thread || GetCurrentThreadId() == s.world_stats.first_thread;
        }
        // Called before the original world render, without the engine stats lock.
        TdvrXrViewSnapshot current{sizeof(TdvrXrViewSnapshot),1};
        const bool began = same_thread && s.begin_xr_world(&current) == S_OK;
        if (tracked_pending) {
            uint32_t game_state = 0;
            const bool gameplay = read(reinterpret_cast<void*>(game+8),&game_state,sizeof(game_state)) && game_state == 6;
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!began || !gameplay || !writable_camera(camera_object)) current = {sizeof(TdvrXrViewSnapshot),1};
            apply = s.tracked_camera.take(current,GetTickCount64(),selected);
            if (apply) {
                ++s.override_stats.applied_calls;
                std::memcpy(s.override_stats.last_camera,&selected,sizeof(selected));
            } else ++s.override_stats.rejected_calls;
            s.tracked_pending.store(s.tracked_camera.remaining(GetTickCount64()) != 0,std::memory_order_release);
        }
    } else if (tracked_pending) {
        std::lock_guard<std::mutex> lock(s.mutex);
        const TdvrXrViewSnapshot invalid{};
        s.tracked_camera.take(invalid,GetTickCount64(),selected);
        s.tracked_pending.store(false,std::memory_order_release);
        ++s.override_stats.rejected_calls;
    }
    // No replay or skipped simulation. Never hold the stats mutex across engine
    // code. On opt-in, the original sees one selected eye; RAII restores the camera.
    if (apply) {
        {
            TdvrEngineCameraScope scope(*static_cast<TdvrEngineCameraMatrices*>(camera_object), selected);
            s.world_original(scene, target, player, camera_object);
        }
        float restored[34]{};
        const bool restored_ok = read(camera_object, restored, sizeof(restored)) && !std::memcmp(before, restored, sizeof(before));
        std::lock_guard<std::mutex> lock(s.mutex);
        if (restored_ok) ++s.override_stats.restored_calls;
        else { ++s.override_stats.rejected_calls; s.override_pending.store(false, std::memory_order_release);
            s.tracked_pending.store(false,std::memory_order_release); }
    } else s.world_original(scene, target, player, camera_object);
    s.world_completed.fetch_add(1, std::memory_order_relaxed);
    if (valid && main_camera) {
        try { probe_eye_targets(scene,target,game); }
        catch (...) { std::lock_guard<std::mutex> lock(s.mutex); s.eye_target_stats.phase = 3; s.eye_target_stats.failure = 4; }
        try { probe_eye_histories(scene,game); }
        catch (...) { std::lock_guard<std::mutex> lock(s.mutex); s.eye_history_stats.phase = 3; s.eye_history_stats.failure = 4; }
        try { probe_eye_taa(scene,game); }
        catch (...) {
            std::lock_guard<std::mutex> lock(s.mutex); s.eye_taa_stats.phase = 3; s.eye_taa_stats.failure = 4;
            for (const auto& bank : s.retained_eye_taa) for (const auto wrapper : bank) if (wrapper) ++s.eye_taa_stats.created;
        }
    }
    if (!sample) return;
    TdvrEngineTargetStats targets{sizeof(TdvrEngineTargetStats), 1};
    if (main_camera) {
        try { targets = observe_target(target, game); }
        catch (...) {} // diagnostics must never unwind into the game's renderer
    }
    valid = valid && read(camera_object, after, sizeof(after))
        && read(static_cast<char*>(scene)+0x2208, &state_after, sizeof(state_after));
    const bool temporal_after_valid = temporal_sample
        && tdvr_observe_temporal(reinterpret_cast<uintptr_t>(scene),temporal_read,nullptr,temporal_after);
    std::lock_guard<std::mutex> lock(s.mutex);
    if (temporal_sample) {
        auto& temporal = s.temporal_stats;
        ++temporal.samples;
        temporal.last_sample_valid = temporal_before_valid && temporal_after_valid ? 1u : 0u;
        if (!temporal.last_sample_valid) ++temporal.rejected;
        else {
            temporal.before = temporal_before.state; temporal.after = temporal_after.state;
            tdvr_compare_temporal(temporal_before,temporal_after,temporal.scene_changed_mask,
                temporal.taa_changed_mask,temporal.descriptor_changed_mask);
        }
    }
    const auto samples = s.target_stats.samples+1;
    const auto required = targets.count && targets.count <= 8 ? (1u << targets.count)-1 : 0;
    const bool confirmed = required && targets.known_mask == required && targets.matching_extent_mask == required;
    const auto accepted = s.target_stats.valid_samples + (confirmed ? 1 : 0);
    const auto rejected = s.target_stats.rejected_samples + (confirmed ? 0 : 1);
    s.target_stats = targets;
    s.target_stats.samples = samples; s.target_stats.valid_samples = accepted; s.target_stats.rejected_samples = rejected;
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
    wchar_t xr_timing[2]{};
    if (GetEnvironmentVariableW(L"TDVR_XR_WORLD_TIMING", xr_timing, 2) == 1 && xr_timing[0] == L'1' && development_process()) {
        const auto capture = GetModuleHandleW(L"tdvr_capture.dll");
        if (capture) s.begin_xr_world = reinterpret_cast<BeginXrWorld>(GetProcAddress(capture, "TdvrBeginXrWorldFrame"));
    }
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
HRESULT WINAPI TdvrGetEngineTargetStats(TdvrEngineTargetStats* stats) {
    if (!stats || stats->size != sizeof(*stats) || stats->abi != 1) return E_INVALIDARG;
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex); *stats = s.target_stats; return S_OK;
}
HRESULT WINAPI TdvrGetTemporalStats(TdvrTemporalStats* stats) {
    if (!stats || stats->size != sizeof(*stats) || stats->abi != 1) return E_INVALIDARG;
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex); *stats = s.temporal_stats; return S_OK;
}
HRESULT WINAPI TdvrRequestEyeTargetProbe(void*) {
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.world_stats.hook_installed || !s.world_stats.main_camera_matches || !development_process())
        return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    if (s.eye_target_stats.phase) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    s.eye_target_stats.phase = 1; return S_OK;
}
HRESULT WINAPI TdvrGetEyeTargetProbeStats(TdvrEyeTargetProbeStats* stats) {
    if (!stats || stats->size != sizeof(*stats) || stats->abi != 1) return E_INVALIDARG;
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex); *stats = s.eye_target_stats; return S_OK;
}
HRESULT WINAPI TdvrRequestEyeHistoryProbe(void*) {
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.world_stats.hook_installed || !s.world_stats.main_camera_matches || !development_process())
        return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    if (s.eye_history_stats.phase) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    s.eye_history_stats.phase = 1; return S_OK;
}
HRESULT WINAPI TdvrGetEyeHistoryProbeStats(TdvrEyeHistoryProbeStats* stats) {
    if (!stats || stats->size != sizeof(*stats) || stats->abi != 1) return E_INVALIDARG;
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex); *stats = s.eye_history_stats; return S_OK;
}
HRESULT WINAPI TdvrRequestEyeTaaProbe(void*) {
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.world_stats.hook_installed || !s.world_stats.main_camera_matches || s.eye_history_stats.phase != 2 || !development_process())
        return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    if (s.eye_taa_stats.phase) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    s.eye_taa_stats.phase = 1; return S_OK;
}
HRESULT WINAPI TdvrGetEyeTaaProbeStats(TdvrEyeTaaProbeStats* stats) {
    if (!stats || stats->size != sizeof(*stats) || stats->abi != 1) return E_INVALIDARG;
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex); *stats = s.eye_taa_stats; return S_OK;
}
HRESULT WINAPI TdvrSubmitCameraOverride(const TdvrCameraOverrideRequest* request) {
    if (!request || request->size != sizeof(*request) || request->abi != 1) return E_INVALIDARG;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.world_stats.hook_installed || !s.world_stats.main_camera_matches || !development_process())
        return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    if (!s.override_camera.arm(*request, GetTickCount64())) return E_INVALIDARG;
    TdvrTrackedCameraRequest cancel{sizeof(TdvrTrackedCameraRequest),1};
    s.tracked_camera.arm(cancel,GetTickCount64());
    s.tracked_pending.store(false,std::memory_order_release);
    ++s.override_stats.accepted_requests;
    s.override_pending.store(s.override_camera.remaining(GetTickCount64()) != 0, std::memory_order_release);
    return S_OK;
}
HRESULT WINAPI TdvrSubmitTrackedCameraPreview(const TdvrTrackedCameraRequest* request) {
    if (!request || request->size != sizeof(*request) || request->abi != 1) return E_INVALIDARG;
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.begin_xr_world || !s.world_stats.main_camera_matches || !development_process())
        return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    if (!s.tracked_camera.arm(*request,GetTickCount64())) return E_INVALIDARG;
    TdvrCameraOverrideRequest cancel{sizeof(TdvrCameraOverrideRequest),1};
    s.override_camera.arm(cancel,GetTickCount64());
    s.override_pending.store(false,std::memory_order_release);
    s.tracked_pending.store(s.tracked_camera.remaining(GetTickCount64()) != 0,std::memory_order_release);
    ++s.override_stats.accepted_requests;
    return S_OK;
}
HRESULT WINAPI TdvrGetCameraOverrideStats(TdvrCameraOverrideStats* stats) {
    if (!stats || stats->size != sizeof(*stats) || stats->abi != 1) return E_INVALIDARG;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    *stats = s.override_stats;
    stats->remaining_calls = s.override_pending.load(std::memory_order_relaxed)
        ? s.override_camera.remaining(GetTickCount64()) : 0;
    if (s.tracked_pending.load(std::memory_order_relaxed)) stats->remaining_calls += s.tracked_camera.remaining(GetTickCount64());
    return S_OK;
}
