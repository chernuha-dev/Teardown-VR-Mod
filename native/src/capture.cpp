#include "tdvr_capture.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <shlobj.h>
#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <string>

namespace {
using Microsoft::WRL::ComPtr;
using Factory = HRESULT (WINAPI*)(REFIID, void**);
using FolderPath = BOOL (WINAPI*)(HWND, LPSTR, int, BOOL);
using Factory2 = HRESULT (WINAPI*)(UINT, REFIID, void**);
using CreateSwapchain = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateHwnd = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using Present = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1 = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using Resize = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

struct CaptureWork {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12Resource> source;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    ~CaptureWork() { if (event) CloseHandle(event); }
};
struct Chain {
    // Swapchain is observed without taking a reference: retaining it prevents game
    // resize/destruction. The queue reference lives for this process diagnostic.
    ComPtr<ID3D12CommandQueue> queue;
    bool captured = false;
    bool failed = false;
};
struct State {
    std::recursive_mutex mutex;
    TdvrCaptureStats stats{sizeof(TdvrCaptureStats), 1};
    bool started = false;
    std::wstring profile;
    FolderPath folder_path = nullptr;
    Factory factory = nullptr, factory1 = nullptr;
    Factory2 factory2 = nullptr;
    CreateSwapchain create_swapchain = nullptr;
    CreateHwnd create_hwnd = nullptr;
    Present present = nullptr;
    Present1 present1 = nullptr;
    Resize resize = nullptr;
    void* create_swapchain_target = nullptr;
    void* create_hwnd_target = nullptr;
    void* present_target = nullptr;
    void* present1_target = nullptr;
    void* resize_target = nullptr;
    std::map<IDXGISwapChain*, Chain> chains;
    // On timeout, submitted GPU allocations MUST outlive unfinished work. Retain
    // that work until process exit; NEVER RemoveDevice on the game's device.
    CaptureWork* quarantined = nullptr;
};
State& state() { static auto* value = new State; return *value; }
BOOL WINAPI folder_path(HWND window, LPSTR path, int folder, BOOL create) {
    auto& s = state();
    const wchar_t* suffix = nullptr;
    switch (folder & ~CSIDL_FLAG_MASK) {
        case CSIDL_LOCAL_APPDATA: suffix = L"\\AppData\\Local"; break;
        case CSIDL_APPDATA: suffix = L"\\AppData\\Roaming"; break;
        case CSIDL_PERSONAL: suffix = L"\\Documents"; break;
        case CSIDL_PROFILE: suffix = L""; break;
        default: return s.folder_path(window, path, folder, create);
    }
    try {
        if (!path) return FALSE;
        const auto redirected = s.profile + suffix;
        BOOL default_character = FALSE;
        const int count = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, redirected.c_str(), -1,
            path, MAX_PATH, nullptr, &default_character);
        if (count <= 0 || default_character) { path[0] = 0; return FALSE; }
        return TRUE;
    } catch (...) { if (path) path[0] = 0; return FALSE; }
}
void* method(IUnknown* object, size_t slot) { return (*reinterpret_cast<void***>(object))[slot]; }
template<class T> bool hook_method(void* target, void* detour, T& original, void*& registered) {
    if (registered) return registered == target;
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(&original)) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) { MH_RemoveHook(target); original = nullptr; return false; }
    registered = target;
    return true;
}

