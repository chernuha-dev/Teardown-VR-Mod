#pragma once
#include <windows.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <array>
#include <string>

namespace tdvr {
// Deliberately no session/space/frame/swapchain entry points: this is preflight only.
struct OpenXrDispatch {
    PFN_xrEnumerateInstanceExtensionProperties enumerate_extensions;
    PFN_xrCreateInstance create_instance;
    PFN_xrDestroyInstance destroy_instance;
    PFN_xrGetInstanceProperties instance_properties;
    PFN_xrGetSystem get_system;
    PFN_xrGetSystemProperties system_properties;
    PFN_xrEnumerateViewConfigurations view_configurations;
    PFN_xrEnumerateViewConfigurationViews view_sizes;
    PFN_xrGetInstanceProcAddr get_proc;
};
enum class ProbeStatus { ready, unavailable, error };
struct OpenXrProbeReport {
    ProbeStatus status = ProbeStatus::error;
    XrResult result = XR_SUCCESS;
    std::string stage;
    std::string runtime_name;
    XrVersion runtime_version = 0;
    std::string system_name;
    LUID adapter_luid{};
    D3D_FEATURE_LEVEL minimum_feature_level = D3D_FEATURE_LEVEL_11_0;
    std::array<XrViewConfigurationView, 2> views{};
};
OpenXrProbeReport probe_openxr(const OpenXrDispatch& api);
}
