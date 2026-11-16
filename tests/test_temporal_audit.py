"""Temporal audit helpers operate on bytes only; never need a game or XR."""
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/Inspect-TemporalCompatibility.py"
SPEC = importlib.util.spec_from_file_location("temporal_audit", SCRIPT)
AUDIT = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(AUDIT)


class TemporalAuditTests(unittest.TestCase):
    def test_snapshot_and_reference_identities_are_checked_before_analysis(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory)/name for name in ("game.exe","snapshot.bin","reference.dll")]
            for path in paths: path.write_bytes(path.name.encode())
            with patch.object(AUDIT,"GAME_SHA",AUDIT.hashlib.sha256(paths[0].read_bytes()).hexdigest()):
                with self.assertRaisesRegex(ValueError,"Unknown snapshot identity"):
                    AUDIT.inspect(*paths)
                with patch.object(AUDIT,"SNAPSHOT_SHA",AUDIT.hashlib.sha256(paths[1].read_bytes()).hexdigest()):
                    with self.assertRaisesRegex(ValueError,"Unknown reference identity"):
                        AUDIT.inspect(*paths)

    def test_wildcards_literal_regex_bytes_and_overlapping_ambiguity(self):
        self.assertEqual(AUDIT.matches(b"\x5b\x0a\x5d", "5b ?? 5d", 0x1000), [0x1000])
        self.assertEqual(AUDIT.matches(b"\xaa\xaa\xaa", "aa aa"), [0,1])
        for data in (b"",b"\xaa\xaa\xaa"):
            with self.assertRaises(ValueError): AUDIT.unique(data,"aa aa")
        for pattern in ("", "?", "GG", "0", "00 | ff"):
            with self.assertRaises(ValueError): AUDIT.matches(b"x",pattern)

    def test_signed_relative_calls_and_bounds(self):
        for call, target in ((10,1),(1,12)):
            blob = bytearray(20); blob[call] = 0xE8
            struct.pack_into("<i",blob,call+1,target-call-5)
            self.assertEqual(AUDIT.relative_call(blob,0x1000+call),0x1000+target)
        for blob, address in ((b"",0x1000),(b"\xe8\0\0\0",0x1000),(b"\x90"*20,0x1000),
                              (b"\xe8\xff\xff\xff\x7f",0x1000),(b"\xe8\0\0\0\0",0xfff)):
            with self.assertRaises(ValueError): AUDIT.relative_call(blob,address)

    def test_missing_code_does_not_publish_partial_audit(self):
        with self.assertRaisesRegex(ValueError,"world-render code range"):
            AUDIT.audit_code(b"\0"*16)
        with self.assertRaisesRegex(ValueError,"found 0"):
            AUDIT.audit_code(b"\0"*(AUDIT.WORLD_RVA-AUDIT.TEXT_RVA+0x600))

    def test_cli_rejects_unknown_identity_without_reading_other_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)/"unknown.exe"; source.write_bytes(b"fixture")
            before = source.read_bytes()
            result = subprocess.run([sys.executable,str(SCRIPT),"--game",str(source),
                "--snapshot",str(Path(directory)/"missing.snapshot"),"--reference",str(Path(directory)/"missing.dll")],
                text=True,capture_output=True,timeout=10)
            self.assertEqual(result.returncode,2)
            report = json.loads(result.stdout)
            self.assertIn("Unknown game identity",report["error"])
            self.assertFalse(report["executed"]); self.assertFalse(report["deployed"])
            self.assertFalse(report["stereo_replay_enabled"])
            self.assertEqual(source.read_bytes(),before)
            self.assertEqual([p.name for p in Path(directory).iterdir()],["unknown.exe"])
