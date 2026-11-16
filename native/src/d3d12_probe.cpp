#include "tdvr_d3d12_probe.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace tdvr {
namespace {

using Microsoft::WRL::ComPtr;
constexpr UINT eye_width = 17;
constexpr UINT eye_height = 13;
constexpr UINT eye_count = 2;

std::string hresult_text(HRESULT result) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << static_cast<std::uint32_t>(result);
    return stream.str();
}

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed: " + hresult_text(result));
    }
}

std::string utf8(const wchar_t* value) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                       nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("Adapter description UTF-8 conversion failed");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(),
                            size, nullptr, nullptr) != size) {
        throw std::runtime_error("Adapter description UTF-8 conversion failed");
    }
    result.pop_back();
    return result;
}

bool same_luid(const AdapterLuid& left, const AdapterLuid& right) {
    return left.low == right.low && left.high == right.high;
}

bool valid_feature_level(std::uint32_t level) {
    return level == D3D_FEATURE_LEVEL_11_0 || level == D3D_FEATURE_LEVEL_11_1
        || level == D3D_FEATURE_LEVEL_12_0 || level == D3D_FEATURE_LEVEL_12_1
        || level == D3D_FEATURE_LEVEL_12_2;
}

D3d12AdapterInfo describe_adapter(IDXGIAdapter1* adapter) {
    DXGI_ADAPTER_DESC1 description{};
    check(adapter->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
    D3d12AdapterInfo info{};
    info.description = utf8(description.Description);
    info.luid = {description.AdapterLuid.LowPart, description.AdapterLuid.HighPart};
    info.vendor_id = description.VendorId;
    info.device_id = description.DeviceId;
    info.dedicated_video_memory = description.DedicatedVideoMemory;
    info.is_software = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        return info;
    }
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS supported{};
    supported.NumFeatureLevels = static_cast<UINT>(std::size(levels));
    supported.pFeatureLevelsRequested = levels;
    check(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &supported,
                                      sizeof(supported)), "CheckFeatureSupport(FEATURE_LEVELS)");
    info.supports_d3d12 = true;
    info.max_feature_level = supported.MaxSupportedFeatureLevel;
    return info;
}

// Members live until after the destructor body. If a submitted command list has
// not finished, remove this isolated diagnostic device before releasing its GPU
// resources. Device removal also resolves registered fence events. No infinite
// wait or orphan worker is needed on a hung/removed device.
struct GpuWork {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Device5> removable_device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    std::array<ComPtr<ID3D12Resource>, eye_count> eyes;
    std::array<ComPtr<ID3D12Resource>, eye_count> readbacks;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    bool submitted = false;
    bool completed = false;

    ~GpuWork() {
        if (submitted && !completed && removable_device) removable_device->RemoveDevice();
        if (event) CloseHandle(event);
    }
    GpuWork() = default;
    GpuWork(const GpuWork&) = delete;
    GpuWork& operator=(const GpuWork&) = delete;
};

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

