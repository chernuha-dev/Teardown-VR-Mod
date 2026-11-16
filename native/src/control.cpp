// SPDX-License-Identifier: MIT
// Development-only game API. No SendInput, cursor calls, focus or foreground changes.
#include "tdvr_capture.h"
#include "tdvr_engine_probe.h"
#include <sddl.h>
#include <filesystem>
#include <cwctype>
#include <sstream>
#include <string>
#include <mutex>
#include <memory>
#include <locale>
#include <cmath>

namespace {
std::mutex startup_mutex;
HANDLE pipe_handle = INVALID_HANDLE_VALUE;
std::wstring lower(std::wstring value) { for (auto& c : value) c = static_cast<wchar_t>(std::towlower(c)); return value; }
bool development_host() {
    wchar_t exe[32768]{}, dll[32768]{}, profile[32768]{};
    HMODULE module = nullptr;
    if (!GetModuleFileNameW(nullptr, exe, 32768)
        || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&development_host), &module)
        || !GetModuleFileNameW(module, dll, 32768)) return false;
    const auto n = GetEnvironmentVariableW(L"TDVR_DEV_PROFILE", profile, 32768);
    if (!n || n >= 32768) return false;
    try {
        const auto binary = lower(std::filesystem::canonical(exe).wstring());
        const auto library = lower(std::filesystem::canonical(dll).wstring());
        const auto root_end = library.rfind(L"\\build\\");
        if (root_end == std::wstring::npos) return false;
        const auto local = library.substr(0, root_end) + L"\\local\\";
        return binary.find(local) == 0 && lower(std::filesystem::path(exe).filename().wstring()) == L"teardown.exe"
            && lower(std::filesystem::canonical(profile).wstring()).find(local) == 0;
    } catch (...) { return false; }
}
BOOL CALLBACK close_game(HWND window, LPARAM) {
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
        wchar_t name[128]{}; GetClassNameW(window, name, 128);
        if (std::wstring(name) == L"TeardownWindow") PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}
