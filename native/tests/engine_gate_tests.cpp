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
    std::cout << "Unknown executable rejected before any engine hook or camera access. No game or XR used.\n";
}
