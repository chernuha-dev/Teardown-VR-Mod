"""Fail-closed binary experiment: synthetic fixtures, no DLL execution or headset."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from test_engine_diagnostic import DIAG, synthetic_pe

SPEC = importlib.util.spec_from_file_location(
    "tool_patch", Path(__file__).resolve().parents[1] / "scripts/Patch-V110-ToolDetection.py")
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)


class ToolPatchTests(unittest.TestCase):
    def test_identity_and_instruction_gates_preserve_source(self):
        data = synthetic_pe()
        data[0x400:0x402] = bytes((0x8B, 0x49))
        original = bytes(data)
        digest = hashlib.sha256(original).hexdigest()
        changes = ((0x1200, 0x8B, 0x8D), (0x1201, 0x49, 0x79))
        with patch.object(TOOL, "DLL_SHA", digest), patch.object(TOOL, "CHANGES", changes):
            result = TOOL.prepare(data, DIAG.PEImage(data))
            self.assertEqual(bytes(data), original)
            self.assertEqual([i for i, (a, b) in enumerate(zip(original, result)) if a != b],
                             [0x400, 0x401])
            self.assertEqual(result[0x400:0x402], bytes((0x8D, 0x79)))
            with self.assertRaisesRegex(ValueError, "Unknown source"):
                TOOL.prepare(result, DIAG.PEImage(result))
            with patch.object(TOOL, "CHANGES", ((0x1200, 0x90, 0x8D),)):
                with self.assertRaisesRegex(ValueError, "Unexpected"):
                    TOOL.prepare(data, DIAG.PEImage(data))
            with patch.object(TOOL, "CHANGES", ((0xFFFFFFFF, 0, 1),)):
                with self.assertRaises(ValueError):
                    TOOL.prepare(data, DIAG.PEImage(data))

    def test_output_scope_and_existing_file_are_rejected_before_input_reads(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            with patch.object(TOOL, "REPO", root):
                for output in (root / "dsound.dll", root / "local/../dsound.dll",
                               root / "local/dxgi.dll"):
                    with self.assertRaisesRegex(ValueError, "Output must"):
                        TOOL.create("missing", "missing", "missing", output)
                existing = root / "local/dsound.dll"
                existing.parent.mkdir()
                existing.write_bytes(b"preserve")
                with self.assertRaisesRegex(ValueError, "already exists"):
                    TOOL.create("missing", "missing", "missing", existing)
                self.assertEqual(existing.read_bytes(), b"preserve")

    def test_game_and_snapshot_identity_are_required(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            game = root / "game.exe"
            game.write_bytes(b"unknown game")
            output = root / "local/dsound.dll"
            with patch.object(TOOL, "REPO", root):
                with self.assertRaisesRegex(ValueError, "Unknown game"):
                    TOOL.create("missing", game, "missing", output)
                with patch.object(TOOL, "GAME_SHA", hashlib.sha256(game.read_bytes()).hexdigest()):
                    snapshot = root / "snapshot.bin"
                    snapshot.write_bytes(b"unknown code")
                    with self.assertRaisesRegex(ValueError, "Unknown runtime"):
                        TOOL.create("missing", game, snapshot, output)
                self.assertFalse(output.exists())