HRESULT capture(IDXGISwapChain* swapchain, Chain& chain) {
    auto& s = state();
    if (s.quarantined) return DXGI_ERROR_DEVICE_HUNG;
    // A normal unique_ptr cannot be used until completion is known: an error after
    // ExecuteCommandLists must not release its GPU allocations.
    auto* work = new CaptureWork;
    bool submitted = false;
    auto finish = [&](HRESULT result) {
        if (submitted) s.quarantined = work;
        else delete work;
        return result;
    };
    work->queue = chain.queue;
    HRESULT hr = chain.queue->GetDevice(IID_PPV_ARGS(&work->device));
    if (FAILED(hr)) return finish(hr);
    ComPtr<IDXGISwapChain3> chain3;
    hr = swapchain->QueryInterface(IID_PPV_ARGS(&chain3));
    if (FAILED(hr)) return finish(hr);
    hr = swapchain->GetBuffer(chain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&work->source));
    if (FAILED(hr)) return finish(hr);
    const auto desc = work->source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1
        || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.Width > 8192 || desc.Height > 8192
        || (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return finish(DXGI_ERROR_UNSUPPORTED);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0; UINT64 row_size = 0, bytes = 0;
    work->device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &bytes);
    if (!bytes || bytes > 512ull * 1024 * 1024 || row_size != desc.Width * 4 || rows != desc.Height)
        return finish(E_INVALIDARG);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = work->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&work->readback));
    if (FAILED(hr)) return finish(hr);
    hr = work->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&work->allocator));
    if (FAILED(hr)) return finish(hr);
    hr = work->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, work->allocator.Get(),
        nullptr, IID_PPV_ARGS(&work->list));
    if (FAILED(hr)) return finish(hr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = work->source.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    work->list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = work->source.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = work->readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    work->list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    work->list->ResourceBarrier(1, &barrier);
    hr = work->list->Close();
    if (FAILED(hr)) return finish(hr);
    hr = work->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&work->fence));
    if (FAILED(hr)) return finish(hr);
    work->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!work->event) return finish(HRESULT_FROM_WIN32(GetLastError()));
    ID3D12CommandList* lists[]{work->list.Get()};
    submitted = true;
    work->queue->ExecuteCommandLists(1, lists);
    hr = work->queue->Signal(work->fence.Get(), 1);
    if (FAILED(hr)) return finish(hr);
    hr = work->fence->SetEventOnCompletion(1, work->event);
    if (FAILED(hr)) return finish(hr);
    const auto waited = WaitForSingleObject(work->event, 2000);
    if (waited != WAIT_OBJECT_0) return finish(waited == WAIT_TIMEOUT ? DXGI_ERROR_WAIT_TIMEOUT : E_FAIL);
    const auto completed = work->fence->GetCompletedValue();
    hr = work->device->GetDeviceRemovedReason();
    if (FAILED(hr) || completed == std::numeric_limits<UINT64>::max()) return finish(FAILED(hr) ? hr : DXGI_ERROR_DEVICE_REMOVED);
    if (completed < 1) return finish(E_FAIL);
    submitted = false;
    void* data = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(bytes)};
    hr = work->readback->Map(0, &read_range, &data);
    if (FAILED(hr)) return finish(hr);
    uint64_t hash = 14695981039346656037ull;
    for (UINT y = 0; y != desc.Height; ++y) {
        const auto* row = static_cast<const uint8_t*>(data) + footprint.Offset + y * footprint.Footprint.RowPitch;
        for (UINT64 x = 0; x != row_size; ++x) { hash ^= row[x]; hash *= 1099511628211ull; }
    }
    const D3D12_RANGE no_writes{};
    work->readback->Unmap(0, &no_writes);
    s.stats.width = static_cast<uint32_t>(desc.Width);
    s.stats.height = desc.Height;
    s.stats.pixel_hash = hash;
    ++s.stats.captured_frames;
    return finish(S_OK);
}

