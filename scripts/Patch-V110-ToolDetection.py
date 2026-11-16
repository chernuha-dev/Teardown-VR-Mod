#!/usr/bin/env python3
"""Create an isolated, hash-gated v1.1.0 tool-detection experiment. Never deploy/run it.

SPDX-License-Identifier: MIT
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DLL_SHA = "c315341efe9d1ec8825f79d8c6ed6e1a9ba1e61ab233d26c3b97c13b004478be"
GAME_SHA = "2733e37969fd6d45c5c2130872b0d4096147c304266b4b0adda01bd98caa2c4d"
SNAPSHOT_SHA = "b2b804daa22e4c65ffeb8786f2947ff9d91a679079089bc375a07a98c0e10ccf"
CHANGES = ((0x78B32, 0x8B, 0x8D), (0x78B38, 0x49, 0x79))


def prepare(data, pe):
    if hashlib.sha256(data).hexdigest() != DLL_SHA:
        raise ValueError("Unknown source DLL; only the audited v1.1.0 binary is accepted")
    result = bytearray(data)
    for rva, old, new in CHANGES:
        offset = pe.file_range(rva, 1)[0]
        if result[offset] != old:
            raise ValueError("Unexpected tool-detector instruction")
        result[offset] = new
    return bytes(result)


def create(source, game, snapshot, output):
    output = Path(output).resolve()
    if output.name.lower() != "dsound.dll" or not output.is_relative_to((REPO / "local").resolve()):
        raise ValueError("Output must be a new dsound.dll under this repository local/ directory")
    if output.exists():
        raise ValueError("Output already exists; refusing to overwrite")
    game_data = Path(game).read_bytes()
    # Full EXE identity plus independently captured unpacked code are required.
    if hashlib.sha256(game_data).hexdigest() != GAME_SHA:
        raise ValueError("Unknown game executable")
    runtime = Path(snapshot).read_bytes()
    if hashlib.sha256(runtime).hexdigest() != SNAPSHOT_SHA:
        raise ValueError("Unknown runtime snapshot")
    if runtime[0x447583-0x1000:0x44758D-0x1000] != bytes.fromhex("48 8b f1 48 8d 79 58 48 8b 0f"):
        raise ValueError("Audited player-link instructions do not match")
    source = Path(source).resolve()
    data = source.read_bytes()
    spec = importlib.util.spec_from_file_location("engine_identity", REPO / "scripts/Inspect-Teardown.py")
    identity = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(identity)
    patched = prepare(data, identity.PEImage(data))
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as stream:
        stream.write(patched)
    return {"source_sha256": hashlib.sha256(data).hexdigest(),
            "output_sha256": hashlib.sha256(patched).hexdigest(),
            "changed_bytes": len(CHANGES), "deployed": False, "executed": False,
            "scope": "experimental v1.1.0 detector compatibility only; never pair with mod/ v0.22"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "game", "snapshot", "output"):
        parser.add_argument("--"+name, required=True, type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(create(args.source, args.game, args.snapshot, args.output), indent=2))
        return 0
    except (OSError, ValueError) as error:
        print(json.dumps({"error": str(error), "deployed": False, "executed": False}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
