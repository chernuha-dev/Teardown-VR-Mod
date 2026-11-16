#include "tdvr_capture.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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
DWORD remote_call(HANDLE process, uintptr_t entry, void* argument) {
    Handle thread(CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(entry), argument, 0, nullptr));
    wincheck(thread.value != nullptr, "CreateRemoteThread");
    // Never free a buffer still used by a timed-out remote thread. The outer
    // guard terminates only our suspended/owned test child on failure.
    if (WaitForSingleObject(thread.value, 10000) != WAIT_OBJECT_0)
        throw std::runtime_error("child initialization timed out");
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
        // This launcher never attaches to an existing process or the user's VR app.
        if (argc != 3) {
            std::cout << "Usage: tdvr_capture_launcher <development-exe> <tdvr_capture.dll>\n"
                << "Capture-only child process. No OpenXR/VR output. No game deployment.\n";
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
        if (!start || !getstats) { FreeLibrary(local); throw std::runtime_error("capture exports missing"); }
        const auto start_rva = reinterpret_cast<uintptr_t>(start) - reinterpret_cast<uintptr_t>(local);
        const auto stats_rva = reinterpret_cast<uintptr_t>(getstats) - reinterpret_cast<uintptr_t>(local);
        FreeLibrary(local);
        Child child;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        std::wstring command = L"\"" + exe.wstring() + L"\"";
        std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(0);
        wincheck(CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED, nullptr, exe.parent_path().c_str(), &startup, &child.info) != FALSE, "CreateProcessW");
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
        remote_call(child.info.hProcess, mapped_module(child.info.hProcess, L"ntdll.dll") + exit_offset, nullptr);
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
        remote_call(child.info.hProcess, remote_loader, path_buffer);
        wincheck(VirtualFreeEx(child.info.hProcess, path_buffer, 0, MEM_RELEASE) != FALSE, "free DLL path");
        // Remote thread DWORD truncates x64 HMODULE; enumerate modules instead.
        const auto remote_dll = remote_module(child.info.dwProcessId, dll.filename().wstring());
        const DWORD init = remote_call(child.info.hProcess, remote_dll + start_rva, nullptr);
        if (FAILED(static_cast<HRESULT>(init))) throw std::runtime_error("capture initialization failed");
        void* stats_buffer = VirtualAllocEx(child.info.hProcess, nullptr, sizeof(TdvrCaptureStats), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        wincheck(stats_buffer != nullptr, "VirtualAllocEx stats");
        wincheck(ResumeThread(child.info.hThread) != static_cast<DWORD>(-1), "ResumeThread");
        uint64_t last_captures = 0;
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
        std::cout << "capture-only child exit=" << code << "; no XR runtime used\n";
        return static_cast<int>(code);
    } catch (const std::exception& error) {
        std::cerr << "Capture launch failed: " << error.what() << '\n';
        return 1;
    }
}
