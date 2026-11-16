#!/usr/bin/env python3
"""Copy a game development layout under ignored local/, without launching it.

No hardlinks, junctions, shared writable assets, or writes to the source install.
SPDX-License-Identifier: MIT
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import stat

REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("engine_identity", REPO / "scripts/Inspect-Teardown.py")
IDENTITY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(IDENTITY)

# Only ordinary game dependencies/assets. Explicitly exclude both legacy VR DLLs,
# mods, logs, crash reports, and VR/user INIs. No DLLs are modified or redistributed.
DIRECTORIES = ("data", "D3D12", "steaminput")
EXCLUDED_DLLS = {"dxgi.dll", "dsound.dll"}


def is_plain(path):
    attributes = path.lstat().st_file_attributes
    return not path.is_symlink() and not attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT


def plan(source, destination, profile_options=None):
    source = Path(source).resolve(strict=True)
    destination = Path(destination).resolve()
    local = (REPO / "local").resolve()
    if destination == local or not destination.is_relative_to(local):
        raise ValueError("Development destination must be beneath this repository's ignored local/")
    if destination == source or destination.is_relative_to(source) or source.is_relative_to(destination):
        raise ValueError("Development destination and source must not overlap")
    if destination.exists():
        raise ValueError("Refusing to merge into or overwrite an existing development directory")
    report = IDENTITY.inspect_file(source / "teardown.exe")
    if report["parse_status"] != "ok" or report["compatibility"]["identity_status"] != "observed_only":
        raise ValueError("Executable identity is not the audited observation; inspect it first")
    files = [source / "teardown.exe"]
    files.extend(sorted(p for p in source.glob("*.dll") if p.name.lower() not in EXCLUDED_DLLS))
    # Preserve an existing Steam app id when present. No changes to licensing/DRM.
    appid = source / "steam_appid.txt"
    if appid.exists():
        files.append(appid)
    # Preserve an existing dependency's configuration verbatim when present. Do
    # not invent/change licensing settings or try to work around startup checks.
    dependency_config = source / "OnlineFix.ini"
    if dependency_config.exists():
        files.append(dependency_config)
    for name in DIRECTORIES:
        directory = source / name
        if not directory.exists():
            if name == "data":
                raise ValueError("Missing game data directory")
            continue
        for entry in directory.rglob("*"):
            if not is_plain(entry):
                raise ValueError("Shared/reparse assets are unsupported in an isolated development copy")
            if entry.is_file():
                files.append(entry)
    for path in files:
        if not is_plain(path) or not path.is_file():
            raise ValueError("Source must contain only ordinary files")
    options = Path(profile_options).resolve(strict=True) if profile_options else None
    if options and (not is_plain(options) or options.name.lower() != "options.xml"):
        raise ValueError("Only an ordinary options.xml can initialize the isolated profile")
    return source, destination, files, report, options


def stage(source, destination, profile_options=None, dry_run=False):
    source, destination, files, identity, options = plan(source, destination, profile_options)
    size = sum(p.stat().st_size for p in files) + (options.stat().st_size if options else 0)
    summary = {"schema_version": 1, "operation": "development_copy_only", "launched": False,
               "file_count": len(files), "bytes": size, "executable_sha256": identity["identity"]["sha256"],
               "legacy_vr_dlls_copied": False, "shared_assets": False,
               "profile_options_copied": bool(options), "dry_run": dry_run}
    if dry_run:
        return summary
    existing = destination.parent
    while not existing.exists():
        existing = existing.parent
    if shutil.disk_usage(existing).free < size + 1024 ** 3:
        raise ValueError("Not enough free space for a separate copy plus 1 GiB reserve")
    destination.mkdir(parents=True, exist_ok=False)
    for path in files:
        target = destination / "game" / path.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    copied = destination / "game/teardown.exe"
    if hashlib.sha256(copied.read_bytes()).hexdigest() != summary["executable_sha256"]:
        raise ValueError("Copied executable checksum mismatch; do not launch this layout")
    for suffix in ("AppData/Local/Teardown", "AppData/Roaming", "Documents"):
        (destination / "profile" / suffix).mkdir(parents=True, exist_ok=True)
    if options:
        shutil.copy2(options, destination / "profile/AppData/Local/Teardown/options.xml")
    (destination / "development-copy.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--destination", required=True, type=Path)
    parser.add_argument("--profile-options", type=Path, help="Copy existing options.xml to keep its quality settings")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    try:
        print(json.dumps(stage(args.source, args.destination, args.profile_options, args.dry_run), indent=2))
        return 0
    except (OSError, ValueError) as error:
        print(json.dumps({"error": str(error), "launched": False}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
