#include "tdvr_engine_probe.h"
#include <cstdlib>
#include <iostream>
int main() {
    TdvrEngineStats stats{};
    if (TdvrGetEngineStats(&stats) != E_INVALIDARG) return 1;
    stats.size = sizeof(stats); stats.abi = 1;
    const auto expected = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (TdvrStartEngineProbe(nullptr) != expected || TdvrStartEngineProbe(nullptr) != expected) return 1;
    if (FAILED(TdvrGetEngineStats(&stats)) || stats.identity_matched || stats.signature_matched
        || stats.hook_installed || stats.camera_calls || stats.valid_samples) return 1;
    TdvrWorldRenderStats world{};
    if (TdvrGetWorldRenderStats(nullptr) != E_INVALIDARG || TdvrGetWorldRenderStats(&world) != E_INVALIDARG) return 1;
    world.size = sizeof(world); world.abi = 1;
    if (FAILED(TdvrGetWorldRenderStats(&world)) || world.calls || world.completed_calls
        || world.valid_samples || world.signature_matched || world.hook_installed) return 1;
    TdvrCameraOverrideRequest request{}; request.size = sizeof(request); request.abi = 1;
    TdvrEngineTargetStats targets{};
    if (TdvrGetEngineTargetStats(&targets) != E_INVALIDARG) return 1;
    targets.size = sizeof(targets); targets.abi = 1;
    if (FAILED(TdvrGetEngineTargetStats(&targets)) || targets.samples || targets.known_mask || targets.count) return 1;
    TdvrTemporalStats temporal{};
    TdvrEyeTargetProbeStats eye_targets{};
    if (TdvrRequestEyeTargetProbe(nullptr) != HRESULT_FROM_WIN32(ERROR_INVALID_STATE)
        || TdvrGetEyeTargetProbeStats(nullptr) != E_INVALIDARG || TdvrGetEyeTargetProbeStats(&eye_targets) != E_INVALIDARG) return 1;
    eye_targets.size = sizeof(eye_targets); eye_targets.abi = 1;
    if (FAILED(TdvrGetEyeTargetProbeStats(&eye_targets)) || eye_targets.phase || eye_targets.created) return 1;
    TdvrEyeHistoryProbeStats histories{};
    if (TdvrRequestEyeHistoryProbe(nullptr) != HRESULT_FROM_WIN32(ERROR_INVALID_STATE)
        || TdvrGetEyeHistoryProbeStats(nullptr) != E_INVALIDARG || TdvrGetEyeHistoryProbeStats(&histories) != E_INVALIDARG) return 1;
    histories.size = sizeof(histories); histories.abi = 1;
    if (FAILED(TdvrGetEyeHistoryProbeStats(&histories)) || histories.phase || histories.created || histories.required_bytes) return 1;
    TdvrEyeTaaProbeStats taa_textures{};
    if (TdvrRequestEyeTaaProbe(nullptr) != HRESULT_FROM_WIN32(ERROR_INVALID_STATE)
        || TdvrGetEyeTaaProbeStats(nullptr) != E_INVALIDARG || TdvrGetEyeTaaProbeStats(&taa_textures) != E_INVALIDARG) return 1;
    taa_textures.size = sizeof(taa_textures); taa_textures.abi = 1;
    if (FAILED(TdvrGetEyeTaaProbeStats(&taa_textures)) || taa_textures.phase || taa_textures.created) return 1;
    if (TdvrGetTemporalStats(nullptr) != E_INVALIDARG || TdvrGetTemporalStats(&temporal) != E_INVALIDARG) return 1;
    temporal.size = sizeof(temporal); temporal.abi = 2;
    if (TdvrGetTemporalStats(&temporal) != E_INVALIDARG) return 1;
    temporal.abi = 1;
    if (FAILED(TdvrGetTemporalStats(&temporal)) || temporal.samples || temporal.last_sample_valid) return 1;
    if (TdvrSubmitCameraOverride(nullptr) != E_INVALIDARG
        || TdvrSubmitCameraOverride(&request) != HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) return 1;
    TdvrCameraOverrideStats override_stats{};
    TdvrTrackedCameraRequest tracked{sizeof(TdvrTrackedCameraRequest),1};
    if (TdvrSubmitTrackedCameraPreview(nullptr) != E_INVALIDARG
        || TdvrSubmitTrackedCameraPreview(&tracked) != HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) return 1;
    if (TdvrGetCameraOverrideStats(&override_stats) != E_INVALIDARG) return 1;
    override_stats.size = sizeof(override_stats); override_stats.abi = 1;
    if (FAILED(TdvrGetCameraOverrideStats(&override_stats)) || override_stats.applied_calls || override_stats.remaining_calls) return 1;
    std::cout << "Unknown executable rejected before any engine hook or camera access. No game or XR used.\n";
}
