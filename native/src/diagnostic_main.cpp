#include "tdvr_d3d12_probe.h"
#include "tdvr_openxr_probe.h"
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
int runtime() {
    const tdvr::OpenXrDispatch dispatch{xrEnumerateInstanceExtensionProperties, xrCreateInstance,
        xrDestroyInstance, xrGetInstanceProperties, xrGetSystem, xrGetSystemProperties,
        xrEnumerateViewConfigurations, xrEnumerateViewConfigurationViews, xrGetInstanceProcAddr};
    const auto report = tdvr::probe_openxr(dispatch);
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
    return gpu(options);
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--runtime") return runtime();
        if (argc >= 2 && std::string(argv[1]) == "--gpu") {
            tdvr::D3d12ProbeOptions options;
            if (argc == 3 && std::string(argv[2]) == "--warp") options.use_warp = true;
            else if (argc != 2) { std::cerr << "Unknown GPU option\n"; return 1; }
            return gpu(options);
        }
        std::cout << "Usage: tdvr_diagnostic --gpu [--warp] | --runtime\n"
            << "--gpu: two-target D3D12 readback, no OpenXR calls.\n"
            << "--runtime: OpenXR instance/system/D3D12 preflight; may start installed runtime.\n"
            << "No session, swapchain, xrWaitFrame, game hooks or headset comfort test.\n";
        return argc == 1 || (argc == 2 && std::string(argv[1]) == "--help") ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Diagnostic failed: " << e.what() << '\n';
        return 1;
    }
}
