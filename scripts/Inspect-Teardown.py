#!/usr/bin/env python3
"""Read-only PE identity and fail-closed Teardown compatibility diagnostic.

No game code, DLL, process, or Windows loader is executed. Standard library only.
SPDX-License-Identifier: MIT
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct


REPO = Path(__file__).resolve().parents[1]
MAX_FILE_SIZE = 512 * 1024 * 1024
MAX_IMPORTS = 4096
CAPABILITIES = (
    "independent_eye_projection",
    "stereo_single_simulation_snapshot",
    "head_and_controller_tracking",
    "engine_tool_6dof",
    "two_hand_physics",
    "vr_menu_hud_map",
)
# An observation, not an allowlist: no verified engine hooks exist yet.
OBSERVED_BUILDS = {
    "2733e37969fd6d45c5c2130872b0d4096147c304266b4b0adda01bd98caa2c4d": {
        "label": "local_observation_2026_11_16",
        "reported_game_version": "2.0.0",
        "size_bytes": 13655624,
        "machine": "0x8664",
        "pe_format": "PE32+",
        "file_versions": ["1.0.0.1"],
        "product_versions": ["1.0.0.1"],
    }
}


class PEError(ValueError):
    """The file cannot be safely interpreted by this diagnostic."""


class PEImage:
    def __init__(self, data):
        self.data = data
        self.sections = []
        if self.read(0, 2) != b"MZ":
            raise PEError("Missing DOS MZ signature")
        pe = self.unpack("<I", 0x3C)[0]
        if pe < 64 or self.read(pe, 4) != b"PE\0\0":
            raise PEError("Invalid PE signature or offset")
        (self.machine, count, self.timestamp, _, _, optional_size,
         self.characteristics) = self.unpack("<HHIIIHH", pe + 4)
        if not 1 <= count <= 96:
            raise PEError("Section count outside supported PE image bounds")
        optional = pe + 24
        self.read(optional, optional_size)
        magic = self.unpack("<H", optional)[0]
        if magic == 0x20B:
            self.pe_format, self.pointer_size = "PE32+", 8
            minimum, count_offset, dirs_offset = 112, 108, 112
            image_base_offset, image_base_format = 24, "<Q"
        elif magic == 0x10B:
            self.pe_format, self.pointer_size = "PE32", 4
            minimum, count_offset, dirs_offset = 96, 92, 96
            image_base_offset, image_base_format = 28, "<I"
        else:
            raise PEError("Unsupported optional header magic")
        if optional_size < minimum:
            raise PEError("Truncated optional header")
        self.image_base = self.unpack(image_base_format, optional + image_base_offset)[0]
        self.header_size = self.unpack("<I", optional + 60)[0]
        dir_count = self.unpack("<I", optional + count_offset)[0]
        if dir_count > 16 or dirs_offset + dir_count * 8 > optional_size:
            raise PEError("Invalid data directory count")
        self.directories = [self.unpack("<II", optional + dirs_offset + i * 8)
                            for i in range(dir_count)]
        table = optional + optional_size
        self.read(table, count * 40)
        if not table + count * 40 <= self.header_size <= len(data):
            raise PEError("Invalid SizeOfHeaders")
        for i in range(count):
            offset = table + i * 40
            virtual_size, rva, raw_size, raw_offset = self.unpack("<IIII", offset + 8)
            span = max(virtual_size, raw_size)
            if rva + span > 0x100000000 or (span and rva < self.header_size):
                raise PEError("Invalid section RVA range")
            if raw_size:
                if raw_offset < self.header_size:
                    raise PEError("Section overlaps PE headers")
                self.read(raw_offset, raw_size)
            for previous in self.sections:
                if span and previous[1] and rva < previous[0] + previous[1] and previous[0] < rva + span:
                    raise PEError("Overlapping virtual sections")
                if raw_size and previous[3] and raw_offset < previous[2] + previous[3] and previous[2] < raw_offset + raw_size:
                    raise PEError("Overlapping raw sections")
            self.sections.append((rva, span, raw_offset, raw_size))

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset > len(self.data) - size:
            raise PEError("File range is truncated or out of bounds")
        return self.data[offset:offset + size]

    def unpack(self, fmt, offset):
        return struct.unpack(fmt, self.read(offset, struct.calcsize(fmt)))

    def file_range(self, rva, size=1):
        if rva < 0 or size < 1 or rva + size > 0x100000000:
            raise PEError("Invalid RVA range")
        if rva < self.header_size and rva + size <= self.header_size:
            return rva, self.header_size - rva
        for start, span, raw_offset, raw_size in self.sections:
            if start <= rva < start + span:
                delta = rva - start
                if delta + size > raw_size:
                    raise PEError("RVA points to unbacked virtual section data")
                return raw_offset + delta, raw_size - delta
        raise PEError("RVA is not mapped to a file section")

    def at_rva(self, rva, size):
        return self.read(self.file_range(rva, size)[0], size)

    def directory(self, index):
        if index >= len(self.directories):
            return 0, 0
        rva, size = self.directories[index]
        if bool(rva) != bool(size):
            raise PEError("Inconsistent data directory RVA/size")
        if rva:
            self.file_range(rva, size)
        return rva, size

    def ascii_z(self, rva):
        offset, available = self.file_range(rva)
        value = self.read(offset, min(available, 4096))
        end = value.find(b"\0")
        if end < 1:
            raise PEError("Empty or unterminated import name")
        try:
            name = value[:end].decode("ascii")
        except UnicodeDecodeError as error:
            raise PEError("Non-ASCII import name") from error
        if any(ord(char) < 32 or ord(char) > 126 for char in name):
            raise PEError("Invalid control character in import name")
        return name

    def import_symbols(self, rva):
        if not rva:
            raise PEError("Missing import lookup table")
        flag = 1 << (self.pointer_size * 8 - 1)
        fmt = "<Q" if self.pointer_size == 8 else "<I"
        result = []
        for i in range(MAX_IMPORTS):
            value = struct.unpack(fmt, self.at_rva(rva + i * self.pointer_size, self.pointer_size))[0]
            if not value:
                return sorted(set(result))
            if value & flag:
                if value & ~(flag | 0xFFFF):
                    raise PEError("Invalid ordinal import")
                result.append("#" + str(value & 0xFFFF))
            else:
                self.at_rva(value, 2)  # Hint; the name immediately follows it.
                result.append(self.ascii_z(value + 2))
        raise PEError("Import lookup table exceeds safety bound or lacks terminator")

    def imports(self, delayed=False):
        rva, size = self.directory(13 if delayed else 1)
        if not rva:
            return []
        width = 32 if delayed else 20
        result = []
        for index in range(min(size // width, MAX_IMPORTS)):
            fields = struct.unpack("<" + "I" * (width // 4), self.at_rva(rva + index * width, width))
            if not any(fields):
                return sorted(result, key=lambda item: item["dll"])
            if delayed:
                attrs, name_rva, _, iat, lookup, _, _, timestamp = fields
                if attrs != 1:
                    raise PEError("Legacy VA-based delay imports are unsupported")
            else:
                lookup, timestamp, _, name_rva, iat = fields
            symbols = None if not lookup and timestamp else self.import_symbols(lookup or iat)
            result.append({"dll": self.ascii_z(name_rva).lower(), "symbols": symbols,
                           "symbols_status": "bound_iat_unavailable" if symbols is None else "parsed"})
        raise PEError("Import directory lacks a bounded null descriptor")

    def versions(self):
        base, size = self.directory(2)
        if not base:
            return []
        resources = self.at_rva(base, size)

        def resource_read(offset, length):
            if offset < 0 or length < 0 or offset > size - length:
                raise PEError("Resource entry exceeds directory bounds")
            return resources[offset:offset + length]

        def entries(offset):
            _, _, _, _, named, ids = struct.unpack("<IIHHHH", resource_read(offset, 16))
            count = named + ids
            if count > MAX_IMPORTS:
                raise PEError("Resource directory exceeds safety bound")
            return [struct.unpack("<II", resource_read(offset + 16 + i * 8, 8)) for i in range(count)]

        result = []
        visited = set()

        def descend(offset, depth):
            if offset in visited or depth > 2 or len(visited) >= MAX_IMPORTS:
                raise PEError("Cyclic, too deep, or excessive version resource tree")
            visited.add(offset)
            for name, child in entries(offset):
                if depth < 2:
                    if not child & 0x80000000:
                        raise PEError("Malformed version resource hierarchy")
                    descend(child & 0x7FFFFFFF, depth + 1)
                else:
                    if child & 0x80000000:
                        raise PEError("Malformed version resource leaf")
                    if len(result) >= MAX_IMPORTS:
                        raise PEError("Too many version resources")
                    data_rva, data_size, _, _ = struct.unpack("<IIII", resource_read(child, 16))
                    result.append(self.fixed_version(self.at_rva(data_rva, data_size), name))

        for name, child in entries(0):
            if name == 16:  # RT_VERSION
                if not child & 0x80000000:
                    raise PEError("Missing version resource type directory")
                descend(child & 0x7FFFFFFF, 1)
        return result

    @staticmethod
    def fixed_version(data, language):
        if len(data) < 6:
            raise PEError("Truncated VS_VERSIONINFO")
        length, value_length, value_type = struct.unpack_from("<HHH", data)
        key = "VS_VERSION_INFO\0".encode("utf-16le")
        value_offset = (6 + len(key) + 3) & ~3
        if (length > len(data) or length < value_offset + 52 or value_length != 52
                or value_type != 0 or data[6:6 + len(key)] != key):
            raise PEError("Invalid VS_VERSIONINFO root")
        fields = struct.unpack_from("<13I", data, value_offset)
        if fields[0] != 0xFEEF04BD or fields[1] != 0x10000:
            raise PEError("Invalid VS_FIXEDFILEINFO signature/version")

        def version(ms, ls):
            return ".".join(str(value) for value in (ms >> 16, ms & 65535, ls >> 16, ls & 65535))

        return {"language_id": language, "file_version": version(fields[2], fields[3]),
                "product_version": version(fields[4], fields[5])}


def compatibility(identity):
    observation = OBSERVED_BUILDS.get(identity.get("sha256"))
    matches = observation is not None and all(identity.get(field) == observation[field] for field in
        ("size_bytes", "machine", "pe_format", "file_versions", "product_versions"))
    reason = "No audited engine hook implementation or runtime validation exists."
    if not matches:
        reason = "Unknown executable identity. " + reason
    return {
        "identity_status": "observed_only" if matches else "unknown",
        "observation": observation["label"] if matches else None,
        "previously_reported_game_version": observation["reported_game_version"] if matches else None,
        "vr_mode_allowed": False,
        "capabilities": {name: {"enabled": False, "reason": reason} for name in CAPABILITIES},
    }


def inspect_bytes(data, filename="teardown.exe", reported_game_version=None):
    report = {"schema_version": 1, "diagnostic": "static_read_only",
              "filename": Path(filename).name, "reported_game_version": reported_game_version,
              "runtime_or_game_executed": False}
    identity = {"sha256": hashlib.sha256(data).hexdigest(), "size_bytes": len(data)}
    report["identity"] = identity
    try:
        if len(data) > MAX_FILE_SIZE:
            raise PEError("File exceeds diagnostic safety bound")
        pe = PEImage(data)
        versions = pe.versions()
        identity.update({"machine": f"0x{pe.machine:04x}", "pe_format": pe.pe_format,
                         "coff_timestamp": pe.timestamp,
                         "file_versions": sorted({v["file_version"] for v in versions}),
                         "product_versions": sorted({v["product_version"] for v in versions})})
        imports, delay_imports = pe.imports(), pe.imports(delayed=True)
        report.update({"parse_status": "ok", "version_resources": versions,
                       "imports": imports, "delay_imports": delay_imports})
        dlls = {item["dll"] for item in imports + delay_imports}
        report["graphics_import_evidence"] = {
            "d3d12_dll_imported": "d3d12.dll" in dlls,
            "dxgi_dll_imported": "dxgi.dll" in dlls,
            "opengl32_dll_imported": "opengl32.dll" in dlls,
            "limitation": "Static imports do not prove the active graphics API, hooks, or compatibility; dynamic loads may be absent.",
        }
        report["compatibility"] = compatibility(identity)
    except PEError as error:
        report.update({"parse_status": "error", "error": str(error),
                       "compatibility": compatibility({})})
        report["compatibility"]["identity_status"] = "invalid_pe"
    return report


def inspect_file(path, reported_game_version=None):
    path = Path(path)
    # Bound memory allocation even if the input is malformed or grows during read.
    with path.open("rb") as stream:
        data = stream.read(MAX_FILE_SIZE + 1)
    if len(data) > MAX_FILE_SIZE:
        raise PEError("File exceeds diagnostic safety bound")
    return inspect_bytes(data, path.name, reported_game_version)


def output_path(value):
    path = Path(value).resolve()
    local = (REPO / "local").resolve()
    if local != REPO.resolve() / "local" or path == local or not path.is_relative_to(local):
        raise PEError("Report output must be inside this repository's ignored local/ directory")
    return path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path, help="Read-only path to the game executable")
    parser.add_argument("--reported-game-version", help="User-reported version, never treated as verified PE identity")
    parser.add_argument("--output", help="Optional JSON destination beneath the repository's ignored local/ directory")
    args = parser.parse_args(argv)
    try:
        destination = output_path(args.output) if args.output else None
        if destination == args.exe.resolve():
            raise PEError("Output cannot replace the inspected executable")
        report = inspect_file(args.exe, args.reported_game_version)
        serialized = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
        if destination:
            destination.parent.mkdir(parents=True, exist_ok=True)
            # Refuse overwrite: existing local reports may be valuable evidence.
            with destination.open("x", encoding="utf-8") as stream:
                stream.write(serialized)
        else:
            print(serialized, end="")
        return 0 if report["parse_status"] == "ok" else 2
    except (OSError, PEError) as error:
        message = error.strerror if isinstance(error, OSError) else str(error)
        print(json.dumps({"schema_version": 1, "parse_status": "error", "error": message,
                          "compatibility": compatibility({})}, indent=2))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
