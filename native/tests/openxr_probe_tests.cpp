#include "tdvr_openxr_probe.h"
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
struct Fake {
    XrResult enumeration = XR_SUCCESS;
    XrResult creation = XR_SUCCESS;
    XrResult system = XR_SUCCESS;
    XrResult requirements = XR_SUCCESS;
    XrResult destruction = XR_SUCCESS;
    bool extension = true, stereo = true, null_function = false, invalid_size = false;
    uint32_t eye_count = 2;
    bool grow_once = false, always_grow = false;
    unsigned creates = 0, destroys = 0, requirement_calls = 0, fills = 0;
} fake;
const auto instance = reinterpret_cast<XrInstance>(static_cast<uintptr_t>(1));
XrResult XRAPI_PTR extensions(const char* layer, uint32_t capacity, uint32_t* count, XrExtensionProperties* out) {
    require(layer == nullptr, "unexpected API layer");
    if (fake.enumeration != XR_SUCCESS) return fake.enumeration;
    *count = fake.extension ? 1 : 0;
    if (capacity && fake.extension) {
        require(out && out[0].type == XR_TYPE_EXTENSION_PROPERTIES, "extension type missing");
        ++fake.fills;
        if (fake.always_grow || (fake.grow_once && fake.fills == 1)) return XR_ERROR_SIZE_INSUFFICIENT;
        std::strcpy(out[0].extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
    }
    return XR_SUCCESS;
}
XrResult XRAPI_PTR create(const XrInstanceCreateInfo* info, XrInstance* out) {
    ++fake.creates;
    require(info->type == XR_TYPE_INSTANCE_CREATE_INFO && info->enabledExtensionCount == 1
        && std::strcmp(info->enabledExtensionNames[0], XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0,
        "D3D12 extension not enabled at instance creation");
    if (fake.creation != XR_SUCCESS) return fake.creation;
    *out = instance;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR destroy(XrInstance value) {
    require(value == instance, "wrong instance destroyed");
    ++fake.destroys;
    return fake.destruction;
}
XrResult XRAPI_PTR properties(XrInstance, XrInstanceProperties* out) {
    require(out->type == XR_TYPE_INSTANCE_PROPERTIES, "instance property type");
    std::strcpy(out->runtimeName, "fake runtime");
    out->runtimeVersion = XR_MAKE_VERSION(1, 2, 3);
    return XR_SUCCESS;
}
XrResult XRAPI_PTR system(XrInstance, const XrSystemGetInfo* info, XrSystemId* out) {
    require(info->type == XR_TYPE_SYSTEM_GET_INFO && info->formFactor == XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
        "wrong form factor");
    *out = 7;
    return fake.system;
}
XrResult XRAPI_PTR system_properties(XrInstance, XrSystemId id, XrSystemProperties* out) {
    require(id == 7 && out->type == XR_TYPE_SYSTEM_PROPERTIES, "system property type/id");
    std::strcpy(out->systemName, "fake HMD");
    return XR_SUCCESS;
}
XrResult XRAPI_PTR configurations(XrInstance, XrSystemId, uint32_t capacity, uint32_t* count, XrViewConfigurationType* out) {
    *count = 1;
    if (capacity) *out = fake.stereo ? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR sizes(XrInstance, XrSystemId, XrViewConfigurationType type,
                       uint32_t capacity, uint32_t* count, XrViewConfigurationView* out) {
    require(type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, "wrong stereo configuration");
    *count = fake.eye_count;
    if (capacity) for (uint32_t i = 0; i < *count; ++i) {
        require(out[i].type == XR_TYPE_VIEW_CONFIGURATION_VIEW, "view type missing");
        out[i].recommendedImageRectWidth = fake.invalid_size ? 0 : 1400 + i * 100;
        out[i].recommendedImageRectHeight = 1600;
        out[i].maxImageRectWidth = out[i].maxImageRectHeight = 4096;
        out[i].recommendedSwapchainSampleCount = 1;
        out[i].maxSwapchainSampleCount = 4;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_PTR requirements(XrInstance value, XrSystemId id, XrGraphicsRequirementsD3D12KHR* out) {
    require(value == instance && id == 7 && out->type == XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR,
        "bad requirements arguments");
    ++fake.requirement_calls;
    out->adapterLuid = {123, -456};
    out->minFeatureLevel = D3D_FEATURE_LEVEL_12_0;
    return fake.requirements;
}
XrResult XRAPI_PTR proc(XrInstance, const char* name, PFN_xrVoidFunction* out) {
    require(std::strcmp(name, "xrGetD3D12GraphicsRequirementsKHR") == 0, "unexpected proc requested");
    *out = fake.null_function ? nullptr : reinterpret_cast<PFN_xrVoidFunction>(requirements);
    return XR_SUCCESS;
}
const tdvr::OpenXrDispatch api{extensions, create, destroy, properties, system, system_properties,
                              configurations, sizes, proc};
}
int main() {
    auto result = tdvr::probe_openxr(api);
    require(result.status == tdvr::ProbeStatus::ready && fake.creates == 1 && fake.destroys == 1
        && fake.requirement_calls == 1, "normal preflight failed or leaked instance");
    require(result.adapter_luid.LowPart == 123 && result.adapter_luid.HighPart == -456
        && result.minimum_feature_level == D3D_FEATURE_LEVEL_12_0, "runtime GPU requirements lost");
    require(result.views[0].recommendedImageRectWidth == 1400
        && result.views[1].recommendedImageRectWidth == 1500, "independent eye sizes lost");
    require(result.runtime_name == "fake runtime" && result.runtime_version == XR_MAKE_VERSION(1,2,3), "runtime identity lost");
    fake = {}; fake.enumeration = XR_ERROR_RUNTIME_UNAVAILABLE;
    result = tdvr::probe_openxr(api);
    require(result.status == tdvr::ProbeStatus::unavailable && fake.creates == 0, "missing runtime mishandled");
    fake = {}; fake.extension = false;
    result = tdvr::probe_openxr(api);
    require(result.status == tdvr::ProbeStatus::unavailable && fake.creates == 0, "missing D3D12 extension accepted");
    fake = {}; fake.creation = XR_ERROR_INITIALIZATION_FAILED;
    result = tdvr::probe_openxr(api);
    require(result.status == tdvr::ProbeStatus::error && fake.destroys == 0, "failed instance mishandled");
    fake = {}; fake.system = XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    result = tdvr::probe_openxr(api);
    require(result.status == tdvr::ProbeStatus::unavailable && fake.destroys == 1
        && fake.requirement_calls == 0, "no HMD should stop before GPU requirements and clean up");
    fake = {}; fake.stereo = false;
    result = tdvr::probe_openxr(api);
    require(result.status != tdvr::ProbeStatus::ready && fake.destroys == 1, "mono accepted as stereo");
    fake = {}; fake.eye_count = 1;
    result = tdvr::probe_openxr(api);
    require(result.status == tdvr::ProbeStatus::error && fake.requirement_calls == 0, "one view accepted");
    fake = {}; fake.invalid_size = true;
    result = tdvr::probe_openxr(api);
    require(result.status == tdvr::ProbeStatus::error && fake.destroys == 1, "zero view dimensions accepted");
    fake = {}; fake.null_function = true;
    result = tdvr::probe_openxr(api);
    require(result.status == tdvr::ProbeStatus::error && fake.destroys == 1, "null extension proc accepted");
    fake = {}; fake.requirements = XR_ERROR_RUNTIME_FAILURE;
    result = tdvr::probe_openxr(api);
    require(result.status == tdvr::ProbeStatus::error && fake.destroys == 1, "requirements failure lost");
    fake = {}; fake.destruction = XR_ERROR_INSTANCE_LOST;
    result = tdvr::probe_openxr(api);
    require(result.status == tdvr::ProbeStatus::error, "cleanup failure lost");
    fake = {}; fake.grow_once = true;
    result = tdvr::probe_openxr(api);
    require(result.status == tdvr::ProbeStatus::ready && fake.fills == 2, "enumeration growth not retried");
    fake = {}; fake.always_grow = true;
    result = tdvr::probe_openxr(api);
    require(result.status == tdvr::ProbeStatus::error && fake.fills == 3 && fake.creates == 0, "retry limit missing");
    auto incomplete = api; incomplete.get_proc = nullptr;
    result = tdvr::probe_openxr(incomplete);
    require(result.status == tdvr::ProbeStatus::error, "incomplete dispatch accepted");
    std::cout << "OpenXR preflight success/failure/cleanup checks passed with a fake API; no runtime loaded.\n";
}
