#!/usr/bin/env python3
"""Opt-in unattended sandbox startup ONLY in an audited local/ game copy.
SPDX-License-Identifier: MIT
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

REPO = Path(__file__).resolve().parents[1]
GAME_SHA = "2733e37969fd6d45c5c2130872b0d4096147c304266b4b0adda01bd98caa2c4d"
SCRIPTS = {
    "splash.lua": ("030ab398300b601b4cd47823edeb3f1f39762ed75133c1d6782859ae1bbb1aac", b'''
-- TDVR development bootstrap (MIT); isolated copy only.
local tdvr_boot_splash_tick = tick
function tick(...)
    tdvr_boot_splash_tick(...)
    if not done and animPreloaded and t > requiredTime then
        done = true
        Menu()
    end
end
'''),
    "menu.lua": ("4c9f012b6acef91f9e07f9e0ceb0ad69cd1b4422a974e6a9732fffb9f0009d12", b'''
-- TDVR development bootstrap (MIT); isolated copy only.
local tdvr_boot_menu_tick = tick
function tick(...)
    tdvr_boot_menu_tick(...)
    if GetTime() > 1 and not GetBool("tdvr.dev.bootstrapped") then
        SetBool("tdvr.dev.bootstrapped", true)
        tryStartSandbox("mansion_sandbox")
    end
end
'''),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def atomic_write(path,data):
    descriptor, temporary = tempfile.mkstemp(prefix=".tdvr-bootstrap-",dir=path.parent)
    try:
        with os.fdopen(descriptor,"wb") as stream: stream.write(data)
        os.replace(temporary,path)
    finally:
        if os.path.exists(temporary): os.unlink(temporary)


def stage(game, remove=False):
    game = Path(game).resolve(strict=True)
    local = REPO / "local"
    if not game.is_relative_to(local) or game == local:
        raise ValueError("Bootstrap is restricted to this repository's local/ development copy")
    executable = (game / "teardown.exe").resolve(strict=True)
    if not executable.is_relative_to(game) or digest(executable.read_bytes()) != GAME_SHA:
        raise ValueError("Unknown development executable")
    backup = (local / "game-dev/backups/development-bootstrap").resolve()
    if not backup.is_relative_to(local):
        raise ValueError("Backup path leaves local/")
    changes = []
    for name, (expected, suffix) in SCRIPTS.items():
        path = (game / "data/ui" / name).resolve(strict=True)
        if not path.is_relative_to(game):
            raise ValueError("UI script path leaves development copy")
        current = path.read_bytes()
        original = current[:-len(suffix)] if current.endswith(suffix) else current
        if digest(original) != expected:
            raise ValueError("Unknown UI script: " + name)
        saved = backup / name
        if saved.exists() and (not saved.resolve().is_relative_to(local) or saved.read_bytes() != original):
            raise ValueError("Backup differs: " + name)
        desired = original if remove else original + suffix
        changes.append((path, saved, current, original, desired))
    # Validate every input before changing either script. Original game code and
    # backups remain ignored; only the small owned suffixes above are published.
    backup.mkdir(parents=True, exist_ok=True)
    for _, saved, _, original, _ in changes:
        if not saved.exists(): atomic_write(saved,original)
    written = []
    try:
        for path, _, current, _, desired in changes:
            if current != desired:
                atomic_write(path,desired); written.append((path,current,desired))
    except OSError:
        for path,current,desired in reversed(written):
            if path.read_bytes() == desired: atomic_write(path,current)
        raise
    return {"development_only":True,"removed":remove,"changed":len(written),"launched":False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game",type=Path,default=REPO / "local/game-dev/game")
    parser.add_argument("--remove",action="store_true")
    args = parser.parse_args()
    try:
        print(json.dumps(stage(args.game,args.remove))); return 0
    except (OSError,ValueError) as error:
        print(json.dumps({"error":str(error),"launched":False})); return 2


if __name__ == "__main__":
    raise SystemExit(main())