void run_gpu_work(IDXGIAdapter1* adapter, const D3d12ProbeOptions& options,
                  D3d12ProbeReport& report) {
    GpuWork gpu;
    check(D3D12CreateDevice(adapter, static_cast<D3D_FEATURE_LEVEL>(options.minimum_feature_level),
                            IID_PPV_ARGS(&gpu.device)), "D3D12CreateDevice(required feature level)");
    check(gpu.device.As(&gpu.removable_device), "Query ID3D12Device5 (bounded failure cleanup)");

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(gpu.device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&gpu.queue)),
          "CreateCommandQueue(DIRECT)");
    check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&gpu.allocator)), "CreateCommandAllocator");
    check(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(),
          nullptr, IID_PPV_ARGS(&gpu.commands)), "CreateCommandList");

    D3D12_DESCRIPTOR_HEAP_DESC descriptor_description{};
    descriptor_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    descriptor_description.NumDescriptors = eye_count;
    check(gpu.device->CreateDescriptorHeap(&descriptor_description, IID_PPV_ARGS(&gpu.descriptors)),
          "CreateDescriptorHeap(RTV)");

    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = eye_width;
    texture.Height = eye_height;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    gpu.device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, &rows, &row_size, &total_bytes);
    if (total_bytes == std::numeric_limits<UINT64>::max() || rows != eye_height
        || row_size != eye_width * 4 || footprint.Footprint.RowPitch < row_size
        || total_bytes > std::numeric_limits<SIZE_T>::max()) {
        throw std::runtime_error("GetCopyableFootprints returned an invalid readback layout");
    }
    report.readback_row_pitch = footprint.Footprint.RowPitch;

    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    const auto readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
    constexpr std::array<std::array<float, 4>, eye_count> colors{{
        {{1.0f, 0.0f, 0.0f, 1.0f}}, {{0.0f, 1.0f, 0.0f, 1.0f}}}};
    auto rtv = gpu.descriptors->GetCPUDescriptorHandleForHeapStart();
    const UINT descriptor_stride = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT eye = 0; eye < eye_count; ++eye) {
        D3D12_CLEAR_VALUE clear{};
        clear.Format = texture.Format;
        for (UINT channel = 0; channel < 4; ++channel) clear.Color[channel] = colors[eye][channel];
        check(gpu.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture,
              D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&gpu.eyes[eye])),
              "CreateCommittedResource(eye render target)");
        check(gpu.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gpu.readbacks[eye])),
              "CreateCommittedResource(readback buffer)");
        gpu.device->CreateRenderTargetView(gpu.eyes[eye].Get(), nullptr, rtv);
        gpu.commands->ClearRenderTargetView(rtv, colors[eye].data(), 0, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = gpu.eyes[eye].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        gpu.commands->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = gpu.eyes[eye].Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = gpu.readbacks[eye].Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        gpu.commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        rtv.ptr += descriptor_stride;
    }
    check(gpu.commands->Close(), "Close command list");
    check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)), "CreateFence");
    gpu.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!gpu.event) check(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");

    ID3D12CommandList* lists[]{gpu.commands.Get()};
    gpu.submitted = true;
    gpu.queue->ExecuteCommandLists(1, lists);
    check(gpu.queue->Signal(gpu.fence.Get(), 1), "CommandQueue::Signal");
    check(gpu.fence->SetEventOnCompletion(1, gpu.event), "Fence::SetEventOnCompletion");
    const DWORD wait = WaitForSingleObject(gpu.event, options.timeout_ms);
    if (wait == WAIT_TIMEOUT) {
        throw std::runtime_error("GPU fence timed out after " + std::to_string(options.timeout_ms)
                                 + " ms; removing diagnostic device before cleanup");
    }
    if (wait == WAIT_FAILED) check(HRESULT_FROM_WIN32(GetLastError()), "WaitForSingleObject");
    if (wait != WAIT_OBJECT_0) throw std::runtime_error("Unexpected GPU fence wait result");
    const UINT64 completed = gpu.fence->GetCompletedValue();
    check(gpu.device->GetDeviceRemovedReason(), "GPU device removed during diagnostic");
    if (completed == std::numeric_limits<UINT64>::max() || completed < 1) {
        throw std::runtime_error("GPU fence signaled without successful work completion");
    }
    gpu.completed = true;

    constexpr std::array<std::array<std::uint8_t, 4>, eye_count> expected{{
        {{255, 0, 0, 255}}, {{0, 255, 0, 255}}}};
    for (UINT eye = 0; eye < eye_count; ++eye) {
        void* mapped = nullptr;
        const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_bytes)};
        check(gpu.readbacks[eye]->Map(0, &read_range, &mapped), "Map readback");
        const auto* bytes = static_cast<const std::uint8_t*>(mapped);
        bool matches = bytes != nullptr;
        for (UINT y = 0; matches && y < eye_height; ++y) {
            const auto* row = bytes + footprint.Offset + static_cast<SIZE_T>(y) * footprint.Footprint.RowPitch;
            for (UINT x = 0; matches && x < eye_width; ++x) {
                for (UINT channel = 0; channel < 4; ++channel) {
                    if (row[x * 4 + channel] != expected[eye][channel]) matches = false;
                }
            }
        }
        const D3D12_RANGE no_writes{0, 0};
        gpu.readbacks[eye]->Unmap(0, &no_writes);
        if (!matches) throw std::runtime_error("Pixel readback mismatch for eye " + std::to_string(eye));
    }
    report.verified_pixels_per_eye = eye_width * eye_height;
}

} // namespace

D3d12ProbeReport run_d3d12_probe(const D3d12ProbeOptions& options) {
    D3d12ProbeReport report;
    try {
        if (options.use_warp && options.require_adapter) {
            throw std::runtime_error("WARP cannot satisfy a required runtime adapter LUID");
        }
        if (!valid_feature_level(options.minimum_feature_level)) {
            throw std::runtime_error("Unsupported minimum D3D feature level value");
        }
        if (options.timeout_ms == 0 || options.timeout_ms > 60000) {
            throw std::runtime_error("GPU timeout must be between 1 and 60000 ms");
        }
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        ComPtr<IDXGIAdapter1> selected;
        for (UINT index = 0; ; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT result = factory->EnumAdapters1(index, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            check(result, "EnumAdapters1");
            const D3d12AdapterInfo info = describe_adapter(adapter.Get());
            report.adapters.push_back(info);
            const bool requested = options.require_adapter && same_luid(info.luid, options.adapter_luid);
            const bool default_hardware = !options.require_adapter && !info.is_software
                && info.supports_d3d12 && info.max_feature_level >= options.minimum_feature_level;
            if (!options.use_warp && !selected && (requested || default_hardware)) {
                selected = adapter;
                report.selected_adapter = info;
            }
        }
        if (options.use_warp) {
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&selected)), "EnumWarpAdapter");
            report.selected_adapter = describe_adapter(selected.Get());
        }
        if (!selected) {
            throw std::runtime_error(options.require_adapter ? "Required runtime adapter LUID was not found"
                                     : "No hardware adapter supports the required D3D12 feature level");
        }
        if (!report.selected_adapter.supports_d3d12
            || report.selected_adapter.max_feature_level < options.minimum_feature_level) {
            throw std::runtime_error("Selected adapter does not support the required D3D12 feature level");
        }
        run_gpu_work(selected.Get(), options, report);
        report.success = true;
    } catch (const std::exception& error) {
        report.error = error.what();
    }
    return report;
}

} // namespace tdvr
