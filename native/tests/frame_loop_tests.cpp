#include "tdvr_frame_loop.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
template<class T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }
struct Fake {
    std::vector<std::string> events;
    bool should_render = true, tracked = true, invalid_pose = false;
    bool renderer_failure = false, renderer_exception = false, bad_index = false;
    XrResult wait_result = XR_SUCCESS, begin_result = XR_SUCCESS, locate_result = XR_SUCCESS;
    XrResult end_result = XR_SUCCESS, release_result = XR_SUCCESS;
    int fail_acquire = -1, timeout_eye = -1;
    uint32_t view_count = 2;
    unsigned renderer_calls = 0, layers = 999;
} fake;
int eye_of(XrSwapchain value) {
    require(value == handle<XrSwapchain>(10) || value == handle<XrSwapchain>(11), "unknown eye swapchain");
    return value == handle<XrSwapchain>(10) ? 0 : 1;
}
XrResult XRAPI_PTR wait_frame(XrSession, const XrFrameWaitInfo* info, XrFrameState* out) {
    require(info->type == XR_TYPE_FRAME_WAIT_INFO && out->type == XR_TYPE_FRAME_STATE, "wait frame types");
    fake.events.push_back("wait frame");
    out->predictedDisplayTime = 1234567;
    out->shouldRender = fake.should_render;
    return fake.wait_result;
}
XrResult XRAPI_PTR begin_frame(XrSession, const XrFrameBeginInfo* info) {
    require(info->type == XR_TYPE_FRAME_BEGIN_INFO, "begin frame type");
    fake.events.push_back("begin frame");
    return fake.begin_result;
}
XrResult XRAPI_PTR locate_views(XrSession, const XrViewLocateInfo* info, XrViewState* state,
    uint32_t capacity, uint32_t* count, XrView* views) {
    require(info->type == XR_TYPE_VIEW_LOCATE_INFO && info->displayTime == 1234567
        && info->space == handle<XrSpace>(2) && capacity == 2
        && info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        "eyes must be located once at predicted display time in one space");
    fake.events.push_back("locate pair");
    *count = fake.view_count;
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (fake.tracked) state->viewStateFlags |= XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    for (int eye = 0; eye != 2; ++eye) {
        require(views[eye].type == XR_TYPE_VIEW, "view type not initialized");
        views[eye].pose.orientation.w = fake.invalid_pose ? 0.f : 1.f;
        views[eye].pose.position.x = eye ? .032f : -.032f;
        views[eye].pose.position.y = 1.6f;
        views[eye].fov = {eye ? -.7f : -.9f, eye ? .9f : .7f, .8f, -.6f};
    }
    return fake.locate_result;
}
XrResult XRAPI_PTR acquire(XrSwapchain value, const XrSwapchainImageAcquireInfo* info, uint32_t* index) {
    const int eye = eye_of(value);
    require(info->type == XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, "acquire type");
    fake.events.push_back(eye ? "acquire right" : "acquire left");
    *index = fake.bad_index ? 3 : static_cast<uint32_t>(eye);
    return fake.fail_acquire == eye ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
}
XrResult XRAPI_PTR wait_image(XrSwapchain value, const XrSwapchainImageWaitInfo* info) {
    const int eye = eye_of(value);
    require(info->type == XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO && info->timeout > 0
        && info->timeout <= 5'000'000, "unbounded image wait");
    fake.events.push_back(eye ? "wait right" : "wait left");
    return fake.timeout_eye == eye ? XR_TIMEOUT_EXPIRED : XR_SUCCESS;
}
XrResult XRAPI_PTR release(XrSwapchain value, const XrSwapchainImageReleaseInfo* info) {
    require(info->type == XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, "release type");
    fake.events.push_back(eye_of(value) ? "release right" : "release left");
    return fake.release_result;
}
XrResult XRAPI_PTR end_frame(XrSession, const XrFrameEndInfo* info) {
    require(info->type == XR_TYPE_FRAME_END_INFO && info->displayTime == 1234567
        && info->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE, "frame end metadata changed");
    fake.events.push_back("end frame");
    fake.layers = info->layerCount;
    require(info->layerCount <= 1, "multiple independent eye layers");
    if (info->layerCount) {
        const auto* layer = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
        require(layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION && layer->viewCount == 2
            && layer->space == handle<XrSpace>(2), "stereo layer malformed");
        for (int eye = 0; eye != 2; ++eye) {
            const auto& view = layer->views[eye];
            require(view.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW
                && view.pose.position.x == (eye ? .032f : -.032f)
                && view.subImage.swapchain == handle<XrSwapchain>(10 + eye)
                && view.subImage.imageRect.extent.width == 640
                && view.subImage.imageRect.extent.height == 480
                && view.subImage.imageArrayIndex == 0, "eye projection/subimage mismatch");
        }
    } else require(!info->layers, "empty layer list must be null");
    return fake.end_result;
}
bool render(const TdvrStereoFrame& packet, const uint32_t indices[2], void*) {
    ++fake.renderer_calls;
    fake.events.push_back("render pair");
    require(packet.valid() && packet.key().simulation_id == 42
        && packet.key().predicted_display_time_ns == 1234567 && indices[0] == 0 && indices[1] == 1,
        "renderer did not receive one agreed snapshot and both image indices");
    require(packet.left_eye().world_pose.position[0] < packet.right_eye().world_pose.position[0]
        && packet.left_eye().projection.m[2] != packet.right_eye().projection.m[2], "eyes collapsed");
    if (fake.renderer_exception) throw std::runtime_error("simulated renderer failure");
    return !fake.renderer_failure;
}
const tdvr::FrameDispatch api{wait_frame, begin_frame, locate_views, acquire, wait_image, release, end_frame};
tdvr::FrameSettings settings() {
    return {handle<XrSession>(1), handle<XrSpace>(2), XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        {{handle<XrSwapchain>(10), 3, {640,480}}, {handle<XrSwapchain>(11), 3, {640,480}}},
        {{0,0,0},{0,0,0,1}}, 1, .05, 100, 42};
}
tdvr::FrameReport run() { return tdvr::run_stereo_frame(api, settings(), render); }
void skipped(const tdvr::FrameReport& report, bool restart) {
    require(!report.submitted && report.restart_session == restart && fake.layers == 0
        && fake.events.back() == "end frame", "failed/skipped frame was not ended empty");
}
}
int main() {
    auto report = run();
    require(report.result == XR_SUCCESS && report.submitted && !report.restart_session
        && fake.renderer_calls == 1 && fake.layers == 1, "normal pair not submitted");
    require(fake.events == std::vector<std::string>{"wait frame","begin frame","locate pair",
        "acquire left","wait left","acquire right","wait right","render pair",
        "release right","release left","end frame"}, "frame ordering broke");
    fake = {}; fake.should_render = false;
    report = run(); skipped(report, false);
    require(fake.events.size() == 3 && fake.renderer_calls == 0, "shouldRender=false performed eye work");
    fake = {}; fake.tracked = false;
    report = run(); skipped(report, false);
    require(fake.renderer_calls == 0 && fake.events.size() == 4, "tracking loss rendered old eyes");
    fake = {}; fake.invalid_pose = true;
    report = run(); skipped(report, false);
    require(fake.renderer_calls == 0 && report.result != XR_SUCCESS, "invalid quaternion rendered");
    fake = {}; fake.view_count = 1;
    report = run(); skipped(report, true);
    fake = {}; fake.locate_result = XR_ERROR_RUNTIME_FAILURE;
    report = run(); skipped(report, true);
    for (int eye = 0; eye != 2; ++eye) {
        fake = {}; fake.timeout_eye = eye;
        report = run(); skipped(report, true);
        require(report.result == XR_TIMEOUT_EXPIRED && fake.renderer_calls == 0,
            "positive timeout result was treated as ready");
        const std::string invalid_release = eye ? "release right" : "release left";
        for (const auto& event : fake.events) require(event != invalid_release, "unwaited image illegally released");
        fake = {}; fake.fail_acquire = eye;
        report = run(); skipped(report, true);
        require(fake.renderer_calls == 0, "partial acquisition rendered");
    }
    fake = {}; fake.bad_index = true;
    report = run(); skipped(report, true);
    require(fake.renderer_calls == 0, "invalid image index reached renderer");
    fake = {}; fake.renderer_failure = true;
    report = run(); skipped(report, true);
    fake = {}; fake.renderer_exception = true;
    report = run(); skipped(report, true);
    fake = {}; fake.release_result = XR_ERROR_RUNTIME_FAILURE;
    report = run(); skipped(report, true);
    fake = {}; fake.end_result = XR_ERROR_RUNTIME_FAILURE;
    report = run();
    require(!report.submitted && report.restart_session, "end failure reported as submission");
    fake = {}; fake.begin_result = XR_FRAME_DISCARDED;
    report = run();
    require(report.submitted && fake.layers == 1, "XR_FRAME_DISCARDED begin success mishandled");
    fake = {}; fake.wait_result = XR_ERROR_SESSION_LOST;
    report = run();
    require(!report.submitted && report.restart_session && fake.events.size() == 1, "wait failure reached begin/end");
    fake = {}; fake.begin_result = XR_ERROR_SESSION_LOST;
    report = run();
    require(!report.submitted && report.restart_session && fake.events.size() == 2, "failed begin reached end");
    fake = {};
    auto invalid = settings(); invalid.eyes[1].swapchain = invalid.eyes[0].swapchain;
    report = tdvr::run_stereo_frame(api, invalid, render);
    require(!report.submitted && fake.events.empty(), "duplicate eye target reached runtime");
    std::cout << "Stereo frame order, single locate/snapshot, no-render/tracking loss and cleanup verified with fake XR only.\n";
}
