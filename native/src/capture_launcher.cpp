#include "tdvr_capture.h"
#include "tdvr_render_trace.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <utility>

namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
void wincheck(bool ok, const char* operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + " Win32=" + std::to_string(GetLastError()));
}
uintptr_t remote_module(DWORD pid, const std::wstring& filename) {
    for (unsigned attempt = 0; attempt != 5; ++attempt) {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if (snapshot.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_BAD_LENGTH) continue;
        wincheck(snapshot.value != INVALID_HANDLE_VALUE, "module snapshot");
        MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (!Module32FirstW(snapshot.value, &entry)) break;
        do {
            if (_wcsicmp(entry.szModule, filename.c_str()) == 0)
                return reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        } while (Module32NextW(snapshot.value, &entry));
        break;
    }
    throw std::runtime_error("required module not found in child");
}
uintptr_t mapped_module(HANDLE process, const wchar_t* filename) {
    // A suspended process has no initialized loader list yet (Toolhelp can return
    // ERROR_PARTIAL_COPY). Query image mappings instead of assuming identical ASLR.
    uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION region{};
    while (VirtualQueryEx(process, reinterpret_cast<void*>(address), &region, sizeof(region))) {
        if (region.Type == MEM_IMAGE && region.BaseAddress == region.AllocationBase) {
            wchar_t name[32768]{};
            if (GetMappedFileNameW(process, region.AllocationBase, name, static_cast<DWORD>(std::size(name)))
                && _wcsicmp(std::filesystem::path(name).filename().c_str(), filename) == 0)
                return reinterpret_cast<uintptr_t>(region.AllocationBase);
        }
        const auto next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= address) break;
        address = next;
    }
    throw std::runtime_error("required initial image mapping not found");
}
DWORD remote_call(HANDLE process, uintptr_t entry, void* argument, const char* operation = "stats RPC") {
    Handle thread(CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(entry), argument, 0, nullptr));
    wincheck(thread.value != nullptr, "CreateRemoteThread");
    // Never free a buffer still used by a timed-out remote thread. The outer
    // guard terminates only our suspended/owned test child on failure.
    if (WaitForSingleObject(thread.value, 10000) != WAIT_OBJECT_0)
        throw std::runtime_error(std::string(operation) + " timed out");
    DWORD result = 0;
    wincheck(GetExitCodeThread(thread.value, &result) != FALSE, "GetExitCodeThread");
    return result;
}
struct Child {
    PROCESS_INFORMATION info{};
    bool exited = false;
    ~Child() {
        if (info.hProcess && !exited) {
            TerminateProcess(info.hProcess, 125);
            WaitForSingleObject(info.hProcess, 5000);
        }
        if (info.hThread) CloseHandle(info.hThread);
        if (info.hProcess) CloseHandle(info.hProcess);
    }
};
}
int wmain(int argc, wchar_t** argv) {
    try {
        std::cout << std::unitbuf;
        // This launcher never attaches to an existing process or the user's VR app.
        const bool trace = argc >= 4 && std::wstring(argv[argc-1]) == L"--trace";
        const bool profile_only = argc >= 4 && std::wstring(argv[argc-1]) == L"--profile-only";
        const bool xr_panel = argc >= 4 && std::wstring(argv[argc-1]) == L"--xr-panel";
        const int positional = argc - ((trace || profile_only || xr_panel) ? 1 : 0);
        if (profile_only && positional != 4) throw std::runtime_error("--profile-only requires a development profile");
        if (positional != 3 && positional != 4) {
            std::cout << "Usage: tdvr_capture_launcher <development-exe> <tdvr_capture.dll> [development-profile] [--trace|--profile-only|--xr-panel]\n"
                << "XR panel is an opt-in FLAT game screen, not stereo VR. Profile-only leaves rendering to child modules.\n";
            return argc == 1 ? 0 : 2;
        }
        const auto exe = std::filesystem::canonical(argv[1]);
        const auto dll = std::filesystem::canonical(argv[2]);
        if (_wcsicmp(dll.filename().c_str(), L"tdvr_capture.dll") != 0)
            throw std::runtime_error("only this project's tdvr_capture.dll is supported");
        if (exe == dll) throw std::runtime_error("executable and module must differ");
        // Load locally only to resolve our own export RVAs. It has no DllMain hooks,
        // starts no threads/devices/runtime, and is not initialized in this process.
        HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        wincheck(local != nullptr, "load capture DLL for export resolution");
        const auto start = GetProcAddress(local, "TdvrStartCapture");
        const auto getstats = GetProcAddress(local, "TdvrGetCaptureStats");
        const auto configure = GetProcAddress(local, "TdvrConfigureDevelopmentProfile");
        const auto start_profile = GetProcAddress(local, "TdvrStartDevelopmentProfile");
        const auto enable_panel = GetProcAddress(local, "TdvrEnableXrPanel");
        const auto get_panel = GetProcAddress(local, "TdvrGetPanelStats");
        const auto enable_trace = GetProcAddress(local, "TdvrEnableRenderTrace");
        const auto get_trace = GetProcAddress(local, "TdvrGetRenderTrace");
        if (!start || !getstats || !configure || !start_profile || !enable_panel || !get_panel || !enable_trace || !get_trace) { FreeLibrary(local); throw std::runtime_error("capture exports missing"); }
        const auto start_rva = reinterpret_cast<uintptr_t>(start) - reinterpret_cast<uintptr_t>(local);
        const auto stats_rva = reinterpret_cast<uintptr_t>(getstats) - reinterpret_cast<uintptr_t>(local);
        const auto configure_rva = reinterpret_cast<uintptr_t>(configure) - reinterpret_cast<uintptr_t>(local);
        const auto profile_rva = reinterpret_cast<uintptr_t>(start_profile) - reinterpret_cast<uintptr_t>(local);
        const auto enable_panel_rva = reinterpret_cast<uintptr_t>(enable_panel) - reinterpret_cast<uintptr_t>(local);
        const auto get_panel_rva = reinterpret_cast<uintptr_t>(get_panel) - reinterpret_cast<uintptr_t>(local);
        const auto enable_trace_rva = reinterpret_cast<uintptr_t>(enable_trace) - reinterpret_cast<uintptr_t>(local);
        const auto get_trace_rva = reinterpret_cast<uintptr_t>(get_trace) - reinterpret_cast<uintptr_t>(local);
        FreeLibrary(local);
        Child child;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        std::wstring command = L"\"" + exe.wstring() + L"\"";
        std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(0);
        std::vector<wchar_t> environment;
        if (positional == 4) {
            // Supply isolation at process creation too: the CRT can cache its
            // environment during loader bootstrap, before our configure RPC.
            const auto profile = std::filesystem::absolute(argv[3]).lexically_normal();
            if (profile == profile.root_path() || profile.wstring().size() > MAX_PATH - 40)
                throw std::runtime_error("development profile must be a non-root short absolute path");
            const auto inherited = GetEnvironmentStringsW();
            wincheck(inherited != nullptr, "read environment");
            for (const wchar_t* entry = inherited; *entry; entry += wcslen(entry) + 1) {
                bool replace = false;
                for (const auto* key : {L"LOCALAPPDATA=", L"APPDATA=", L"USERPROFILE=", L"TDVR_DEV_PROFILE="})
                    if (_wcsnicmp(entry, key, wcslen(key)) == 0) replace = true;
                if (!replace) environment.insert(environment.end(), entry, entry + wcslen(entry) + 1);
            }
            FreeEnvironmentStringsW(inherited);
            for (const auto& pair : {std::pair{L"LOCALAPPDATA=", profile / L"AppData/Local"},
                 std::pair{L"APPDATA=", profile / L"AppData/Roaming"},
                 std::pair{L"USERPROFILE=", profile}, std::pair{L"TDVR_DEV_PROFILE=", profile}}) {
                std::filesystem::create_directories(pair.second);
                const auto text = std::wstring(pair.first) + pair.second.wstring();
                environment.insert(environment.end(), text.begin(), text.end());
                environment.push_back(0);
            }
            std::vector<std::wstring> sorted;
            for (size_t offset = 0; offset < environment.size();) {
                sorted.emplace_back(environment.data() + offset);
                offset += sorted.back().size() + 1;
            }
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
            environment.clear();
            for (const auto& entry : sorted) {
                environment.insert(environment.end(), entry.begin(), entry.end());
                environment.push_back(0);
            }
            environment.push_back(0);
        }
        wincheck(CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | (environment.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
            environment.empty() ? nullptr : environment.data(), exe.parent_path().c_str(), &startup, &child.info) != FALSE, "CreateProcessW");
        BOOL wow64 = FALSE;
        wincheck(IsWow64Process(child.info.hProcess, &wow64) != FALSE, "child architecture");
        if (wow64) throw std::runtime_error("only x64 development children are supported");
        // Initialize the normal Windows loader without resuming the primary thread.
        // New threads run loader initialization before their entry. Exit immediately
        // through ntdll, which is already mapped, then locate KernelBase normally.
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto exit_thread = GetProcAddress(ntdll, "RtlExitUserThread");
        wincheck(exit_thread != nullptr, "RtlExitUserThread");
        const auto exit_offset = reinterpret_cast<uintptr_t>(exit_thread) - reinterpret_cast<uintptr_t>(ntdll);
        std::cout << "Initializing the child loader before primary main\n";
        remote_call(child.info.hProcess, mapped_module(child.info.hProcess, L"ntdll.dll") + exit_offset, nullptr, "Windows loader bootstrap");
        const auto loadlibrary = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        wincheck(loadlibrary != nullptr, "LoadLibraryW address");
        // Windows may forward kernel32 exports to KernelBase. Resolve the module
        // containing the actual function and add its offset to the child's base.
        HMODULE loader_module = nullptr;
        wincheck(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(loadlibrary), &loader_module) != FALSE, "loader owning module");
        wchar_t loader_path[MAX_PATH]{};
        wincheck(GetModuleFileNameW(loader_module, loader_path, MAX_PATH) != 0, "loader module filename");
        const auto loader_offset = reinterpret_cast<uintptr_t>(loadlibrary) - reinterpret_cast<uintptr_t>(loader_module);
        const auto remote_loader = remote_module(child.info.dwProcessId, std::filesystem::path(loader_path).filename().wstring()) + loader_offset;
        const auto dll_text = dll.wstring();
        const SIZE_T path_bytes = (dll_text.size() + 1) * sizeof(wchar_t);
        void* path_buffer = VirtualAllocEx(child.info.hProcess, nullptr, path_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        wincheck(path_buffer != nullptr, "VirtualAllocEx path");
        wincheck(WriteProcessMemory(child.info.hProcess, path_buffer, dll_text.c_str(), path_bytes, nullptr) != FALSE, "write DLL path");
        std::cout << "Loading capture module in child\n";
        remote_call(child.info.hProcess, remote_loader, path_buffer, "capture LoadLibrary");
        wincheck(VirtualFreeEx(child.info.hProcess, path_buffer, 0, MEM_RELEASE) != FALSE, "free DLL path");
        // Remote thread DWORD truncates x64 HMODULE; enumerate modules instead.
        const auto remote_dll = remote_module(child.info.dwProcessId, dll.filename().wstring());
        if (positional == 4) {
            const auto profile = std::filesystem::absolute(argv[3]).lexically_normal();
            if (profile == profile.root_path() || profile.wstring().size() > MAX_PATH - 40)
                throw std::runtime_error("development profile must be a non-root short absolute path");
            for (const auto* suffix : {L"AppData\\Local", L"AppData\\Roaming", L"Documents"})
                std::filesystem::create_directories(profile / suffix);
            const auto text = profile.wstring();
            const SIZE_T size = (text.size() + 1) * sizeof(wchar_t);
            void* remote_profile = VirtualAllocEx(child.info.hProcess, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            wincheck(remote_profile != nullptr, "allocate profile path");
            wincheck(WriteProcessMemory(child.info.hProcess, remote_profile, text.c_str(), size, nullptr) != FALSE, "write profile path");
            const auto configured = remote_call(child.info.hProcess, remote_dll + configure_rva, remote_profile, "profile configuration");
            wincheck(VirtualFreeEx(child.info.hProcess, remote_profile, 0, MEM_RELEASE) != FALSE, "free profile path");
            if (FAILED(static_cast<HRESULT>(configured))) throw std::runtime_error("profile configuration failed");
        }
        if (xr_panel && FAILED(static_cast<HRESULT>(remote_call(child.info.hProcess,
            remote_dll + enable_panel_rva, nullptr, "enable flat XR panel"))))
            throw std::runtime_error("XR panel configuration failed");
        if (trace && FAILED(static_cast<HRESULT>(remote_call(child.info.hProcess,
            remote_dll + enable_trace_rva, nullptr, "enable render trace"))))
            throw std::runtime_error("render trace configuration failed");
        std::cout << (profile_only ? "Starting folder redirection only; no own graphics hooks\n" : "Initializing capture hooks before primary main\n");
        const DWORD init = remote_call(child.info.hProcess, remote_dll + (profile_only ? profile_rva : start_rva), nullptr, "startup");
        if (FAILED(static_cast<HRESULT>(init))) throw std::runtime_error("initialization failed HRESULT=" + std::to_string(init));
        if (profile_only) {
            wincheck(ResumeThread(child.info.hThread) != static_cast<DWORD>(-1), "ResumeThread");
            std::cout << "Profile-only child pid=" << child.info.dwProcessId << "; rendering belongs to child modules\n";
            wincheck(WaitForSingleObject(child.info.hProcess, INFINITE) == WAIT_OBJECT_0, "wait profile child");
            child.exited = true;
            DWORD code = 0;
            wincheck(GetExitCodeProcess(child.info.hProcess, &code) != FALSE, "child exit code");
            std::cout << "Profile-only child exit=" << code << '\n';
            return static_cast<int>(code);
        }
        void* stats_buffer = VirtualAllocEx(child.info.hProcess, nullptr, sizeof(TdvrCaptureStats), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        wincheck(stats_buffer != nullptr, "VirtualAllocEx stats");
        void* trace_buffer = trace ? VirtualAllocEx(child.info.hProcess, nullptr, sizeof(TdvrRenderTrace),
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : nullptr;
        if (trace) wincheck(trace_buffer != nullptr, "allocate trace snapshot");
        wincheck(ResumeThread(child.info.hThread) != static_cast<DWORD>(-1), "ResumeThread");
        uint64_t last_captures = 0;
        uint32_t trace_records = 0, polls = 0;
        uint64_t trace_generation = 0;
        uint64_t panel_submitted = 0;
        uint32_t panel_stage = 0;
        for (;;) {
            const DWORD wait = WaitForSingleObject(child.info.hProcess, 100);
            if (wait == WAIT_OBJECT_0) break;
            wincheck(wait == WAIT_TIMEOUT, "wait child");
            TdvrCaptureStats stats{}; stats.size = sizeof(stats); stats.abi = 1;
            try {
                wincheck(WriteProcessMemory(child.info.hProcess, stats_buffer, &stats, sizeof(stats), nullptr) != FALSE, "initialize remote stats");
                const DWORD status = remote_call(child.info.hProcess, remote_dll + stats_rva, stats_buffer);
                if (FAILED(static_cast<HRESULT>(status))) throw std::runtime_error("remote stats failed");
                wincheck(ReadProcessMemory(child.info.hProcess, stats_buffer, &stats, sizeof(stats), nullptr) != FALSE, "read stats");
                if (xr_panel) {
                    static_assert(sizeof(TdvrPanelStats) <= sizeof(TdvrCaptureStats));
                    TdvrPanelStats panel{}; panel.size = sizeof(panel); panel.abi = 2;
                    wincheck(WriteProcessMemory(child.info.hProcess, stats_buffer, &panel, sizeof(panel), nullptr) != FALSE, "initialize panel stats");
                    if (FAILED(static_cast<HRESULT>(remote_call(child.info.hProcess, remote_dll + get_panel_rva, stats_buffer))))
                        throw std::runtime_error("panel stats failed");
                    wincheck(ReadProcessMemory(child.info.hProcess, stats_buffer, &panel, sizeof(panel), nullptr) != FALSE, "read panel stats");
                    if (panel.stage != panel_stage || panel.submitted >= panel_submitted + 60) {
                        panel_stage = panel.stage; panel_submitted = panel.submitted;
                        std::cout << "FLAT XR panel stage=" << panel.stage << " frames=" << panel.frames
                            << " submitted=" << panel.submitted << " xr=" << panel.xr_result
                            << " gpu=" << panel.gpu_result << " quarantined=" << panel.quarantined
                            << " src=" << panel.source_format << " xr_format=" << panel.swapchain_format
                            << " storage=" << panel.resource_format << " color=" << panel.color_space
                            << " failed_at=" << panel.failure_stage << '\n';
                    }
                }
                if (trace && (++polls % 10 == 0)) {
                    TdvrRenderTrace snapshot{}; snapshot.size = sizeof(snapshot); snapshot.abi = 1;
                    wincheck(WriteProcessMemory(child.info.hProcess, trace_buffer, &snapshot, sizeof(snapshot), nullptr) != FALSE, "initialize trace snapshot");
                    if (FAILED(static_cast<HRESULT>(remote_call(child.info.hProcess, remote_dll + get_trace_rva, trace_buffer))))
                        throw std::runtime_error("render trace snapshot failed");
                    wincheck(ReadProcessMemory(child.info.hProcess, trace_buffer, &snapshot, sizeof(snapshot), nullptr) != FALSE, "read trace snapshot");
                    if (snapshot.record_count > 64) throw std::runtime_error("invalid trace record count");
                    if (trace_generation != snapshot.generation) { trace_records = 0; trace_generation = snapshot.generation; }
                    for (; trace_records < snapshot.record_count; ++trace_records) {
                        const auto& record = snapshot.records[trace_records];
                        if (record.depth > 24) throw std::runtime_error("invalid trace depth");
                        std::cout << "render_trace command=" << record.command << " first_present=" << record.first_present
                            << " mask=" << snapshot.hook_mask << " exe_return_rvas=";
                        for (uint32_t i = 0; i < record.depth; ++i) std::cout << " 0x" << std::hex << record.executable_rvas[i];
                        std::cout << std::dec << '\n';
                    }
                }
            } catch (...) {
                // Child may exit normally between the process wait and RPC. Only
                // swallow the error if the process has actually terminated.
                if (WaitForSingleObject(child.info.hProcess, 0) == WAIT_OBJECT_0) break;
                throw;
            }
            if (stats.captured_frames != last_captures) {
                last_captures = stats.captured_frames;
                std::cout << "captured=" << stats.captured_frames << " size=" << stats.width << 'x' << stats.height
                    << " presents=" << stats.presents << " pixel_hash=" << stats.pixel_hash << '\n';
            }
        }
        child.exited = true;
        DWORD code = 0;
        wincheck(GetExitCodeProcess(child.info.hProcess, &code) != FALSE, "child exit code");
        std::cout << (xr_panel ? "XR-panel child exit=" : "capture-only child exit=") << code << '\n';
        return static_cast<int>(code);
    } catch (const std::exception& error) {
        std::cerr << "Capture launch failed: " << error.what() << '\n';
        return 1;
    }
}
