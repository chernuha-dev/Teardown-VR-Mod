// SPDX-License-Identifier: MIT
#include "tdvr_capture.h"
#include <shlobj.h>
#include <filesystem>
#include <cstdlib>
#include <iostream>

int main() {
    const auto module = GetModuleHandleW(L"tdvr_capture.dll");
    if (!module) return 1;
    const auto start_profile = reinterpret_cast<HRESULT (WINAPI*)()>(GetProcAddress(module, "TdvrStartDevelopmentProfile"));
    const auto start_capture = reinterpret_cast<HRESULT (WINAPI*)()>(GetProcAddress(module, "TdvrStartCapture"));
    const auto configure = reinterpret_cast<HRESULT (WINAPI*)(const wchar_t*)>(GetProcAddress(module, "TdvrConfigureDevelopmentProfile"));
    const auto get_stats = reinterpret_cast<HRESULT (WINAPI*)(TdvrCaptureStats*)>(GetProcAddress(module, "TdvrGetCaptureStats"));
    if (!start_profile || !start_capture || !configure || !get_stats) return 2;
    if (start_profile() != S_FALSE || start_capture() != HRESULT_FROM_WIN32(ERROR_INVALID_STATE)
        || configure(L"C:\\unused") != HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) return 3;
    const auto* profile = std::getenv("TDVR_DEV_PROFILE");
    if (!profile) return 4;
    const auto* local = std::getenv("LOCALAPPDATA");
    const auto* roaming = std::getenv("APPDATA");
    const auto* user = std::getenv("USERPROFILE");
    if (!local || !roaming || !user
        || std::filesystem::path(local) != std::filesystem::path(profile) / "AppData/Local"
        || std::filesystem::path(roaming) != std::filesystem::path(profile) / "AppData/Roaming"
        || std::filesystem::path(user) != std::filesystem::path(profile)) return 8;
    for (const auto folder : {CSIDL_LOCAL_APPDATA, CSIDL_APPDATA, CSIDL_PERSONAL, CSIDL_PROFILE}) {
        char path[MAX_PATH]{};
        const char* suffix = folder == CSIDL_LOCAL_APPDATA ? "AppData/Local" : folder == CSIDL_APPDATA ? "AppData/Roaming"
            : folder == CSIDL_PERSONAL ? "Documents" : "";
        const auto expected = *suffix ? std::filesystem::path(profile) / suffix : std::filesystem::path(profile);
        if (!SHGetSpecialFolderPathA(nullptr, path, folder, FALSE)
            || std::filesystem::path(path) != expected) return 5;
    }
    TdvrCaptureStats stats{}; stats.size = sizeof(stats); stats.abi = 1;
    if (get_stats(&stats) != S_OK || stats.presents || stats.associations || stats.captured_frames) return 6;
    if (GetModuleHandleW(L"openxr_loader.dll")) return 7;
    std::cout << "Profile-only folders isolated; capture startup rejected; no XR loader\n";
    return 0;
}