std::string engine_status() {
    const auto engine = GetModuleHandleW(L"tdvr_engine_probe.dll");
    if (!engine) return "{\"ok\":false,\"error\":\"engine_probe_not_loaded\"}";
    const auto camera_fn = reinterpret_cast<HRESULT (WINAPI*)(TdvrEngineStats*)>(GetProcAddress(engine, "TdvrGetEngineStats"));
    const auto world_fn = reinterpret_cast<HRESULT (WINAPI*)(TdvrWorldRenderStats*)>(GetProcAddress(engine, "TdvrGetWorldRenderStats"));
    const auto target_fn = reinterpret_cast<HRESULT (WINAPI*)(TdvrEngineTargetStats*)>(GetProcAddress(engine, "TdvrGetEngineTargetStats"));
    const auto temporal_fn = reinterpret_cast<HRESULT (WINAPI*)(TdvrTemporalStats*)>(GetProcAddress(engine, "TdvrGetTemporalStats"));
    TdvrEngineStats camera{}; camera.size = sizeof(camera); camera.abi = 1;
    TdvrWorldRenderStats world{}; world.size = sizeof(world); world.abi = 1;
    TdvrEngineTargetStats target{}; target.size = sizeof(target); target.abi = 1;
    TdvrTemporalStats temporal{}; temporal.size = sizeof(temporal); temporal.abi = 1;
    const auto camera_hr = camera_fn ? camera_fn(&camera) : E_NOINTERFACE;
    const auto world_hr = world_fn ? world_fn(&world) : E_NOINTERFACE;
    const auto target_hr = target_fn ? target_fn(&target) : E_NOINTERFACE;
    const auto temporal_hr = temporal_fn ? temporal_fn(&temporal) : E_NOINTERFACE;
    std::ostringstream out; out.imbue(std::locale::classic()); out.precision(17);
    out << "{\"ok\":true,\"protocol\":1,\"pid\":" << GetCurrentProcessId()
        << ",\"camera_hresult\":" << camera_hr << ",\"world_hresult\":" << world_hr << ",\"target_hresult\":" << target_hr
        << ",\"temporal_hresult\":" << temporal_hr;
    // These are separate observational snapshots, not an atomic stereo frame.
    // Return null for invalid floating-point samples rather than invalid JSON.
    auto array = [&](const auto& values) {
        out << '['; bool first = true;
        for (const auto value : values) {
            if (!first) out << ',';
            first = false;
            if (std::isfinite(value)) out << value; else out << "null";
        }
        out << ']';
    };
    if (SUCCEEDED(camera_hr)) {
        out << ",\"camera\":{\"calls\":" << camera.camera_calls << ",\"valid\":" << camera.valid_samples
            << ",\"rejected\":" << camera.rejected_samples << ",\"hook_installed\":" << camera.hook_installed << ",\"position\":";
        array(camera.position); out << ",\"orientation\":"; array(camera.orientation); out << '}';
    }
    if (SUCCEEDED(world_hr)) {
        out << ",\"world\":{\"calls\":" << world.calls << ",\"completed\":" << world.completed_calls
            << ",\"valid\":" << world.valid_samples << ",\"invalid\":" << world.invalid_samples
            << ",\"camera_mutations\":" << world.camera_mutations << ",\"serial_steps\":" << world.serial_steps
            << ",\"history_swaps\":" << world.history_swaps << ",\"thread_changes\":" << world.thread_changes
            << ",\"hook_installed\":" << world.hook_installed << ",\"camera\":";
        array(world.camera); out << '}';
    }
    if (SUCCEEDED(target_hr)) {
        out << ",\"target\":{\"samples\":" << target.samples << ",\"valid\":" << target.valid_samples
            << ",\"rejected\":" << target.rejected_samples << ",\"game_state\":" << target.game_state
            << ",\"width\":" << target.width << ",\"height\":" << target.height << ",\"count\":" << target.count
            << ",\"known_mask\":" << target.known_mask << ",\"matching_extent_mask\":" << target.matching_extent_mask << '}';
    }
    if (SUCCEEDED(temporal_hr)) {
        out << ",\"temporal\":{\"samples\":" << temporal.samples << ",\"rejected\":" << temporal.rejected
            << ",\"last_sample_valid\":" << temporal.last_sample_valid;
        if (temporal.last_sample_valid) {
            out << ",\"scene_changed_mask\":" << temporal.scene_changed_mask << ",\"taa_changed_mask\":" << temporal.taa_changed_mask
                << ",\"descriptor_changed_mask\":" << temporal.descriptor_changed_mask;
            auto emit = [&](const TdvrTemporalState& state) {
                out << "{\"serial\":" << state.serial << ",\"history\":[" << state.history[0] << ',' << state.history[1]
                    << "],\"reset\":" << state.reset_flag << ",\"taa_word\":" << state.taa_word
                    << ",\"taa_word_readable\":" << state.taa_word_readable << ",\"scene_slot_mask\":" << state.scene_slot_mask
                    << ",\"scene_descriptor_mask\":" << state.scene_descriptor_mask << ",\"taa_slot_mask\":" << state.taa_slot_mask
                    << ",\"taa_presence_mask\":" << state.taa_presence_mask << ",\"target_layout_mask\":" << state.target_layout_mask
                    << ",\"descriptor_hashes\":[";
                for (unsigned i = 0; i < 10; ++i) { if (i) out << ','; out << "\"" << std::hex << state.descriptor_hashes[i] << "\"" << std::dec; }
                out << "]}";
            };
            out << ",\"before\":"; emit(temporal.before); out << ",\"after\":"; emit(temporal.after);
        }
        out << '}';
    }
    out << '}'; return out.str();
}
std::string execute(const std::string& command) {
    if (command == "ping") return "{\"ok\":true,\"protocol\":1}";
    if (command == "engine-status") return engine_status();
    if (command == "xr-views") {
        const auto capture = GetModuleHandleW(L"tdvr_capture.dll");
        const auto get = capture ? reinterpret_cast<HRESULT (WINAPI*)(TdvrXrViewSnapshot*)>(GetProcAddress(capture, "TdvrGetXrViews")) : nullptr;
        if (!get) return "{\"ok\":false,\"error\":\"xr_views_export_unavailable\"}";
        TdvrXrViewSnapshot snapshot{}; snapshot.size = sizeof(snapshot); snapshot.abi = 1;
        const auto hr = get(&snapshot);
        std::ostringstream out; out.imbue(std::locale::classic()); out.precision(17);
        out << "{\"ok\":" << (SUCCEEDED(hr) ? "true" : "false") << ",\"hresult\":" << hr;
        if (SUCCEEDED(hr)) {
            out << ",\"available\":" << snapshot.available << ",\"sample_result\":" << snapshot.sample_result
                << ",\"xr_frame_id\":" << snapshot.frame_id << ",\"predicted_display_time\":" << snapshot.predicted_display_time
                << ",\"view_state_flags\":" << snapshot.view_state_flags;
            if (snapshot.available) {
                out << ",\"eyes\":[";
                for (unsigned eye = 0; eye < 2; ++eye) {
                    if (eye) out << ',';
                    const auto& p = snapshot.eyes[eye]; const auto& f = snapshot.fov[eye];
                    out << "{\"position\":[" << p.position[0] << ',' << p.position[1] << ',' << p.position[2]
                        << "],\"orientation\":[" << p.orientation[0] << ',' << p.orientation[1] << ',' << p.orientation[2] << ',' << p.orientation[3]
                        << "],\"fov\":[" << f.left << ',' << f.right << ',' << f.up << ',' << f.down << "]}";
                }
                out << ']';
            }
        }
        out << '}'; return out.str();
    }
    if (command == "request-exit") {
        // Ask this process's own game window to follow its normal exit path.
        // Does not activate a window, synthesize a key, or terminate the process.
        EnumWindows(close_game, 0);
        return "{\"ok\":true,\"exit_requested\":true,\"exit_confirmed\":false}";
    }
    if (command != "status") return "{\"ok\":false,\"error\":\"unknown_command\"}";
    const auto capture = GetModuleHandleW(L"tdvr_capture.dll");
    if (!capture) return "{\"ok\":false,\"error\":\"capture_not_loaded\"}";
    const auto get_capture = reinterpret_cast<HRESULT (WINAPI*)(TdvrCaptureStats*)>(GetProcAddress(capture, "TdvrGetCaptureStats"));
    const auto get_panel = reinterpret_cast<HRESULT (WINAPI*)(TdvrPanelStats*)>(GetProcAddress(capture, "TdvrGetPanelStats"));
    TdvrCaptureStats stats{}; stats.size = sizeof(stats); stats.abi = 1;
    TdvrPanelStats panel{}; panel.size = sizeof(panel); panel.abi = 2;
    const auto cap_hr = get_capture ? get_capture(&stats) : E_NOINTERFACE;
    const auto panel_hr = get_panel ? get_panel(&panel) : E_NOINTERFACE;
    std::ostringstream out;
    out << "{\"ok\":true,\"protocol\":1,\"pid\":" << GetCurrentProcessId()
        << ",\"capture_hresult\":" << cap_hr << ",\"presents\":" << stats.presents
        << ",\"captured\":" << stats.captured_frames << ",\"panel_hresult\":" << panel_hr;
    if (SUCCEEDED(panel_hr)) out << ",\"panel\":{\"stage\":" << panel.stage << ",\"submitted\":" << panel.submitted
        << ",\"frames\":" << panel.frames << ",\"xr\":" << panel.xr_result << ",\"gpu\":" << panel.gpu_result
        << ",\"source_format\":" << panel.source_format << ",\"swapchain_format\":" << panel.swapchain_format
        << ",\"resource_format\":" << panel.resource_format << ",\"color_space\":" << panel.color_space
        << ",\"failure_stage\":" << panel.failure_stage << ",\"quarantined\":" << panel.quarantined << '}';
    out << '}'; return out.str();
}
DWORD WINAPI server(void*) {
    for (;;) {
        const auto connected = ConnectNamedPipe(pipe_handle, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected) { Sleep(5); continue; }
        if (connected) {
            char input[128]{}; DWORD read = 0, written = 0;
            std::string response;
            const auto deadline = GetTickCount64() + 2000;
            bool received = false;
            do {
                received = ReadFile(pipe_handle, input, sizeof(input), &read, nullptr) != FALSE;
                if (received || GetLastError() != ERROR_NO_DATA) break;
                Sleep(5);
            } while (GetTickCount64() < deadline);
            if (received && read && read < sizeof(input)) {
                std::string command(input, read);
                while (!command.empty() && (command.back() == '\n' || command.back() == '\r')) command.pop_back();
                try { response = execute(command); }
                catch (...) { response = "{\"ok\":false,\"error\":\"internal_error\"}"; }
            } else response = "{\"ok\":false,\"error\":\"invalid_request\"}";
            WriteFile(pipe_handle, response.data(), static_cast<DWORD>(response.size()), &written, nullptr);
            // Wait for a bounded acknowledgement before disconnecting, so the
            // response isn't discarded. A stalled peer cannot block rendering.
            const auto ack_deadline = GetTickCount64() + 2000;
            do {
                if (ReadFile(pipe_handle, input, sizeof(input), &read, nullptr) || GetLastError() != ERROR_NO_DATA) break;
                Sleep(5);
            } while (GetTickCount64() < ack_deadline);
        }
        DisconnectNamedPipe(pipe_handle);
        Sleep(5);
    }
}
}
extern "C" __declspec(dllexport) DWORD WINAPI TdvrStartControl(void*) {
    try {
    std::lock_guard<std::mutex> lock(startup_mutex);
    if (!development_host()) return static_cast<DWORD>(E_ACCESSDENIED);
    if (pipe_handle != INVALID_HANDLE_VALUE) return static_cast<DWORD>(S_FALSE);
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return static_cast<DWORD>(HRESULT_FROM_WIN32(GetLastError()));
    DWORD needed = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    auto buffer = std::make_unique<unsigned char[]>(needed);
    if (!GetTokenInformation(token, TokenUser, buffer.get(), needed, &needed)) { const auto error = GetLastError(); CloseHandle(token); return static_cast<DWORD>(HRESULT_FROM_WIN32(error)); }
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.get())->User.Sid, &sid)) return static_cast<DWORD>(HRESULT_FROM_WIN32(GetLastError()));
    const auto acl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")"; LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return static_cast<DWORD>(HRESULT_FROM_WIN32(GetLastError()));
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    const auto name = L"\\\\.\\pipe\\tdvr-dev-" + std::to_wstring(GetCurrentProcessId());
    pipe_handle = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 128, 1000, &security);
    const auto error = GetLastError(); LocalFree(descriptor);
    if (pipe_handle == INVALID_HANDLE_VALUE) return static_cast<DWORD>(HRESULT_FROM_WIN32(error));
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&TdvrStartControl), &pinned)) { CloseHandle(pipe_handle); pipe_handle = INVALID_HANDLE_VALUE; return static_cast<DWORD>(E_FAIL); }
    const auto thread = CreateThread(nullptr, 0, server, nullptr, 0, nullptr);
    if (!thread) { const auto last = GetLastError(); CloseHandle(pipe_handle); pipe_handle = INVALID_HANDLE_VALUE; return static_cast<DWORD>(HRESULT_FROM_WIN32(last)); }
    CloseHandle(thread); return S_OK;
    } catch (...) { return static_cast<DWORD>(E_FAIL); }
}
