#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tdvr {

struct AdapterLuid {
    std::uint32_t low = 0;
    std::int32_t high = 0;
};

struct D3d12AdapterInfo {
    std::string description;
    AdapterLuid luid;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint64_t dedicated_video_memory = 0;
    bool is_software = false;
    bool supports_d3d12 = false;
    // D3D_FEATURE_LEVEL numeric value; zero if D3D12 device creation failed.
    std::uint32_t max_feature_level = 0;
};

struct D3d12ProbeOptions {
    bool use_warp = false;
    bool require_adapter = false;
    AdapterLuid adapter_luid;
    std::uint32_t minimum_feature_level = 0xb000; // D3D_FEATURE_LEVEL_11_0
    std::uint32_t timeout_ms = 5000;
};

struct D3d12ProbeReport {
    bool success = false;
    std::string error;
    std::vector<D3d12AdapterInfo> adapters;
    D3d12AdapterInfo selected_adapter;
    std::uint32_t verified_pixels_per_eye = 0;
    std::uint32_t readback_row_pitch = 0;
    bool verified_cross_device_rejection = false;
};

// Standalone diagnostic process only: no window, OpenXR session, game or Present.
// A failed GPU wait removes this process's diagnostic D3D12 device before resource
// teardown. Never call inside an existing renderer (devices are per-adapter singletons).
// WARP and a required runtime adapter are mutually exclusive; no silent fallback.
D3d12ProbeReport run_d3d12_probe(const D3d12ProbeOptions& options);

} // namespace tdvr
