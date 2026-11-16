#include "tdvr_frame_loop.h"
#include <array>

namespace tdvr {
namespace {
struct FrameScope {
    const FrameDispatch& api;
    const FrameSettings& settings;
    XrTime display_time = 0;
    bool begun = false;
    bool waited[2]{};
    XrResult release() {
        XrResult first = XR_SUCCESS;
        for (int eye = 1; eye >= 0; --eye) {
            if (!waited[eye]) continue;
            waited[eye] = false;
            XrSwapchainImageReleaseInfo info{};
            info.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            const auto r = api.release_image(settings.eyes[eye].swapchain, &info);
            if (r != XR_SUCCESS && first == XR_SUCCESS) first = r;
        }
        return first;
    }
    XrResult end(const XrCompositionLayerBaseHeader* layer) {
        if (!begun) return XR_SUCCESS;
        begun = false;
        XrFrameEndInfo info{};
        info.type = XR_TYPE_FRAME_END_INFO;
        info.displayTime = display_time;
        info.environmentBlendMode = settings.blend_mode;
        info.layerCount = layer ? 1 : 0;
        info.layers = layer ? &layer : nullptr;
        return api.end_frame(settings.session, &info);
    }
    ~FrameScope() { release(); end(nullptr); }
};
}

FrameReport run_stereo_frame(const FrameDispatch& api, const FrameSettings& settings,
    RenderStereo render, void* context) {
    FrameReport report;
    FrameScope scope{api, settings};
    auto finish = [&](XrResult result, const char* stage, bool restart,
                      const XrCompositionLayerBaseHeader* layer = nullptr) {
        report.result = result;
        report.stage = stage;
        report.restart_session = restart;
        const XrResult released = scope.release();
        if (released != XR_SUCCESS) {
            report.result = released;
            report.stage += "; release failed";
            report.restart_session = true;
            layer = nullptr;
        }
        const XrResult ended = scope.end(layer);
        if (ended != XR_SUCCESS) {
            report.result = ended;
            report.stage += "; end frame failed";
            report.restart_session = true;
        }
        report.submitted = layer && ended == XR_SUCCESS;
        return report;
    };
    if (!api.wait_frame || !api.begin_frame || !api.locate_views || !api.acquire_image
        || !api.wait_image || !api.release_image || !api.end_frame || !render
        || !settings.session || !settings.space
        || settings.eyes[0].swapchain == settings.eyes[1].swapchain)
        return finish(XR_ERROR_VALIDATION_FAILURE, "invalid frame arguments", false);
    for (const auto& eye : settings.eyes)
        if (!eye.swapchain || !eye.image_count || eye.extent.width <= 0 || eye.extent.height <= 0)
            return finish(XR_ERROR_VALIDATION_FAILURE, "invalid frame target", false);
    if (settings.blend_mode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE
        && settings.blend_mode != XR_ENVIRONMENT_BLEND_MODE_ADDITIVE
        && settings.blend_mode != XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
        return finish(XR_ERROR_VALIDATION_FAILURE, "invalid blend mode", false);

    XrFrameWaitInfo wait{}; wait.type = XR_TYPE_FRAME_WAIT_INFO;
    XrFrameState state{}; state.type = XR_TYPE_FRAME_STATE;
    XrResult r = api.wait_frame(settings.session, &wait, &state);
    if (r != XR_SUCCESS) return finish(r, "xrWaitFrame", true);
    scope.display_time = report.display_time = state.predictedDisplayTime;
    XrFrameBeginInfo begin{}; begin.type = XR_TYPE_FRAME_BEGIN_INFO;
    r = api.begin_frame(settings.session, &begin);
    if (r != XR_SUCCESS && r != XR_FRAME_DISCARDED) return finish(r, "xrBeginFrame", true);
    scope.begun = true;
    if (!state.shouldRender) return finish(XR_SUCCESS, "runtime requested no rendering", false);

    XrViewLocateInfo locate{};
    locate.type = XR_TYPE_VIEW_LOCATE_INFO;
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = state.predictedDisplayTime;
    locate.space = settings.space;
    XrViewState view_state{}; view_state.type = XR_TYPE_VIEW_STATE;
    std::array<XrView, 2> views{};
    for (auto& view : views) view.type = XR_TYPE_VIEW;
    uint32_t count = 0;
    r = api.locate_views(settings.session, &locate, &view_state, 2, &count, views.data());
    if (r != XR_SUCCESS) return finish(r, "xrLocateViews", true);
    if (count != 2) return finish(XR_ERROR_RUNTIME_FAILURE, "stereo requires two located views", true);
    constexpr XrViewStateFlags tracked = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT
        | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    if ((view_state.viewStateFlags & tracked) != tracked)
        return finish(XR_SUCCESS, "invalid/untracked pose: pair omitted", false);
    TdvrFrameInput input{};
    input.frame = {settings.simulation_id, state.predictedDisplayTime};
    input.world_from_tracking = settings.world_from_tracking;
    input.world_scale = settings.world_scale;
    input.near_clip_m = settings.near_clip_m;
    input.far_clip_m = settings.far_clip_m;
    for (size_t eye = 0; eye != 2; ++eye) {
        const auto& pose = views[eye].pose;
        const auto& fov = views[eye].fov;
        input.eyes[eye] = {input.frame,
            {{pose.position.x, pose.position.y, pose.position.z},
             {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w}},
            {fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown},
            TDVR_ORIENTATION_VALID | TDVR_POSITION_VALID | TDVR_ORIENTATION_TRACKED | TDVR_POSITION_TRACKED};
    }
    TdvrStereoFrame packet;
    if (tdvr_build_stereo_frame(&input, &packet) != TDVR_OK)
        return finish(XR_ERROR_VALIDATION_FAILURE, "invalid stereo packet: pair omitted", false);
    uint32_t indices[2]{};
    for (size_t eye = 0; eye != 2; ++eye) {
        XrSwapchainImageAcquireInfo acquire{}; acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
        r = api.acquire_image(settings.eyes[eye].swapchain, &acquire, &indices[eye]);
        if (r != XR_SUCCESS) return finish(r, "xrAcquireSwapchainImage", true);
        XrSwapchainImageWaitInfo image_wait{};
        image_wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
        image_wait.timeout = 5'000'000; // ns, no infinite swapchain wait
        r = api.wait_image(settings.eyes[eye].swapchain, &image_wait);
        // Timeout is a positive XrResult. Never treat it as a ready image.
        if (r != XR_SUCCESS) return finish(r, "xrWaitSwapchainImage; session teardown required", true);
        scope.waited[eye] = true;
        if (indices[eye] >= settings.eyes[eye].image_count)
            return finish(XR_ERROR_RUNTIME_FAILURE, "swapchain image index out of bounds", true);
    }
    try {
        if (!render(packet, indices, context))
            return finish(XR_ERROR_RUNTIME_FAILURE, "renderer rejected stereo pair", true);
    } catch (...) {
        return finish(XR_ERROR_RUNTIME_FAILURE, "renderer threw; stereo pair omitted", true);
    }
    std::array<XrCompositionLayerProjectionView, 2> layer_views{};
    for (size_t eye = 0; eye != 2; ++eye) {
        auto& target = layer_views[eye];
        target.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        target.pose = views[eye].pose;
        target.fov = views[eye].fov;
        target.subImage.swapchain = settings.eyes[eye].swapchain;
        target.subImage.imageRect.extent = settings.eyes[eye].extent;
    }
    XrCompositionLayerProjection layer{};
    layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    layer.space = settings.space;
    layer.viewCount = 2;
    layer.views = layer_views.data();
    return finish(XR_SUCCESS, "stereo pair submitted", false,
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer));
}
}
