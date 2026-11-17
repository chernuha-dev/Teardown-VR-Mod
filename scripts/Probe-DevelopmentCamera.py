#!/usr/bin/env python3
"""Load/read the camera diagnostic ONLY in an audited local/ development game.

Default: observations only. Explicit --camera-test applies a bounded synthetic
camera during world render. --xr-camera-test consumes a running own XR session.
No VR startup by this script, signature guessing or live-install attachment.
SPDX-License-Identifier: MIT
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import importlib.util
import json
import math
import time
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


class RenderStack(c.Structure):
    _fields_ = [("command", c.c_uint32), ("depth", c.c_uint32),
                ("observations", c.c_uint64), ("first_present", c.c_uint64), ("rvas", c.c_uint32 * 24)]


class WorldRenderStats(c.Structure):
    _fields_ = [("size", c.c_uint32), ("abi", c.c_uint32),
                *[(name, c.c_uint64) for name in ("calls", "completed_calls", "valid_samples", "invalid_samples",
                  "main_camera_matches", "camera_mutations", "serial_steps", "history_swaps")],
                *[(name, c.c_uint32) for name in ("first_thread", "thread_changes", "signature_matched", "hook_installed")],
                ("camera", c.c_float * 34)]


class EngineResourceInfo(c.Structure):
    _fields_ = [("width", c.c_uint64)] + [(name, c.c_uint32) for name in (
        "height", "format", "flags", "samples", "dimension", "mips", "layers", "reserved")]


class EngineTargetStats(c.Structure):
    _fields_ = [("size", c.c_uint32), ("abi", c.c_uint32),
                *[(name, c.c_uint64) for name in ("samples", "valid_samples", "rejected_samples")],
                *[(name, c.c_uint32) for name in ("width", "height", "count", "game_state", "known_mask", "matching_extent_mask")],
                ("colors", EngineResourceInfo * 8)]


class RenderTrace(c.Structure):
    _fields_ = [("size", c.c_uint32), ("abi", c.c_uint32),
                ("commands", c.c_uint64), ("stack_samples", c.c_uint64), ("dropped", c.c_uint64),
                ("hook_mask", c.c_uint32), ("record_count", c.c_uint32),
                ("last_hresult", c.c_int32), ("exhausted", c.c_uint32), ("generation", c.c_uint64), ("records", RenderStack * 64)]


class TargetView(c.Structure):
    _fields_ = [("creation", c.c_uint64), ("width", c.c_uint64),
                *[(name, c.c_uint32) for name in ("height", "resource_format", "view_format", "dimension", "samples", "flags")]]


class TargetBinding(c.Structure):
    _fields_ = [(name, c.c_uint32) for name in ("count", "contiguous", "depth", "known_mask")] + [
        ("rvas", c.c_uint32 * 12), ("views", TargetView * 8)]


class TargetTrace(c.Structure):
    _fields_ = [("size", c.c_uint32), ("abi", c.c_uint32),
                *[(name, c.c_uint64) for name in ("bindings", "descriptor_writes", "dropped")],
                ("hook_mask", c.c_uint32), ("record_count", c.c_uint32),
                ("last_hresult", c.c_int32), ("reserved", c.c_uint32), ("records", TargetBinding * 32)]


class FrameKey(c.Structure):
    _fields_ = [("simulation_id", c.c_uint64), ("time_ns", c.c_int64)]


class Pose(c.Structure):
    _fields_ = [("position", c.c_double * 3), ("orientation", c.c_double * 4)]


class Fov(c.Structure):
    _fields_ = [("left", c.c_double), ("right", c.c_double), ("up", c.c_double), ("down", c.c_double)]


class EyeSample(c.Structure):
    _fields_ = [("frame", FrameKey), ("pose", Pose), ("fov", Fov), ("flags", c.c_uint32)]


class FrameInput(c.Structure):
    _fields_ = [("frame", FrameKey), ("origin", Pose), ("scale", c.c_double),
                ("near", c.c_double), ("far", c.c_double), ("eyes", EyeSample * 2)]


class CameraRequest(c.Structure):
    _fields_ = [(name, c.c_uint32) for name in ("size", "abi", "eye", "render_calls", "duration_ms", "reserved")] + [("frame", FrameInput)]


class TrackedCameraRequest(c.Structure):
    _fields_ = [(name, c.c_uint32) for name in ("size", "abi", "eye", "render_calls", "duration_ms", "reserved")] + [
        ("world_anchor", Pose), ("world_scale", c.c_double), ("near_clip_m", c.c_double), ("far_clip_m", c.c_double)]


class CameraOverrideStats(c.Structure):
    _fields_ = [("size", c.c_uint32), ("abi", c.c_uint32),
                *[(name, c.c_uint64) for name in ("accepted_requests", "applied_calls", "restored_calls", "rejected_calls")],
                ("remaining_calls", c.c_uint32), ("reserved", c.c_uint32), ("last_camera", c.c_float * 34)]


def probe(pid, dll, load=True, render_trace=False, reset_trace=False, world_render=False, camera_test=None, override_stats=False, target_trace=False, engine_targets=False, control=False):
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
                    if name.lower() == dll.name.lower() and Path(entry.path).resolve() != dll:
                        raise ValueError("Loaded diagnostic DLL path differs from --dll; refusing mismatched export offsets")
                    return entry.base
                present = k.Module32NextW(snapshot, c.byref(entry))
        finally:
            k.CloseHandle(snapshot)
        raise RuntimeError("module not loaded: " + name)

    dll = Path(dll).resolve(strict=True)
    expected = "tdvr_control.dll" if control else ("tdvr_capture.dll" if render_trace else "tdvr_engine_probe.dll")
    if dll.name != expected or not dll.is_relative_to((REPO / "build").resolve()):
        raise ValueError("Only this repository's built diagnostic module is supported")
    if render_trace and load:
        raise ValueError("Render trace requires the capture launcher --trace; late capture loading is unsupported")
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
            start_address = check(k.GetProcAddress(local, b"TdvrStartControl" if control else (b"TdvrResetTargetTrace" if target_trace else (b"TdvrResetRenderTrace" if render_trace else b"TdvrStartEngineProbe"))), "start export")
            stats_export = b"TdvrGetTargetTrace" if target_trace else (b"TdvrGetRenderTrace" if render_trace else (b"TdvrGetWorldRenderStats" if world_render else
                            (b"TdvrGetCameraOverrideStats" if override_stats else b"TdvrGetEngineStats")))
            if engine_targets: stats_export = b"TdvrGetEngineTargetStats"
            stats_address = start_address if control else check(k.GetProcAddress(local, stats_export), "stats export")
            start_offset, stats_offset = start_address - local, stats_address - local
            if camera_test is not None:
                submit_export = b"TdvrSubmitTrackedCameraPreview" if camera_test.get("tracked") else b"TdvrSubmitCameraOverride"
                submit_offset = check(k.GetProcAddress(local, submit_export), "camera request export") - local
                override_offset = check(k.GetProcAddress(local, b"TdvrGetCameraOverrideStats"), "camera status export") - local
                world_offset = check(k.GetProcAddress(local, b"TdvrGetWorldRenderStats"), "world render export") - local
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
        if control:
            return {"schema_version": 1, "runtime_started": False, "control_server": True,
                    "pipe": rf"\\.\pipe\tdvr-dev-{pid}"}
        if render_trace and reset_trace:
            status = call(module(dll.name) + start_offset)
            if status & 0x80000000:
                raise RuntimeError(f"trace reset refused HRESULT=0x{status:08x}")
        def exchange(offset, value, readback=True):
            remote = check(k.VirtualAllocEx(process, None, c.sizeof(value), 0x3000, 4), "allocate diagnostic argument")
            check(k.WriteProcessMemory(process, remote, c.byref(value), c.sizeof(value), None), "initialize diagnostic argument")
            status = call(module(dll.name) + offset, remote)  # timeout intentionally retains in-flight storage
            if not status & 0x80000000 and readback:
                check(k.ReadProcessMemory(process, remote, c.byref(value), c.sizeof(value), None), "read diagnostic result")
            check(k.VirtualFreeEx(process, remote, 0, 0x8000), "free diagnostic argument")
            if status & 0x80000000:
                raise RuntimeError(f"diagnostic call refused HRESULT=0x{status:08x}")
            return value

        def read_stats(offset, kind):
            value = kind(); value.size, value.abi = c.sizeof(value), 1
            return exchange(offset, value)

        kind = TargetTrace if target_trace else (RenderTrace if render_trace else (WorldRenderStats if world_render else (CameraOverrideStats if override_stats else EngineStats)))
        if engine_targets: kind = EngineTargetStats
        stats = read_stats(stats_offset, kind)
        if camera_test is not None:
            request = TrackedCameraRequest() if camera_test.get("tracked") else CameraRequest()
            request.size, request.abi = c.sizeof(request), 1
            if camera_test.get("tracked") and not camera_test.get("cancel"):
                world = read_stats(world_offset, WorldRenderStats)
                if not stats.valid_samples or not world.main_camera_matches:
                    raise ValueError("Observe a gameplay camera before submitting a tracked preview")
                request.eye = camera_test["eye"]; request.render_calls = 600; request.duration_ms = 10000
                request.world_anchor = Pose(stats.position, stats.orientation)
                request.world_scale = 1; request.near_clip_m, request.far_clip_m = world.camera[32:34]
            elif not camera_test.get("cancel"):
                world = read_stats(world_offset, WorldRenderStats)
                if not stats.valid_samples or not world.main_camera_matches:
                    raise ValueError("Observe a gameplay camera before submitting a synthetic test")
                request.eye = camera_test["eye"]; request.render_calls = 600; request.duration_ms = 10000
                frame = request.frame
                frame.frame = FrameKey(time.monotonic_ns(), 0)  # diagnostic tag, not XR predicted display time
                frame.origin = Pose(stats.position, stats.orientation)
                frame.scale = 1; frame.near, frame.far = world.camera[32:34]
                p = world.camera
                fov = camera_test.get("fov")
                if fov is None:
                    fov = [math.atan((p[8]-1)/p[0]), math.atan((p[8]+1)/p[0]),
                           math.atan((p[9]+1)/p[5]), math.atan((p[9]-1)/p[5])]
                else: fov = [math.radians(x) for x in fov]
                for eye in range(2):
                    sample = frame.eyes[eye]; sample.frame = frame.frame; sample.flags = 15
                    sample.pose.position[:] = camera_test["offset"]
                    sample.pose.position[0] += -.032 if eye == 0 else .032
                    angle = math.radians(camera_test["yaw"])/2
                    sample.pose.orientation[:] = (0, math.sin(angle), 0, math.cos(angle))
                    sample.fov = Fov(*fov)
            exchange(submit_offset, request, readback=False)
            stats = read_stats(override_offset, CameraOverrideStats)
        if engine_targets:
            if stats.count > 8: raise RuntimeError("invalid engine target bounds")
            return {"schema_version": 1, "observational_only": True, "runtime_started": False,
                    **{name: getattr(stats, name) for name, _ in EngineTargetStats._fields_ if name not in ("size", "abi", "colors")},
                    "colors": [{name: getattr(v, name) for name, _ in EngineResourceInfo._fields_ if name != "reserved"}
                               if stats.known_mask & (1 << i) else None for i, v in enumerate(stats.colors[:stats.count])]}
        if target_trace:
            if stats.record_count > 32 or any(s.depth > 12 or s.count > 8 for s in stats.records[:stats.record_count]):
                raise RuntimeError("invalid target trace bounds")
            return {"schema_version": 1, "observational_only": True, "runtime_started": False,
                    **{name: getattr(stats, name) for name, _ in TargetTrace._fields_ if name not in ("size", "abi", "records", "reserved")},
                    "records": [{"count": s.count, "contiguous": bool(s.contiguous), "known_mask": s.known_mask,
                                 "executable_return_rvas": [f"0x{x:x}" for x in s.rvas[:s.depth]],
                                 "views": [{name: getattr(v, name) for name, _ in TargetView._fields_}
                                           if s.known_mask & (1 << i) else None for i, v in enumerate(s.views[:s.count])]}
                                for s in stats.records[:stats.record_count]]}
        if render_trace:
            if stats.record_count > 64 or any(s.depth > 24 for s in stats.records[:stats.record_count]):
                raise RuntimeError("invalid render trace bounds")
            return {"schema_version": 1, "observational_only": True, "runtime_started": False,
                    **{name: getattr(stats, name) for name, _ in RenderTrace._fields_ if name not in ("size", "abi", "records")},
                    "records": [{"command": s.command, "observations": s.observations, "first_present": s.first_present,
                                 "executable_return_rvas": [f"0x{x:x}" for x in s.rvas[:s.depth]]}
                                for s in stats.records[:stats.record_count]]}
        return {"schema_version": 1, "observational_only": camera_test is None and not override_stats, "runtime_started": False,
                **{name: list(getattr(stats, name)) if name in ("position", "orientation", "projection", "camera", "last_camera") else getattr(stats, name)
                   for name, _ in stats._fields_ if name not in ("size", "abi", "reserved")}}
    finally:
        k.CloseHandle(process)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", required=True, type=int)
    parser.add_argument("--dll", type=Path)
    parser.add_argument("--stats-only", action="store_true")
    parser.add_argument("--render-trace", action="store_true", help="Read the already initialized capture module's recording stacks")
    parser.add_argument("--target-trace", action="store_true", help="Read observed RTV resource descriptions and binding call stacks")
    parser.add_argument("--world-render", action="store_true", help="Observe the audited scene render boundary without changing its arguments")
    parser.add_argument("--engine-targets", action="store_true", help="Read verified D3D12 resource metadata for the world-render output")
    parser.add_argument("--reset-trace", action="store_true", help="Rearm the capture trace window for the current scene")
    parser.add_argument("--camera-test", type=float, nargs=3, metavar=("X", "Y", "Z"), help="Apply a synthetic eye offset in metres for <=10 seconds, only in development")
    parser.add_argument("--test-yaw", type=float, default=0, help="Synthetic local yaw in degrees")
    parser.add_argument("--test-fov", type=float, nargs=4, metavar=("LEFT", "RIGHT", "UP", "DOWN"), help="Signed synthetic FOV angles in degrees")
    parser.add_argument("--test-eye", choices=("left", "right"), default="left")
    parser.add_argument("--cancel-camera-test", action="store_true")
    parser.add_argument("--xr-camera-test", action="store_true", help="Bounded live-pose single-eye preview; requires --xr-world-timing game")
    parser.add_argument("--override-stats", action="store_true")
    args = parser.parse_args()
    try:
        if args.target_trace and args.render_trace:
            raise ValueError("Select target trace or recording trace")
        args.render_trace = args.render_trace or args.target_trace
        if args.engine_targets and (args.render_trace or args.world_render or args.override_stats or args.camera_test is not None or args.cancel_camera_test or args.xr_camera_test):
            raise ValueError("Engine target observation is a separate diagnostic mode")
        if args.reset_trace and not args.render_trace:
            raise ValueError("--reset-trace requires --render-trace")
        if args.world_render and args.render_trace:
            raise ValueError("Select either world render observation or recording trace")
        camera_test = None
        if args.xr_camera_test and args.camera_test is not None:
            raise ValueError("Select synthetic or tracked camera test")
        if args.camera_test is not None or args.cancel_camera_test or args.xr_camera_test:
            if args.world_render or args.render_trace or args.override_stats:
                raise ValueError("Camera test is a separate diagnostic mode")
            camera_test = {"cancel": args.cancel_camera_test, "offset": args.camera_test,
                           "tracked": args.xr_camera_test, "yaw": args.test_yaw, "fov": args.test_fov, "eye": int(args.test_eye == "right")}
        if args.override_stats and (args.world_render or args.render_trace):
            raise ValueError("Override stats are a separate diagnostic mode")
        dll = args.dll or REPO / "build/native/Release" / ("tdvr_capture.dll" if args.render_trace else "tdvr_engine_probe.dll")
        print(json.dumps(probe(args.pid, dll, not (args.stats_only or args.render_trace or args.override_stats), args.render_trace,
                               args.reset_trace, args.world_render, camera_test, args.override_stats, args.target_trace, args.engine_targets), indent=2, allow_nan=False))
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(json.dumps({"error": str(error), "runtime_started": False}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
