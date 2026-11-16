#!/usr/bin/env python3
"""Read-only temporal-history audit of the pinned binaries and saved runtime code.

No process access, patching, hooks, resource allocation or runtime launch.
SPDX-License-Identifier: MIT
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct

REPO = Path(__file__).resolve().parents[1]
GAME_SHA = "2733e37969fd6d45c5c2130872b0d4096147c304266b4b0adda01bd98caa2c4d"
SNAPSHOT_SHA = "b2b804daa22e4c65ffeb8786f2947ff9d91a679079089bc375a07a98c0e10ccf"
DLL_SHA = "c315341efe9d1ec8825f79d8c6ed6e1a9ba1e61ab233d26c3b97c13b004478be"
TEXT_RVA = 0x1000
WORLD_RVA = 0x865B0
BUFFER_FIELDS = (0xA50, 0xA58, 0xAA8, 0xAB0, 0xAC0, 0xAC8, 0xAE0, 0xAE8, 0xAF8, 0xB00)


def matches(data, pattern, base=0):
    tokens = pattern.split()
    if not tokens or any(t != "??" and not re.fullmatch(r"[0-9a-fA-F]{2}", t) for t in tokens):
        raise ValueError("Invalid byte pattern")
    expression = b"".join(b"." if t == "??" else re.escape(bytes.fromhex(t)) for t in tokens)
    # Count overlapping candidates too; a uniqueness gate must not hide them.
    return [base + match.start() for match in re.finditer(b"(?="+expression+b")", data, re.DOTALL)]


def unique(data, pattern, base=0):
    found = matches(data, pattern, base)
    if len(found) != 1:
        raise ValueError(f"Expected one byte-pattern match; found {len(found)}")
    return found[0]


def relative_call(data, call_rva, base=TEXT_RVA):
    offset = call_rva - base
    if offset < 0 or offset > len(data)-5 or data[offset] != 0xE8:
        raise ValueError("Not an in-range direct CALL rel32")
    target = call_rva + 5 + struct.unpack_from("<i", data, offset+1)[0]
    if not base <= target < base+len(data):
        raise ValueError("CALL target outside the saved code")
    return target


def audit_code(runtime):
    start = WORLD_RVA-TEXT_RVA
    if start < 0 or start+0x600 > len(runtime):
        raise ValueError("Missing world-render code range")
    world = runtime[start:start+0x600]
    serial = unique(world, "48 FF 81 08 22 00 00 44 8B 81 14 22 00 00", WORLD_RVA)
    matrix = unique(world, "41 0F 10 87 EC 22 00 00 41 0F 11 87 2C 23 00 00", WORLD_RVA)
    taa_block = unique(world, "49 8B 8F 30 0A 00 00 E8 ?? ?? ?? ?? 4D 8D AF 58 22 00 00 41 0F 10 45 00 41 0F 11 87 6C 23 00 00", WORLD_RVA)
    reset = unique(world, "80 B9 F1 23 00 00 00", WORLD_RVA)
    taa_create = unique(runtime, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 56 41 57 48 8D 6C 24 B9 48 81 EC C0 00 00 00 48 8B F1 89 51 10 44 89 41 14", TEXT_RVA)
    release_sites = matches(runtime[taa_create-TEXT_RVA:taa_create-TEXT_RVA+0x200],
                            "48 85 C9 74 ?? E8 ?? ?? ?? ??", taa_create)
    if len(release_sites) != 3:
        raise ValueError("Unexpected TAA release-site count")
    release_targets = {relative_call(runtime, site+5) for site in release_sites}
    if len(release_targets) != 1:
        raise ValueError("TAA release calls disagree")
    buffer_release = unique(runtime, "48 85 C9 74 ?? E8 ?? ?? ?? ?? 90 66 C7 45 ?? 00 00 C7 45 ?? 01 00 00 00 C7 45 ?? ?? 00 00 00 48 8B 8B C0 09 00 00 48 8B 01 4C 8D 05", TEXT_RVA)
    return {
        "world_render_rva": WORLD_RVA,
        "legacy_guard_drift": [
            {"site": "serial_history_swap", "legacy_offset": 0x50, "observed_offset": serial-WORLD_RVA, "registers_changed": True},
            {"site": "previous_matrix_copy", "legacy_offset": 0x299, "observed_offset": matrix-WORLD_RVA, "registers_changed": False},
            {"site": "taa_and_previous_camera", "legacy_offset": 0x373, "observed_offset": taa_block-WORLD_RVA, "registers_changed": True},
        ],
        "reset_test_rva": reset,
        "taa_frame_function_rva": relative_call(runtime, taa_block+7),
        "taa_create_rva": taa_create,
        "taa_release_rva": next(iter(release_targets)),
        "taa_release_call_count": len(release_sites),
        "buffer_release_rva": relative_call(runtime, buffer_release+5),
    }


def inspect(game, snapshot, reference):
    blobs = {}
    for name, path, expected in (("game", game, GAME_SHA), ("snapshot", snapshot, SNAPSHOT_SHA), ("reference", reference, DLL_SHA)):
        blob = Path(path).read_bytes()
        if hashlib.sha256(blob).hexdigest() != expected:
            raise ValueError(f"Unknown {name} identity; audit refuses unverified binaries")
        blobs[name] = blob
    spec = importlib.util.spec_from_file_location("temporal_pe", REPO / "scripts/Inspect-Teardown.py")
    pe_module = importlib.util.module_from_spec(spec); spec.loader.exec_module(pe_module)
    pe = pe_module.PEImage(blobs["reference"])
    offset = pe.file_range(0x167598, 40)[0]
    fields = struct.unpack_from("<10I", blobs["reference"], offset)
    if fields != BUFFER_FIELDS:
        raise ValueError("Unexpected reference history-field table")
    return {
        "schema_version": 1, "read_only": True, "executed": False, "deployed": False,
        "stereo_replay_enabled": False,
        "identities": {"game": GAME_SHA, "snapshot": SNAPSHOT_SHA, "reference": DLL_SHA},
        **audit_code(blobs["snapshot"]),
        "reference_resource_contract": {
            "scene_buffer_fields": list(fields), "descriptor_bytes": 0x38,
            "taa_scene_field": 0xA30, "taa_resource_fields": [0x50,0x58,0x60,0x68,0x70],
            "renderer_scene_field": 0x9C0, "create_buffer_vtable_slot_bytes": 0x138,
            "wait_idle_vtable_slot_bytes": 0x1D0,
        },
        "unverified": ["current engine allocator/destructor ABI", "descriptor fields and ownership",
                       "GPU resource states and in-flight lifetime", "per-eye history isolation", "simulation freeze"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("game", "snapshot", "reference"):
        parser.add_argument("--"+name, type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(inspect(args.game,args.snapshot,args.reference), indent=2))
        return 0
    except (OSError,ValueError,struct.error) as error:
        print(json.dumps({"error":str(error),"executed":False,"deployed":False,"stereo_replay_enabled":False}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
