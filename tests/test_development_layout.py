"""Verify development copies do not share game assets or mutate live settings."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/Stage-DevelopmentGame.py"
SPEC = importlib.util.spec_from_file_location("development_layout", SCRIPT)
LAYOUT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAYOUT)


class DevelopmentLayoutTests(unittest.TestCase):
    def test_copy_excludes_mods_and_creates_independent_profile(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "source"
            source.mkdir()
            for name in ("teardown.exe", "dxgi.dll", "dsound.dll", "dxcompiler.dll", "TeardownVR.ini", "TeardownVR.log"):
                (source / name).write_bytes(name.encode())
            (source / "data").mkdir()
            (source / "data/asset.vox").write_bytes(b"fixture asset")
            (source / "mods").mkdir()
            (source / "mods/main.lua").write_text("ignored")
            options = root / "options.xml"
            options.write_text('<options quality="3"/>')
            identity = {"parse_status": "ok", "compatibility": {"identity_status": "observed_only"},
                        "identity": {"sha256": LAYOUT.hashlib.sha256(b"teardown.exe").hexdigest()}}
            destination = root / "repo/local/development"
            with patch.object(LAYOUT, "REPO", root / "repo"), patch.object(LAYOUT.IDENTITY, "inspect_file", return_value=identity), \
                    patch.object(LAYOUT.shutil, "disk_usage", return_value=SimpleNamespace(free=8 * 1024**3)) as disk:
                dry = LAYOUT.stage(source, destination, options, dry_run=True)
                self.assertFalse(destination.exists())
                self.assertEqual(dry["file_count"], 3)
                disk.assert_not_called()
                # Reject before creating anything, including when only the
                # reserve fits. Tests must not depend on this PC's free space.
                disk.return_value = SimpleNamespace(free=1024**3)
                with self.assertRaisesRegex(ValueError, "free space"):
                    LAYOUT.stage(source, destination, options)
                self.assertFalse(destination.exists())
                disk.return_value = SimpleNamespace(free=8 * 1024**3)
                actual = LAYOUT.stage(source, destination, options)
                self.assertFalse(actual["launched"])
                self.assertTrue((destination / "game/dxcompiler.dll").exists())
                for excluded in ("dxgi.dll", "dsound.dll", "mods", "TeardownVR.ini", "TeardownVR.log"):
                    self.assertFalse((destination / "game" / excluded).exists())
                asset = destination / "game/data/asset.vox"
                asset.write_bytes(b"changed fixture")
                self.assertEqual((source / "data/asset.vox").read_bytes(), b"fixture asset")
                (destination / "profile/AppData/Local/Teardown/options.xml").write_text("changed fixture")
                self.assertEqual(options.read_text(), '<options quality="3"/>')
                with self.assertRaises(ValueError):
                    LAYOUT.stage(source, destination, options)
                with self.assertRaises(ValueError):
                    LAYOUT.stage(source, root / "outside", options)
                with patch.object(LAYOUT.IDENTITY, "inspect_file", return_value={"parse_status": "error"}):
                    with self.assertRaises(ValueError):
                        LAYOUT.stage(source, root / "repo/local/other", options)
