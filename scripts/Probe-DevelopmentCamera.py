#!/usr/bin/env python3
"""Load/read the observational camera probe ONLY in an audited local/ development game.

No game mutation, VR runtime startup, signature guessing, or attach to live installs.
SPDX-License-Identifier: MIT
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import importlib.util
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


class ModuleEntry(c.Structure):
    _fields_ = [("size", w.DWORD), ("module_id", w.DWORD), ("pid", w.DWORD),
                ("global_count", w.DWORD), ("process_count", w.DWORD), ("base", c.c_void_p),
                ("module_size", w.DWORD), ("module", w.HMODULE),
                ("name", w.WCHAR * 256), ("path", w.WCHAR * 260)]


class EngineStats(c.Structure):
    _fields_ = [("size", c.c_uint32), ("abi", c.c_uint32),
                ("camera_calls", c.c_uint64), ("valid_samples", c.c_uint64), ("rejected_samples", c.c_uint64),
                ("position", c.c_double * 3), ("orientation", c.c_double * 4),
                ("maximum_view_error", c.c_double), ("projection", c.c_float * 16),
                ("identity_matched", c.c_uint32), ("signature_matched", c.c_uint32), ("hook_installed", c.c_uint32)]


def probe(pid, dll, load=True):
    k = c.WinDLL("kernel32", use_last_error=True)
    declarations = {
        "OpenProcess": ([w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
        "CloseHandle": ([w.HANDLE], w.BOOL),
        "QueryFullProcessImageNameW": ([w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)], w.BOOL),
        "GetModuleHandleW": ([w.LPCWSTR], w.HMODULE),
        "GetModuleHandleExW": ([w.DWORD, c.c_void_p, c.POINTER(w.HMODULE)], w.BOOL),
        "GetModuleFileNameW": ([w.HMODULE, w.LPWSTR, w.DWORD], w.DWORD),
        "GetProcAddress": ([w.HMODULE, c.c_char_p], c.c_void_p),
        "LoadLibraryW": ([w.LPCWSTR], w.HMODULE),
        "FreeLibrary": ([w.HMODULE], w.BOOL),
        "CreateToolhelp32Snapshot": ([w.DWORD, w.DWORD], w.HANDLE),
        "Module32FirstW": ([w.HANDLE, c.POINTER(ModuleEntry)], w.BOOL),
        "Module32NextW": ([w.HANDLE, c.POINTER(ModuleEntry)], w.BOOL),
        "VirtualAllocEx": ([w.HANDLE, c.c_void_p, c.c_size_t, w.DWORD, w.DWORD], c.c_void_p),
        "VirtualFreeEx": ([w.HANDLE, c.c_void_p, c.c_size_t, w.DWORD], w.BOOL),
        "WriteProcessMemory": ([w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)], w.BOOL),
        "ReadProcessMemory": ([w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)], w.BOOL),
        "CreateRemoteThread": ([w.HANDLE, c.c_void_p, c.c_size_t, c.c_void_p, c.c_void_p, w.DWORD, c.c_void_p], w.HANDLE),
        "WaitForSingleObject": ([w.HANDLE, w.DWORD], w.DWORD),
        "GetExitCodeThread": ([w.HANDLE, c.POINTER(w.DWORD)], w.BOOL),
    }
    for name, (args, result) in declarations.items():
        getattr(k, name).argtypes, getattr(k, name).restype = args, result

    def check(result, operation):
        if not result:
            raise OSError(c.get_last_error(), operation)
        return result

    def module(name):
        for _ in range(5):
            snapshot = k.CreateToolhelp32Snapshot(0x18, pid)
            if snapshot != c.c_void_p(-1).value:
                break
            if c.get_last_error() != 24:
                raise OSError(c.get_last_error(), "module snapshot")
        else:
            raise RuntimeError("unstable module enumeration")
        try:
            entry = ModuleEntry()
            entry.size = c.sizeof(entry)
            present = k.Module32FirstW(snapshot, c.byref(entry))
            while present:
                if entry.name.lower() == name.lower():
                    return entry.base
                present = k.Module32NextW(snapshot, c.byref(entry))
        finally:
            k.CloseHandle(snapshot)
        raise RuntimeError("module not loaded: " + name)

    dll = Path(dll).resolve(strict=True)
    if dll.name != "tdvr_engine_probe.dll" or not dll.is_relative_to((REPO / "build").resolve()):
        raise ValueError("Only this repository's built camera probe is supported")
    process = check(k.OpenProcess(0x043A, False, pid), "open development process")
    try:
        text = c.create_unicode_buffer(32768)
        length = w.DWORD(len(text))
        check(k.QueryFullProcessImageNameW(process, 0, text, c.byref(length)), "query image path")
        executable = Path(text.value).resolve(strict=True)
        if executable.name.lower() != "teardown.exe" or not executable.is_relative_to((REPO / "local").resolve()):
            raise ValueError("Refusing to attach outside this repository's local/ development copy")
        spec = importlib.util.spec_from_file_location("engine_identity", REPO / "scripts/Inspect-Teardown.py")
        identity = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(identity)
        report = identity.inspect_file(executable)
        if report["parse_status"] != "ok" or report["compatibility"]["identity_status"] != "observed_only":
            raise ValueError("Unknown development executable identity")

        def call(address, argument=None):
            thread = check(k.CreateRemoteThread(process, None, 0, address, argument, 0, None), "create probe RPC")
            try:
                if k.WaitForSingleObject(thread, 10000) != 0:
                    raise TimeoutError("probe RPC did not complete; do not unload module or free in-flight arguments")
                status = w.DWORD()
                check(k.GetExitCodeThread(thread, c.byref(status)), "RPC exit code")
                return status.value
            finally:
                k.CloseHandle(thread)

        local = check(k.LoadLibraryW(str(dll)), "load local probe exports")
        try:
            start_address = check(k.GetProcAddress(local, b"TdvrStartEngineProbe"), "start export")
            stats_address = check(k.GetProcAddress(local, b"TdvrGetEngineStats"), "stats export")
            start_offset, stats_offset = start_address - local, stats_address - local
        finally:
            k.FreeLibrary(local)
        if load:
            loader = check(k.GetProcAddress(k.GetModuleHandleW("kernel32.dll"), b"LoadLibraryW"), "loader export")
            owning = w.HMODULE()
            check(k.GetModuleHandleExW(6, loader, c.byref(owning)), "loader module")
            check(k.GetModuleFileNameW(owning, text, len(text)), "loader module name")
            remote_loader = module(Path(text.value).name) + loader - owning.value
            path = c.create_unicode_buffer(str(dll))
            remote = check(k.VirtualAllocEx(process, None, c.sizeof(path), 0x3000, 4), "allocate module path")
            check(k.WriteProcessMemory(process, remote, path, c.sizeof(path), None), "write module path")
            call(remote_loader, remote)  # On timeout remote allocation intentionally stays alive.
            check(k.VirtualFreeEx(process, remote, 0, 0x8000), "free module path")
            status = call(module(dll.name) + start_offset)
            if status & 0x80000000:
                raise RuntimeError(f"engine probe refused startup HRESULT=0x{status:08x}")
        stats = EngineStats()
        stats.size, stats.abi = c.sizeof(stats), 1
        remote = check(k.VirtualAllocEx(process, None, c.sizeof(stats), 0x3000, 4), "allocate stats")
        check(k.WriteProcessMemory(process, remote, c.byref(stats), c.sizeof(stats), None), "initialize stats")
        status = call(module(dll.name) + stats_offset, remote)
        if status & 0x80000000:
            raise RuntimeError(f"stats failed HRESULT=0x{status:08x}")
        check(k.ReadProcessMemory(process, remote, c.byref(stats), c.sizeof(stats), None), "read stats")
        check(k.VirtualFreeEx(process, remote, 0, 0x8000), "free stats")
        return {"schema_version": 1, "observational_only": True, "runtime_started": False,
                **{name: list(getattr(stats, name)) if name in ("position", "orientation", "projection") else getattr(stats, name)
                   for name, _ in stats._fields_ if name not in ("size", "abi")}}
    finally:
        k.CloseHandle(process)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", required=True, type=int)
    parser.add_argument("--dll", type=Path, default=REPO / "build/native/Release/tdvr_engine_probe.dll")
    parser.add_argument("--stats-only", action="store_true")
    args = parser.parse_args()
    try:
        print(json.dumps(probe(args.pid, args.dll, not args.stats_only), indent=2, allow_nan=False))
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(json.dumps({"error": str(error), "runtime_started": False}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
