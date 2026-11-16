#include "tdvr_d3d12_probe.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool listed(const tdvr::AdapterLuid& luid, const tdvr::D3d12ProbeReport& report) {
    for (const auto& adapter : report.adapters) {
        if (luid.low == adapter.luid.low && luid.high == adapter.luid.high) return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    tdvr::D3d12ProbeOptions options;
    options.use_warp = true;
    if (argc == 2 && std::string(argv[1]) == "--hardware") options.use_warp = false;
    else if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--warp")) {
        std::cerr << "Usage: tdvr_d3d12_tests [--warp|--hardware]\n";
        return 2;
    }

    auto invalid = options;
    invalid.use_warp = true;
    invalid.require_adapter = true;
    require(!tdvr::run_d3d12_probe(invalid).success, "WARP accepted with a required adapter");
    invalid = options;
    invalid.minimum_feature_level = 0xdead;
    require(!tdvr::run_d3d12_probe(invalid).success, "Invalid feature level accepted");
    invalid = options;
    invalid.timeout_ms = 0;
    require(!tdvr::run_d3d12_probe(invalid).success, "Zero GPU timeout accepted");
    invalid.timeout_ms = std::numeric_limits<std::uint32_t>::max();
    require(!tdvr::run_d3d12_probe(invalid).success, "Infinite GPU timeout accepted");

    const auto report = tdvr::run_d3d12_probe(options);
    require(report.success, "D3D12 probe failed: " + report.error);
    require(report.selected_adapter.is_software == options.use_warp, "Wrong adapter kind selected");
    require(report.selected_adapter.supports_d3d12, "Selected adapter lacks D3D12 support");
    require(report.selected_adapter.max_feature_level >= options.minimum_feature_level,
            "Selected adapter lacks the requested feature level");
    require(report.verified_pixels_per_eye == 17 * 13, "Both complete eye textures were not checked");
    require(report.readback_row_pitch > 17 * 4 && report.readback_row_pitch % 256 == 0,
            "Test did not exercise D3D12 readback row padding");

    // An absent runtime LUID must fail instead of silently selecting the first GPU/WARP.
    tdvr::D3d12ProbeOptions missing;
    missing.require_adapter = true;
    missing.adapter_luid = {0, std::numeric_limits<std::int32_t>::min()};
    while (listed(missing.adapter_luid, report)) ++missing.adapter_luid.low;
    const auto missing_report = tdvr::run_d3d12_probe(missing);
    require(!missing_report.success && missing_report.error.find("LUID") != std::string::npos,
            "Missing required adapter did not fail explicitly");

    // Replay an enumerated adapter by exact LUID through the same path used for XR requirements.
    // Some platforms only expose WARP via EnumWarpAdapter, so WARP replay is conditional.
    if (listed(report.selected_adapter.luid, report)) {
        tdvr::D3d12ProbeOptions replay;
        replay.require_adapter = true;
        replay.adapter_luid = report.selected_adapter.luid;
        const auto replay_report = tdvr::run_d3d12_probe(replay);
        require(replay_report.success, "Exact adapter LUID replay failed: " + replay_report.error);
        require(replay_report.selected_adapter.luid.low == replay.adapter_luid.low
            && replay_report.selected_adapter.luid.high == replay.adapter_luid.high,
            "Exact adapter LUID replay selected a different device");
    }
    std::cout << "D3D12 " << (options.use_warp ? "WARP" : "hardware") << ": "
              << report.selected_adapter.description << "; " << report.verified_pixels_per_eye
              << " pixels per eye verified, row pitch " << report.readback_row_pitch
              << ". No OpenXR session, game rendering, stereo timing or headset comfort tested.\n";
    return 0;
}