void observe_present(IDXGISwapChain* object, UINT flags) noexcept {
    auto& s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    ++s.stats.presents;
    if (flags & DXGI_PRESENT_TEST) return;
    const auto found = s.chains.find(object);
    if (found == s.chains.end() || found->second.captured || found->second.failed) return;
    try {
        s.stats.last_hresult = capture(object, found->second);
        found->second.captured = SUCCEEDED(s.stats.last_hresult);
        found->second.failed = !found->second.captured;
    } catch (...) { found->second.failed = true; s.stats.last_hresult = E_FAIL; }
}
HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* object, UINT sync, UINT flags) {
    observe_present(object, flags);
    return state().present(object, sync, flags);
}
HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain1* object, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    observe_present(object, flags);
    return state().present1(object, sync, flags, params);
}
HRESULT STDMETHODCALLTYPE resize(IDXGISwapChain* object, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    auto& s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    const auto hr = s.resize(object, count, width, height, format, flags);
    if (SUCCEEDED(hr)) {
        ++s.stats.resizes;
        const auto found = s.chains.find(object);
        if (found != s.chains.end()) found->second.captured = found->second.failed = false;
    }
    return hr;
}
void associate(IUnknown* device, IDXGISwapChain* object) noexcept {
    if (!device || !object) return;
    try {
        auto& s = state();
        std::lock_guard<std::recursive_mutex> lock(s.mutex);
        ComPtr<ID3D12CommandQueue> queue;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            ++s.stats.unsupported; return;
        }
        if (!hook_method(method(object, 8), reinterpret_cast<void*>(present), s.present, s.present_target)
            || !hook_method(method(object, 13), reinterpret_cast<void*>(resize), s.resize, s.resize_target)) {
            ++s.stats.unsupported; return;
        }
        ComPtr<IDXGISwapChain1> chain1;
        if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&chain1)))) {
            if (!hook_method(method(chain1.Get(), 22), reinterpret_cast<void*>(present1), s.present1, s.present1_target)) {
                ++s.stats.unsupported; return;
            }
        }
        s.chains[object] = Chain{queue, false, false};
        ++s.stats.associations;
    } catch (...) { state().stats.last_hresult = E_FAIL; }
}
HRESULT STDMETHODCALLTYPE create_swapchain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    const auto hr = state().create_swapchain(factory, device, desc, out);
    if (SUCCEEDED(hr) && out) associate(device, *out);
    return hr;
}
HRESULT STDMETHODCALLTYPE create_hwnd(IDXGIFactory2* factory, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** out) {
    const auto hr = state().create_hwnd(factory, device, hwnd, desc, fullscreen, output, out);
    if (SUCCEEDED(hr) && out) associate(device, *out);
    return hr;
}
void observe_factory(void** value) noexcept {
    if (!value || !*value) return;
    try {
        auto& s = state();
        std::lock_guard<std::recursive_mutex> lock(s.mutex);
        ComPtr<IDXGIFactory> factory;
        auto* unknown = static_cast<IUnknown*>(*value);
        if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&factory)))) return;
        if (!hook_method(method(factory.Get(), 10), reinterpret_cast<void*>(create_swapchain), s.create_swapchain, s.create_swapchain_target))
            ++s.stats.unsupported;
        ComPtr<IDXGIFactory2> factory2;
        if (SUCCEEDED(factory.As(&factory2)) && !hook_method(method(factory2.Get(), 15), reinterpret_cast<void*>(create_hwnd), s.create_hwnd, s.create_hwnd_target))
            ++s.stats.unsupported;
    } catch (...) { state().stats.last_hresult = E_FAIL; }
}
HRESULT WINAPI factory(REFIID iid, void** out) {
    const auto hr = state().factory(iid, out);
    if (SUCCEEDED(hr)) observe_factory(out);
    return hr;
}
HRESULT WINAPI factory1(REFIID iid, void** out) {
    const auto hr = state().factory1(iid, out);
    if (SUCCEEDED(hr)) observe_factory(out);
    return hr;
}
HRESULT WINAPI factory2(UINT flags, REFIID iid, void** out) {
    const auto hr = state().factory2(flags, iid, out);
    if (SUCCEEDED(hr)) observe_factory(out);
    return hr;
}
}

