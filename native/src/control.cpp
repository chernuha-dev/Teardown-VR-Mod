// SPDX-License-Identifier: MIT
// Development-only game API. No SendInput, cursor calls, focus or foreground changes.
#include "tdvr_capture.h"
#include <sddl.h>
#include <filesystem>
#include <cwctype>
#include <sstream>
#include <string>
#include <mutex>
#include <memory>

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
std::string execute(const std::string& command) {
    if (command == "ping") return "{\"ok\":true,\"protocol\":1}";
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
