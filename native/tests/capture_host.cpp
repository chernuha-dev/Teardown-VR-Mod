#include "tdvr_capture.h"
#include "tdvr_render_trace.h"
#include "tdvr_target_trace.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <shlobj.h>
#include <filesystem>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <memory>
#include <d3dcompiler.h>
#include <cstring>

namespace {
#ifdef TDVR_EXTERNALLY_LOADED
using StartCapture = HRESULT (WINAPI*)();
using GetCaptureStats = HRESULT (WINAPI*)(TdvrCaptureStats*);
StartCapture externally_started = nullptr;
GetCaptureStats externally_stats = nullptr;
#define TdvrStartCapture externally_started
#define TdvrGetCaptureStats externally_stats
#endif
using Microsoft::WRL::ComPtr;
void require(bool ok, const char* text) { if (!ok) { std::cerr << text << '\n'; std::exit(1); } }
void check(HRESULT result, const char* text) {
    if (FAILED(result)) { std::cerr << text << " 0x" << std::hex << static_cast<uint32_t>(result) << '\n'; std::exit(1); }
}
uint64_t color_hash(uint32_t width, uint32_t height, std::array<uint8_t, 4> color) {
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            for (const auto v : color) { hash ^= v; hash *= 1099511628211ull; }
    return hash;
}
TdvrCaptureStats stats() {
    TdvrCaptureStats out{}; out.size = sizeof(out); out.abi = 1;
    check(TdvrGetCaptureStats(&out), "stats"); return out;
}
#ifndef TDVR_EXTERNALLY_LOADED
#include "trace_scene.h"
#endif
}
int main(int argc, char** argv) {
#ifdef TDVR_EXTERNALLY_LOADED
    const auto module = GetModuleHandleW(L"tdvr_capture.dll");
    require(module != nullptr, "launcher did not load the capture DLL");
    externally_started = reinterpret_cast<StartCapture>(GetProcAddress(module, "TdvrStartCapture"));
    externally_stats = reinterpret_cast<GetCaptureStats>(GetProcAddress(module, "TdvrGetCaptureStats"));
    require(externally_started && externally_stats, "injected exports missing");
    require(TdvrStartCapture() == S_FALSE, "capture was not initialized before child main");
    if (const auto* profile = std::getenv("TDVR_DEV_PROFILE")) {
        char folder[MAX_PATH]{};
        require(SHGetSpecialFolderPathA(nullptr, folder, CSIDL_LOCAL_APPDATA, FALSE) != FALSE
            && std::filesystem::path(folder) == std::filesystem::path(profile) / "AppData" / "Local",
            "child special folder escaped development profile");
        require(SHGetSpecialFolderPathA(nullptr, folder, CSIDL_PERSONAL, FALSE) != FALSE
            && std::filesystem::path(folder) == std::filesystem::path(profile) / "Documents",
            "child Documents escaped development profile");
        const auto* env = std::getenv("LOCALAPPDATA");
        require(env && std::filesystem::path(env) == std::filesystem::path(profile) / "AppData" / "Local",
            "CRT environment escaped development profile");
    }
#endif
    const bool legacy = argc == 2 && std::string(argv[1]) == "--legacy";
    const bool trace = argc == 2 && std::string(argv[1]) == "--trace";
    require(argc == 1 || legacy || trace, "usage: tdvr_capture_host [--legacy|--trace]");
#ifndef TDVR_EXTERNALLY_LOADED
    TdvrRenderTrace snapshot{};
    TdvrTargetTrace targets{};
    require(TdvrGetTargetTrace(&targets) == E_INVALIDARG, "bad target ABI accepted");
    require(TdvrResetTargetTrace(nullptr) == HRESULT_FROM_WIN32(ERROR_INVALID_STATE), "inactive target reset accepted");
    require(TdvrGetRenderTrace(&snapshot) == E_INVALIDARG, "bad trace ABI accepted");
    require(TdvrResetRenderTrace(nullptr) == HRESULT_FROM_WIN32(ERROR_INVALID_STATE), "unconfigured trace reset accepted");
    if (trace) check(TdvrEnableRenderTrace(nullptr), "enable render tracing");
#endif
    TdvrCaptureStats invalid{};
    require(TdvrGetCaptureStats(&invalid) == E_INVALIDARG, "bad ABI accepted");
    check(TdvrStartCapture(), "start capture");
    require(TdvrStartCapture() == S_FALSE, "second start must be idempotent");
    ComPtr<IDXGIFactory4> factory;
    if (legacy) check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    else check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    ComPtr<IDXGIFactory> factory0;
    check(CreateDXGIFactory(IID_PPV_ARGS(&factory0)), "CreateDXGIFactory");
    ComPtr<IDXGIAdapter> adapter;
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"tdvr offline capture host";
    require(RegisterClassW(&wc) != 0, "window class");
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"tdvr offline capture host", WS_OVERLAPPEDWINDOW,
        0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    require(window != nullptr, "hidden window");
    ComPtr<IDXGISwapChain3> swapchain;
    constexpr UINT width = 19, height = 11;
    if (legacy) {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = width; desc.BufferDesc.Height = height;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2; desc.OutputWindow = window; desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain> created;
        check(factory0->CreateSwapChain(queue.Get(), &desc, &created), "legacy swapchain");
        check(created.As(&swapchain), "chain3");
    } else {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width; desc.Height = height; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> created;
        check(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &created), "hwnd swapchain");
        check(created.As(&swapchain), "chain3");
    }
    require(stats().associations >= 1, "creation was not hooked/associated with its queue");