HRESULT WINAPI TdvrStartCapture() {
    auto& s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (s.started) return S_FALSE;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&TdvrStartCapture), &module)) return HRESULT_FROM_WIN32(GetLastError());
    wchar_t directory[MAX_PATH]{};
    if (!GetSystemDirectoryW(directory, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    const auto path = std::wstring(directory) + L"\\dxgi.dll";
    HMODULE dxgi = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dxgi) return HRESULT_FROM_WIN32(GetLastError());
    wchar_t loaded_path[MAX_PATH]{};
    if (!GetModuleFileNameW(dxgi, loaded_path, MAX_PATH) || _wcsicmp(loaded_path, path.c_str()) != 0)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); // Do not stack over v0.22/proxies.
    const auto sound = GetModuleHandleW(L"dsound.dll");
    if (sound) {
        const auto system_sound = std::wstring(directory) + L"\\dsound.dll";
        if (!GetModuleFileNameW(sound, loaded_path, MAX_PATH) || _wcsicmp(loaded_path, system_sound.c_str()) != 0)
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); // Keep live v1.1.0 separate.
    }
    const auto status = MH_Initialize();
    if (status != MH_OK) return E_FAIL; // Own a separate statically linked MinHook instance.
    void* targets[]{reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory")),
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")),
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2"))};
    void* detours[]{reinterpret_cast<void*>(factory), reinterpret_cast<void*>(factory1), reinterpret_cast<void*>(factory2)};
    void** originals[]{reinterpret_cast<void**>(&s.factory), reinterpret_cast<void**>(&s.factory1), reinterpret_cast<void**>(&s.factory2)};
    for (size_t i = 0; i != 3; ++i) {
        if (!targets[i] || MH_CreateHook(targets[i], detours[i], originals[i]) != MH_OK) {
            MH_Uninitialize(); return E_FAIL;
        }
    }
    if (!s.profile.empty()) {
        const auto shell_path = std::wstring(directory) + L"\\shell32.dll";
        const auto shell = LoadLibraryExW(shell_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto folder_target = shell ? reinterpret_cast<void*>(GetProcAddress(shell, "SHGetSpecialFolderPathA")) : nullptr;
        if (!folder_target || MH_CreateHook(folder_target, reinterpret_cast<void*>(folder_path),
            reinterpret_cast<void**>(&s.folder_path)) != MH_OK || MH_QueueEnableHook(folder_target) != MH_OK) {
            MH_Uninitialize(); return E_FAIL;
        }
    }
    for (const auto target : targets)
        if (MH_QueueEnableHook(target) != MH_OK) { MH_Uninitialize(); return E_FAIL; }
    if (MH_ApplyQueued() != MH_OK) { MH_Uninitialize(); return E_FAIL; }
    s.started = true;
    return S_OK;
}
HRESULT WINAPI TdvrConfigureDevelopmentProfile(const wchar_t* root) {
    if (!root || !*root) return E_INVALIDARG;
    auto& s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (s.started) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    try {
        const std::wstring profile(root);
        if (profile.size() > MAX_PATH - 40 || profile.size() < 4 || profile[1] != L':'
            || (profile[2] != L'\\' && profile[2] != L'/')) return E_INVALIDARG;
        // Directories are prepared by the development launcher, before child main.
        const auto local = profile + L"\\AppData\\Local";
        const auto roaming = profile + L"\\AppData\\Roaming";
        if (!SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str())
            || !SetEnvironmentVariableW(L"APPDATA", roaming.c_str())
            || !SetEnvironmentVariableW(L"USERPROFILE", profile.c_str())
            || !SetEnvironmentVariableW(L"TDVR_DEV_PROFILE", profile.c_str()))
            return HRESULT_FROM_WIN32(GetLastError());
        s.profile = profile;
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}
HRESULT WINAPI TdvrGetCaptureStats(TdvrCaptureStats* stats) {
    if (!stats || stats->size != sizeof(TdvrCaptureStats) || stats->abi != 1) return E_INVALIDARG;
    auto& s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    *stats = s.stats;
    return S_OK;
}
