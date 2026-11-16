#include "tdvr_openxr_probe.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace tdvr {
namespace {
struct InstanceGuard {
    const OpenXrDispatch& api;
    XrInstance handle = XR_NULL_HANDLE;
    ~InstanceGuard() { if (handle != XR_NULL_HANDLE) api.destroy_instance(handle); }
};
bool unavailable(XrResult r) {
    return r == XR_ERROR_RUNTIME_UNAVAILABLE || r == XR_ERROR_FORM_FACTOR_UNAVAILABLE
        || r == XR_ERROR_FORM_FACTOR_UNSUPPORTED || r == XR_ERROR_EXTENSION_NOT_PRESENT
        || r == XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
}
// Count/fill can race with runtime enumeration changes. Retry, with a fixed bound.
template<class T, class Enumerate>
XrResult enumerate(std::vector<T>& out, const T& initial, Enumerate call) {
    for (int attempt = 0; attempt != 3; ++attempt) {
        uint32_t count = 0;
        XrResult r = call(0, &count, nullptr);
        if (r != XR_SUCCESS) return r;
        if (count > 4096) return XR_ERROR_LIMIT_REACHED;
        out.assign(count, initial);
        if (!count) return XR_SUCCESS;
        uint32_t written = 0;
        r = call(count, &written, out.data());
        if (r == XR_ERROR_SIZE_INSUFFICIENT) continue;
        if (r != XR_SUCCESS) return r;
        if (written > count) return XR_ERROR_RUNTIME_FAILURE;
        out.resize(written);
        return XR_SUCCESS;
    }
    return XR_ERROR_SIZE_INSUFFICIENT;
}
}

OpenXrProbeReport probe_openxr(const OpenXrDispatch& api) {
    OpenXrProbeReport out;
    InstanceGuard owner{api};
    auto& instance = owner.handle;
    auto finish = [&](XrResult result, const char* stage) {
        out.result = result;
        out.stage = stage;
        out.status = result == XR_SUCCESS ? ProbeStatus::ready
            : unavailable(result) ? ProbeStatus::unavailable : ProbeStatus::error;
        if (instance != XR_NULL_HANDLE) {
            const XrResult cleanup = api.destroy_instance(instance);
            instance = XR_NULL_HANDLE;
            if (cleanup != XR_SUCCESS) {
                out.status = ProbeStatus::error;
                out.result = cleanup;
                out.stage += "; xrDestroyInstance failed";
            }
        }
        return out;
    };
    if (!api.enumerate_extensions || !api.create_instance || !api.destroy_instance
        || !api.instance_properties || !api.get_system || !api.system_properties
        || !api.view_configurations || !api.view_sizes || !api.get_proc)
        return finish(XR_ERROR_VALIDATION_FAILURE, "incomplete dispatch");
    std::vector<XrExtensionProperties> extensions;
    XrExtensionProperties extension{};
    extension.type = XR_TYPE_EXTENSION_PROPERTIES;
    XrResult r = enumerate(extensions, extension, [&](uint32_t n, uint32_t* count, XrExtensionProperties* p) {
        return api.enumerate_extensions(nullptr, n, count, p);
    });
    if (r != XR_SUCCESS) return finish(r, "xrEnumerateInstanceExtensionProperties");
    const bool d3d12 = std::any_of(extensions.begin(), extensions.end(), [](const auto& e) {
        return std::strncmp(e.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME,
                            XR_MAX_EXTENSION_NAME_SIZE) == 0;
    });
    if (!d3d12) return finish(XR_ERROR_EXTENSION_NOT_PRESENT, "XR_KHR_D3D12_enable absent");
    XrInstanceCreateInfo create{};
    create.type = XR_TYPE_INSTANCE_CREATE_INFO;
    std::strcpy(create.applicationInfo.applicationName, "tdvr diagnostic");
    std::strcpy(create.applicationInfo.engineName, "standalone preflight");
    create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    const char* enabled[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    create.enabledExtensionCount = 1;
    create.enabledExtensionNames = enabled;
    r = api.create_instance(&create, &instance);
    if (r != XR_SUCCESS) return finish(r, "xrCreateInstance");
    XrInstanceProperties properties{};
    properties.type = XR_TYPE_INSTANCE_PROPERTIES;
    r = api.instance_properties(instance, &properties);
    if (r != XR_SUCCESS) return finish(r, "xrGetInstanceProperties");
    out.runtime_name.assign(properties.runtimeName, strnlen(properties.runtimeName, XR_MAX_RUNTIME_NAME_SIZE));
    out.runtime_version = properties.runtimeVersion;
    XrSystemGetInfo get{};
    get.type = XR_TYPE_SYSTEM_GET_INFO;
    get.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    r = api.get_system(instance, &get, &system);
    if (r != XR_SUCCESS) return finish(r, "xrGetSystem");
    XrSystemProperties system_properties{};
    system_properties.type = XR_TYPE_SYSTEM_PROPERTIES;
    r = api.system_properties(instance, system, &system_properties);
    if (r != XR_SUCCESS) return finish(r, "xrGetSystemProperties");
    out.system_name.assign(system_properties.systemName, strnlen(system_properties.systemName, XR_MAX_SYSTEM_NAME_SIZE));
    std::vector<XrViewConfigurationType> configurations;
    r = enumerate(configurations, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        [&](uint32_t n, uint32_t* count, XrViewConfigurationType* p) {
            return api.view_configurations(instance, system, n, count, p);
        });
    if (r != XR_SUCCESS) return finish(r, "xrEnumerateViewConfigurations");
    if (std::find(configurations.begin(), configurations.end(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        == configurations.end())
        return finish(XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED, "primary stereo unavailable");
    std::vector<XrViewConfigurationView> sizes;
    XrViewConfigurationView view{};
    view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    r = enumerate(sizes, view, [&](uint32_t n, uint32_t* count, XrViewConfigurationView* p) {
        return api.view_sizes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, count, p);
    });
    if (r != XR_SUCCESS) return finish(r, "xrEnumerateViewConfigurationViews");
    if (sizes.size() != 2) return finish(XR_ERROR_RUNTIME_FAILURE, "primary stereo requires exactly two views");
    for (size_t i = 0; i != 2; ++i) {
        const auto& v = sizes[i];
        if (!v.recommendedImageRectWidth || !v.recommendedImageRectHeight
            || !v.recommendedSwapchainSampleCount || v.recommendedImageRectWidth > v.maxImageRectWidth
            || v.recommendedImageRectHeight > v.maxImageRectHeight
            || v.recommendedSwapchainSampleCount > v.maxSwapchainSampleCount)
            return finish(XR_ERROR_RUNTIME_FAILURE, "invalid recommended view dimensions");
        out.views[i] = v;
    }
    PFN_xrVoidFunction address = nullptr;
    r = api.get_proc(instance, "xrGetD3D12GraphicsRequirementsKHR", &address);
    if (r != XR_SUCCESS) return finish(r, "xrGetInstanceProcAddr(D3D12 requirements)");
    if (!address) return finish(XR_ERROR_FUNCTION_UNSUPPORTED, "null D3D12 requirements function");
    XrGraphicsRequirementsD3D12KHR requirements{};
    requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;
    r = reinterpret_cast<PFN_xrGetD3D12GraphicsRequirementsKHR>(address)(instance, system, &requirements);
    if (r != XR_SUCCESS) return finish(r, "xrGetD3D12GraphicsRequirementsKHR");
    out.adapter_luid = requirements.adapterLuid;
    out.minimum_feature_level = requirements.minFeatureLevel;
    return finish(XR_SUCCESS, "D3D12 requirements queried; no session or frames submitted");
}
}