#ifndef TDVR_EXTERNALLY_LOADED
    std::unique_ptr<TraceScene> scene;
    if (trace) scene = std::make_unique<TraceScene>(device.Get());
#endif
    swapchain->Present(0, DXGI_PRESENT_TEST);
    require(stats().captured_frames == 0, "DXGI_PRESENT_TEST triggered a capture");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Fence> fence;
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.NumDescriptors = 2; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "rtv heap");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    check(list->Close(), "initial close");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "event");
    UINT64 fence_value = 0;
    auto draw = [&](bool green, bool use_present1) {
        check(allocator->Reset(), "allocator reset");
        check(list->Reset(allocator.Get(), nullptr), "list reset");
        ComPtr<ID3D12Resource> buffer;
        check(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)), "backbuffer");
        const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(buffer.Get(), nullptr, rtv);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = buffer.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        const float red[]{1,0,0,1}, g[]{0,1,0,1};
#ifndef TDVR_EXTERNALLY_LOADED
        // Runtime count prevents unrolling into different caller RVAs: repeat
        // the SAME binding call sites to exercise descriptor-write coalescing.
        if (scene) for (int repeat = 0; repeat < argc; ++repeat) {
            const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            const D3D12_CPU_DESCRIPTOR_HANDLE other{rtv.ptr+stride};
            device->CreateRenderTargetView(buffer.Get(), nullptr, other);
            device->CopyDescriptorsSimple(1, other, rtv, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            list->OMSetRenderTargets(2, &rtv, TRUE, nullptr); // copied destination must be unknown
            device->CreateRenderTargetView(buffer.Get(), nullptr, other);
            const D3D12_CPU_DESCRIPTOR_HANDLE reversed[]{other, rtv};
            list->OMSetRenderTargets(2, reversed, FALSE, nullptr);
            device->CopyDescriptors(1, &rtv, nullptr, 1, &other, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr); // range copy invalidates old metadata
            device->CreateRenderTargetView(buffer.Get(), nullptr, rtv);
        }
        if (scene) scene->render(list.Get(), rtv, static_cast<UINT>(buffer->GetDesc().Width), buffer->GetDesc().Height, green ? g : red);
        else
#endif
            list->ClearRenderTargetView(rtv, green ? g : red, 0, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        check(list->Close(), "close");
        ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
        if (use_present1) {
            DXGI_PRESENT_PARAMETERS params{};
            check(swapchain->Present1(0, 0, &params), "Present1");
        } else check(swapchain->Present(0, 0), "Present");
        check(queue->Signal(fence.Get(), ++fence_value), "signal");
        check(fence->SetEventOnCompletion(fence_value, event), "fence event");
        require(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "GPU completion");
        check(device->GetDeviceRemovedReason(), "device removed");
    };
    draw(false, false);
    auto first = stats();
    require(first.captured_frames == 1 && first.width == width && first.height == height
        && first.pixel_hash == color_hash(width, height, {255,0,0,255}), "Present pixels/row padding mismatch");
    draw(false, true);
    require(stats().captured_frames == 1, "diagnostic should capture once per size, not stall every frame");
    // The host deliberately retains a backbuffer: DXGI must keep rejecting this
    // invalid resize, and the hook must not falsify its HRESULT or reset capture.
    ComPtr<ID3D12Resource> retained;
    check(swapchain->GetBuffer(0, IID_PPV_ARGS(&retained)), "retained backbuffer");
    require(FAILED(swapchain->ResizeBuffers(2, 23, 17, DXGI_FORMAT_R8G8B8A8_UNORM, 0)), "invalid resize was changed to success");
    require(stats().resizes == 0 && stats().captured_frames == 1, "failed resize changed capture state");
    retained.Reset();
    check(swapchain->ResizeBuffers(2, 23, 17, DXGI_FORMAT_R8G8B8A8_UNORM, 0), "ResizeBuffers (capture must release buffers)");
    draw(true, true);
    const auto second = stats();
    require(second.captured_frames == 2 && second.resizes == 1 && second.width == 23 && second.height == 17
        && second.pixel_hash == color_hash(23, 17, {0,255,0,255}), "resized Present1 pixels mismatch");
    require(second.last_hresult == S_OK && second.unsupported == 0, "capture reported errors/unsupported hooks");
#ifndef TDVR_EXTERNALLY_LOADED
    snapshot.size = sizeof(snapshot); snapshot.abi = 1;
    check(TdvrGetRenderTrace(&snapshot), "render trace snapshot");
    targets.size = sizeof(targets); targets.abi = 1;
    check(TdvrGetTargetTrace(&targets), "target trace snapshot");
    if (trace) {
        std::cerr << "target stats: mask=" << targets.hook_mask << " bindings=" << targets.bindings
            << " writes=" << targets.descriptor_writes << " records=" << targets.record_count << " dropped=" << targets.dropped << '\n';
        require(targets.hook_mask == 15 && targets.bindings == 21 && targets.descriptor_writes == 21
            && targets.record_count == 12 && targets.dropped == 0, "target bindings missing or descriptor rewrites filled records");
        for (uint32_t i = 0; i < targets.record_count; ++i) {
            const auto& entry = targets.records[i];
            const auto pass = i%4;
            require(entry.depth > 0 && entry.depth <= 12, "target caller absent");
            require(entry.count == (pass < 2 ? 2u : 1u)
                && entry.contiguous == (pass == 0 ? 1u : 0u)
                && entry.known_mask == (pass == 0 ? 1u : pass == 1 ? 3u : pass == 2 ? 0u : 1u),
                "descriptor copy invalidation or contiguous/array binding mismatch");
            for (uint32_t eye = 0; eye < entry.count; ++eye) if (entry.known_mask & (1u << eye)) {
                const auto& view = entry.views[eye];
                require(view.creation && view.width == (i < 8 ? 19u : 23u) && view.height == (i < 8 ? 11u : 17u)
                    && view.resource_format == DXGI_FORMAT_R8G8B8A8_UNORM && view.samples == 1
                    && view.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D, "target shape stale after resize");
            }
            if (pass == 1) require(entry.views[0].creation > entry.views[1].creation, "array ordering lost");
        }
        require(snapshot.hook_mask == 7 && snapshot.commands == 9 && snapshot.dropped == 0
            && snapshot.stack_samples == 9 && snapshot.record_count >= 3, "GPU recording calls not traced");
        uint32_t kinds = 0;
        for (uint32_t i = 0; i < snapshot.record_count; ++i) {
            const auto& record = snapshot.records[i];
            require(record.depth > 0 && record.depth <= 24, "missing executable stack");
            kinds |= 1u << (record.command-1);
        }
        require(kinds == 7, "draw/indexed/dispatch stack coverage incomplete");
        require(TdvrEnableRenderTrace(nullptr) == HRESULT_FROM_WIN32(ERROR_INVALID_STATE), "late trace enable accepted");
        check(TdvrResetRenderTrace(nullptr), "reset trace window");
        check(TdvrGetRenderTrace(&snapshot), "reset trace stats");
        require(snapshot.commands == 0 && snapshot.record_count == 0 && snapshot.hook_mask == 7
            && snapshot.generation == 1, "trace reset lost hooks or kept stale records");
        check(TdvrResetTargetTrace(nullptr), "reset target trace");
        check(TdvrGetTargetTrace(&targets), "reset target snapshot");
        require(targets.bindings == 0 && targets.record_count == 0 && targets.hook_mask == 15
            && targets.descriptor_writes == 21, "target reset lost descriptor identities");
    } else require(snapshot.commands == 0 && snapshot.hook_mask == 0 && targets.bindings == 0
        && targets.hook_mask == 0, "tracing enabled without opt in");
#endif
    CloseHandle(event);
    swapchain.Reset();
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::cout << "Real DXGI " << (legacy ? "legacy" : "HWND")
        << " hooks: queue association, Present/Present1 pixel hashes and resize passed on WARP. No game or XR runtime started.\n";
}
