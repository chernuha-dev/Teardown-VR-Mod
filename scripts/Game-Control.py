#!/usr/bin/env python3
"""Local development-game API / stdio MCP bridge. Never moves/focuses the mouse.
SPDX-License-Identifier: MIT
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import importlib.util
import json
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[1]
COMMANDS = {"tdvr_status": "status", "tdvr_ping": "ping", "tdvr_engine_status": "engine-status",
            "tdvr_xr_views": "xr-views", "tdvr_request_exit": "request-exit"}


def attach(pid, dll):
    spec = importlib.util.spec_from_file_location("development_probe", REPO / "scripts/Probe-DevelopmentCamera.py")
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    return probe.probe(pid, dll, control=True)


def exchange(pid, command):
    if type(pid) is not int or pid <= 0 or command not in COMMANDS.values():
        raise ValueError("Invalid PID or command")
    k = c.WinDLL("kernel32", use_last_error=True)
    class Overlapped(c.Structure):
        _fields_ = [("internal", c.c_size_t), ("internal_high", c.c_size_t),
                    ("offset", w.DWORD), ("offset_high", w.DWORD), ("event", w.HANDLE)]
    declarations = {
        "WaitNamedPipeW": ([w.LPCWSTR, w.DWORD], w.BOOL),
        "CreateFileW": ([w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE], w.HANDLE),
        "GetNamedPipeServerProcessId": ([w.HANDLE, c.POINTER(w.ULONG)], w.BOOL),
        "SetNamedPipeHandleState": ([w.HANDLE, c.POINTER(w.DWORD), c.c_void_p, c.c_void_p], w.BOOL),
        "CreateEventW": ([c.c_void_p, w.BOOL, w.BOOL, w.LPCWSTR], w.HANDLE),
        "ReadFile": ([w.HANDLE, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.POINTER(Overlapped)], w.BOOL),
        "WriteFile": ([w.HANDLE, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.POINTER(Overlapped)], w.BOOL),
        "WaitForSingleObject": ([w.HANDLE, w.DWORD], w.DWORD),
        "GetOverlappedResult": ([w.HANDLE, c.POINTER(Overlapped), c.POINTER(w.DWORD), w.BOOL], w.BOOL),
        "CancelIoEx": ([w.HANDLE, c.POINTER(Overlapped)], w.BOOL),
        "CloseHandle": ([w.HANDLE], w.BOOL),
    }
    for name, (args, result) in declarations.items():
        getattr(k, name).argtypes, getattr(k, name).restype = args, result
    def check(ok, operation):
        if not ok:
            raise OSError(c.get_last_error(), operation)
        return ok
    name = rf"\\.\pipe\tdvr-dev-{pid}"
    check(k.WaitNamedPipeW(name, 1500), "development pipe unavailable; use --attach first")
    pipe = k.CreateFileW(name, 0xC0000000, 0, None, 3, 0x40000000, None)
    if pipe == c.c_void_p(-1).value:
        raise OSError(c.get_last_error(), "open development pipe")
    try:
        server_pid = w.ULONG()
        check(k.GetNamedPipeServerProcessId(pipe, c.byref(server_pid)), "pipe server identity")
        if server_pid.value != pid:
            raise ValueError("Pipe server PID does not match requested game")
        mode = w.DWORD(2)
        check(k.SetNamedPipeHandleState(pipe, c.byref(mode), None, None), "message mode")
        def io(write, payload=None):
            buffer = c.create_string_buffer(payload) if write else c.create_string_buffer(4096)
            event = check(k.CreateEventW(None, True, False, None), "I/O event")
            operation = Overlapped(); operation.event = event
            count = w.DWORD()
            try:
                ok = (k.WriteFile if write else k.ReadFile)(pipe, buffer, len(payload) if write else 4096,
                                                            c.byref(count), c.byref(operation))
                if not ok:
                    if c.get_last_error() != 997:
                        raise OSError(c.get_last_error(), "pipe I/O")
                    if k.WaitForSingleObject(event, 5000) != 0:
                        k.CancelIoEx(pipe, c.byref(operation))
                        # Named-pipe cancellation completes before buffers go away.
                        k.GetOverlappedResult(pipe, c.byref(operation), c.byref(count), True)
                        raise TimeoutError("Development pipe timed out")
                    check(k.GetOverlappedResult(pipe, c.byref(operation), c.byref(count), False), "I/O completion")
                return bytes(buffer.raw[:count.value])
            finally:
                k.CloseHandle(event)
        io(True, command.encode("ascii"))
        response = json.loads(io(False).decode("utf-8"))
        io(True, b"ACK")
        return response
    finally:
        k.CloseHandle(pipe)


def rpc(request):
    """MCP transport adapter; no game access for initialize/list/ping."""
    ident = request.get("id")
    method = request.get("method")
    if ident is None:
        return None
    result = {}
    try:
        if method == "initialize":
            result = {"protocolVersion": request.get("params", {}).get("protocolVersion", "2024-11-05"),
                      "capabilities": {"tools": {}}, "serverInfo": {"name": "teardown-vr-development", "version": "0.1.0"}}
        elif method == "tools/list":
            result = {"tools": [{"name": name, "description": command + " in the attached local development game; no mouse/focus control",
                       "inputSchema": {"type": "object", "properties": {"pid": {"type": "integer", "minimum": 1}},
                                       "required": ["pid"], "additionalProperties": False},
                       "annotations": {"readOnlyHint": command != "request-exit", "openWorldHint": False}}
                      for name, command in COMMANDS.items()]}
        elif method == "tools/call":
            params = request.get("params", {})
            if params.get("name") not in COMMANDS:
                raise ValueError("Unknown tool")
            args = params.get("arguments", {})
            if set(args) != {"pid"} or type(args["pid"]) is not int or args["pid"] <= 0:
                raise ValueError("Exactly one positive integer pid is required")
            try:
                value = exchange(args["pid"], COMMANDS[params["name"]])
                result = {"content": [{"type": "text", "text": json.dumps(value)}], "isError": not value.get("ok", False)}
            except (OSError, ValueError, RuntimeError, TimeoutError) as error:
                result = {"content": [{"type": "text", "text": str(error)}], "isError": True}
        elif method != "ping":
            return {"jsonrpc": "2.0", "id": ident, "error": {"code": -32601, "message": "Method not found"}}
        return {"jsonrpc": "2.0", "id": ident, "result": result}
    except (ValueError, KeyError, TypeError) as error:
        return {"jsonrpc": "2.0", "id": ident, "error": {"code": -32602, "message": str(error)}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--attach", action="store_true", help="Load only the gated control DLL, without XR or graphics hooks")
    parser.add_argument("--dll", type=Path, default=REPO / "build/native-trace/Release/tdvr_control.dll")
    parser.add_argument("--mcp", action="store_true", help="Serve newline-delimited stdio MCP")
    parser.add_argument("command", nargs="?", choices=tuple(COMMANDS.values()), default="status")
    args = parser.parse_args()
    if args.mcp:
        for line in sys.stdin:
            try:
                response = rpc(json.loads(line))
            except (ValueError, TypeError, AttributeError):
                response = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}}
            if response is not None:
                print(json.dumps(response), flush=True)
        return 0
    if not args.pid or args.pid < 1:
        parser.error("--pid is required without --mcp")
    try:
        if args.attach:
            attach(args.pid, args.dll)
        response = exchange(args.pid, args.command)
        print(json.dumps(response, indent=2))
        return 0 if response.get("ok") else 2
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        print(json.dumps({"ok": False, "error": str(error)}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
