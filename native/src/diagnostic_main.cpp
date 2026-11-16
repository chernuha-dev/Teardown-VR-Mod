#include "tdvr_d3d12_probe.h"
#include "tdvr_openxr_probe.h"
#include "tdvr_session_probe.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>

namespace {
int gpu(const tdvr::D3d12ProbeOptions& options) {
    const auto report = tdvr::run_d3d12_probe(options);
    for (const auto& adapter : report.adapters)
        std::cout << "adapter=" << adapter.description << " luid=" << std::hex
            << static_cast<uint32_t>(adapter.luid.high) << ':' << adapter.luid.low
            << " max_feature_level=0x" << adapter.max_feature_level << std::dec
            << " software=" << adapter.is_software << '\n';
    if (!report.success) {
        std::cerr << "GPU_ERROR: " << report.error << '\n';
        return 1;
    }
    std::cout << "GPU_PASS: " << report.selected_adapter.description
        << "; verified_pixels_per_eye=" << report.verified_pixels_per_eye
        << "; readback_row_pitch=" << report.readback_row_pitch
        << "; two independent clear/copy targets, no game rendering\n";
    return 0;
}
XrResult session_ready(XrInstance instance, XrSystemId system,
    const tdvr::OpenXrProbeReport& runtime, void*) {
    using Microsoft::WRL::ComPtr;
    auto checked = [](HRESULT result, const char* stage) {
        if (FAILED(result)) {
            std::cerr << stage << " failed HRESULT=0x" << std::hex << static_cast<uint32_t>(result) << std::dec << '\n';
            return false;
        }
        return true;
    };
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    if (!checked(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2")
        || !checked(factory->EnumAdapterByLuid(runtime.adapter_luid, IID_PPV_ARGS(&adapter)), "EnumAdapterByLuid")
        || !checked(D3D12CreateDevice(adapter.Get(), runtime.minimum_feature_level, IID_PPV_ARGS(&device)), "D3D12CreateDevice"))
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    D3D12_COMMAND_QUEUE_DESC description{};
    description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!checked(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue)), "CreateCommandQueue"))
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    const tdvr::SessionProbeDispatch api{xrCreateSession, xrDestroySession, xrEnumerateSwapchainFormats,
        xrCreateSwapchain, xrDestroySwapchain, xrEnumerateSwapchainImages};
    const auto result = tdvr::probe_session(api, instance, system, device.Get(), queue.Get());
    std::cout << "session_stage=" << result.stage << " result=" << static_cast<int>(result.result)
        << " format=" << result.format << " image_counts=" << result.image_count[0] << ',' << result.image_count[1]
        << " swapchain_destroy_attempts=" << result.destroyed_swapchains
        << " session_destroy_attempted=" << result.destroyed_session << '\n';
    return result.result;
}
int runtime(bool session) {
    const tdvr::OpenXrDispatch dispatch{xrEnumerateInstanceExtensionProperties, xrCreateInstance,
        xrDestroyInstance, xrGetInstanceProperties, xrGetSystem, xrGetSystemProperties,
        xrEnumerateViewConfigurations, xrEnumerateViewConfigurationViews, xrGetInstanceProcAddr};
    const auto report = tdvr::probe_openxr(dispatch, session ? session_ready : nullptr);
    std::cout << "runtime=" << report.runtime_name << " version=" << XR_VERSION_MAJOR(report.runtime_version)
        << '.' << XR_VERSION_MINOR(report.runtime_version) << '.' << XR_VERSION_PATCH(report.runtime_version)
        << " system=" << report.system_name << '\n';
    std::cout << "OpenXR stage=" << report.stage << " result=" << static_cast<int>(report.result) << '\n';
    if (report.status != tdvr::ProbeStatus::ready) {
        std::cout << (report.status == tdvr::ProbeStatus::unavailable ? "XR_UNAVAILABLE" : "XR_ERROR") << '\n';
        return report.status == tdvr::ProbeStatus::unavailable ? 2 : 1;
    }
    for (size_t i = 0; i != 2; ++i)
        std::cout << "eye=" << i << " recommended_size=" << report.views[i].recommendedImageRectWidth
            << 'x' << report.views[i].recommendedImageRectHeight << '\n';
    tdvr::D3d12ProbeOptions options;
    options.require_adapter = true;
    options.adapter_luid = {report.adapter_luid.LowPart, report.adapter_luid.HighPart};
    options.minimum_feature_level = static_cast<uint32_t>(report.minimum_feature_level);
    std::cout << "runtime_required_luid=" << std::hex << static_cast<uint32_t>(options.adapter_luid.high)
        << ':' << options.adapter_luid.low << " min_feature_level=0x" << options.minimum_feature_level << std::dec << '\n';
    return session ? 0 : gpu(options);
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--runtime") return runtime(false);
        if (argc == 2 && std::string(argv[1]) == "--session") return runtime(true);
        if (argc >= 2 && std::string(argv[1]) == "--gpu") {
            tdvr::D3d12ProbeOptions options;
            if (argc == 3 && std::string(argv[2]) == "--warp") options.use_warp = true;
            else if (argc != 2) { std::cerr << "Unknown GPU option\n"; return 1; }
            return gpu(options);
        }
        std::cout << "Usage: tdvr_diagnostic --gpu [--warp] | --runtime | --session\n"
            << "--gpu: two-target D3D12 readback, no OpenXR calls.\n"
            << "--runtime: OpenXR instance/system/D3D12 preflight; may start installed runtime.\n"
            << "--session: allocate/destroy an idle D3D12 session and two tiny swapchains.\n"
            << "No xrBeginSession, xrWaitFrame, submitted frames, game hooks or headset comfort test.\n";
        return argc == 1 || (argc == 2 && std::string(argv[1]) == "--help") ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Diagnostic failed: " << e.what() << '\n';
        return 1;
    }
}
