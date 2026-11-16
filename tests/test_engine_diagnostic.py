"""Synthetic PE fixtures only: no installed game, runtime, or headset required."""

import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/Inspect-Teardown.py"
SPEC = importlib.util.spec_from_file_location("engine_diagnostic", SCRIPT)
DIAG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DIAG)


def synthetic_pe(pe32=False, delayed=False):
    """Minimal independently laid-out PE with an import and RT_VERSION tree."""
    data = bytearray(0x1200)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    optional_size = 0xE0 if pe32 else 0xF0
    struct.pack_into("<HHIIIHH", data, 0x84, 0x14C if pe32 else 0x8664, 1,
                     1234, 0, 0, optional_size, 0x22)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x10B if pe32 else 0x20B)
    struct.pack_into("<I" if pe32 else "<Q", data, optional + (28 if pe32 else 24),
                     0x400000 if pe32 else 0x140000000)
    struct.pack_into("<I", data, optional + 60, 0x200)
    struct.pack_into("<I", data, optional + (92 if pe32 else 108), 16)
    dirs = optional + (96 if pe32 else 112)
    struct.pack_into("<II", data, dirs + 8, 0x1000, 40)
    struct.pack_into("<II", data, dirs + 16, 0x1300, 0x300)
    section = optional + optional_size
    data[section:section + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", data, section + 8, 0x1000, 0x1000, 0x1000, 0x200)
    struct.pack_into("<IIIII", data, 0x200, 0x1080, 0, 0, 0x1100, 0x1080)
    ptr = "I" if pe32 else "Q"
    ordinal = (1 << (31 if pe32 else 63)) | 7
    struct.pack_into("<" + ptr * 3, data, 0x280, 0x1180, ordinal, 0)
    data[0x300:0x30A] = b"D3D12.dll\0"
    symbol = b"\0\0D3D12CreateDevice\0"
    data[0x380:0x380 + len(symbol)] = symbol
    resource = 0x500
    for offset in (0, 0x20, 0x40):
        struct.pack_into("<IIHHHH", data, resource + offset, 0, 0, 0, 0, 0, 1)
    struct.pack_into("<II", data, resource + 16, 16, 0x80000020)
    struct.pack_into("<II", data, resource + 0x30, 1, 0x80000040)
    struct.pack_into("<II", data, resource + 0x50, 0x409, 0x60)
    struct.pack_into("<IIII", data, resource + 0x60, 0x1400, 92, 0, 0)
    version = 0x600
    struct.pack_into("<HHH", data, version, 92, 52, 0)
    key = "VS_VERSION_INFO\0".encode("utf-16le")
    data[version + 6:version + 6 + len(key)] = key
    struct.pack_into("<13I", data, version + 40, 0xFEEF04BD, 0x10000,
                     0x10000, 1, 0x10000, 1, 0, 0, 0, 1, 0, 0, 0)
    if delayed:
        struct.pack_into("<II", data, dirs + 13 * 8, 0x1700, 64)
        struct.pack_into("<8I", data, 0x900, 1, 0x1800, 0, 0x1080, 0x1080, 0, 0, 0)
        data[0xA00:0xA09] = b"dxgi.dll\0"
    return data


class EngineDiagnosticTests(unittest.TestCase):
    def assert_disabled(self, report):
        self.assertFalse(report["compatibility"]["vr_mode_allowed"])
        self.assertEqual(set(report["compatibility"]["capabilities"]), set(DIAG.CAPABILITIES))
        for value in report["compatibility"]["capabilities"].values():
            self.assertFalse(value["enabled"])
            self.assertTrue(value["reason"])

    def test_identity_versions_imports_and_delay_imports(self):
        for pe32 in (False, True):
            with self.subTest(pe32=pe32):
                data = synthetic_pe(pe32, delayed=True)
                report = DIAG.inspect_bytes(data, "teardown.exe", "2.0.0")
                self.assertEqual(report["parse_status"], "ok", report.get("error"))
                self.assertEqual(report["identity"]["sha256"], hashlib.sha256(data).hexdigest())
                self.assertEqual(report["identity"]["size_bytes"], len(data))
                self.assertEqual(report["identity"]["machine"], "0x014c" if pe32 else "0x8664")
                self.assertEqual(report["identity"]["pe_format"], "PE32" if pe32 else "PE32+")
                self.assertEqual(report["identity"]["file_versions"], ["1.0.0.1"])
                self.assertEqual(report["identity"]["product_versions"], ["1.0.0.1"])
                self.assertEqual(report["reported_game_version"], "2.0.0")
                self.assertEqual(report["imports"][0]["dll"], "d3d12.dll")
                self.assertEqual(report["imports"][0]["symbols"], ["#7", "D3D12CreateDevice"])
                self.assertEqual(report["delay_imports"][0]["dll"], "dxgi.dll")
                self.assertTrue(report["graphics_import_evidence"]["d3d12_dll_imported"])
                self.assertEqual(report["compatibility"]["identity_status"], "unknown")
                self.assert_disabled(report)

    def test_observed_identity_does_not_enable_any_capability(self):
        checksum, observation = next(iter(DIAG.OBSERVED_BUILDS.items()))
        identity = {**observation, "sha256": checksum}
        report = {"compatibility": DIAG.compatibility(identity)}
        self.assertEqual(report["compatibility"]["identity_status"], "observed_only")
        self.assert_disabled(report)
        for key, value in (("sha256", "0" * 64), ("machine", "0x014c"),
                           ("size_bytes", 1), ("file_versions", ["2.0.0.0"])):
            changed = {"compatibility": DIAG.compatibility({**identity, key: value})}
            self.assertEqual(changed["compatibility"]["identity_status"], "unknown")
            self.assert_disabled(changed)

    def test_no_imports_or_version_is_not_compatibility(self):
        data = synthetic_pe()
        data[0x98 + 112:0x98 + 112 + 16 * 8] = bytes(16 * 8)
        report = DIAG.inspect_bytes(data, reported_game_version="2.0.0")
        self.assertEqual(report["parse_status"], "ok")
        self.assertEqual(report["identity"]["file_versions"], [])
        self.assertEqual(report["imports"], [])
        self.assertFalse(report["graphics_import_evidence"]["d3d12_dll_imported"])
        self.assert_disabled(report)

    def test_malformed_inputs_fail_closed_without_struct_errors(self):
        cases = {"empty": b"", "truncated": synthetic_pe()[:100]}

        def changed(name, offset, fmt, *values):
            data = synthetic_pe()
            struct.pack_into(fmt, data, offset, *values)
            cases[name] = data

        changed("pe_offset", 0x3C, "<I", 0xFFFFFFF0)
        changed("bad_magic", 0x98, "<H", 0x999)
        changed("directory_count", 0x98 + 108, "<I", 17)
        changed("raw_past_end", 0x188 + 16, "<I", 0x2000)
        changed("virtual_only_import", 0x188 + 16, "<I", 0x10)
        changed("missing_import_terminator", 0x98 + 112 + 12, "<I", 20)
        changed("invalid_import_name", 0x200 + 12, "<I", 0xFFFFF000)
        changed("invalid_thunk", 0x280, "<Q", 0xFFFFFFFF)
        changed("invalid_ordinal", 0x280, "<Q", 0x8000000000010001)
        changed("resource_cycle", 0x530, "<II", 1, 0x80000020)
        changed("resource_bounds", 0x550, "<II", 0x409, 0x2FC)
        changed("resource_data", 0x560, "<I", 0xFFFF0000)
        changed("version_signature", 0x628, "<I", 0xDEADBEEF)
        changed("version_length", 0x600, "<H", 40)
        data = synthetic_pe()
        struct.pack_into("<H", data, 0x86, 2)
        data[0x1B0:0x1D8] = data[0x188:0x1B0]
        cases["overlapping_sections"] = data
        data = synthetic_pe(delayed=True)
        struct.pack_into("<I", data, 0x900, 0)
        cases["legacy_delay_imports"] = data
        for name, data in cases.items():
            with self.subTest(name=name):
                report = DIAG.inspect_bytes(data)
                self.assertEqual(report["parse_status"], "error", report)
                self.assertEqual(report["compatibility"]["identity_status"], "invalid_pe")
                self.assert_disabled(report)

    def test_cli_reads_only_and_refuses_game_or_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "game" / "teardown.exe"
            executable.parent.mkdir()
            data = synthetic_pe()
            executable.write_bytes(data)
            destination = root / "local" / "engine.json"
            with patch.object(DIAG, "REPO", root), contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(DIAG.main(["--exe", str(executable), "--output", str(destination)]), 0)
                report = json.loads(destination.read_text(encoding="utf-8"))
                self.assertEqual(report["parse_status"], "ok")
                self.assertNotIn(str(root), destination.read_text(encoding="utf-8"))
                self.assertEqual(DIAG.main(["--exe", str(executable), "--output", str(destination)]), 2)
                self.assertEqual(DIAG.main(["--exe", str(executable), "--output", str(executable)]), 2)
                self.assertEqual(DIAG.main(["--exe", str(root / "missing.exe")]), 2)
                self.assertIn('"error"', output.getvalue())
            self.assertEqual(executable.read_bytes(), data)


if __name__ == "__main__":
    unittest.main()
