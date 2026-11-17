import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from lupa.lua51 import LuaRuntime

spec = importlib.util.spec_from_file_location("development_bootstrap",Path(__file__).resolve().parents[1]/"scripts/Stage-DevelopmentBootstrap.py")
bootstrap = importlib.util.module_from_spec(spec); spec.loader.exec_module(bootstrap)


class BootstrapTests(unittest.TestCase):
    def test_lua_bootstrap_waits_and_starts_once_without_input(self):
        splash = LuaRuntime()
        splash.execute('''
            ticks=0; menus=0; done=false; animPreloaded=false; t=10; requiredTime=4.5
            function tick(dt) ticks=ticks+dt end
            function Menu() menus=menus+1 end
        ''')
        splash.execute(bootstrap.SCRIPTS["splash.lua"][1].decode())
        splash.execute('''
            tick(1); assert(menus==0)
            animPreloaded=true; t=4; tick(2); assert(menus==0)
            t=5; tick(3); tick(4); assert(menus==1 and done and ticks==10)
        ''')
        menu = LuaRuntime()
        menu.execute('''
            now=0; ticks=0; levels=0; registry={}
            function tick(dt) ticks=ticks+dt end
            function GetTime() return now end
            function GetBool(key) return registry[key] or false end
            function SetBool(key,v) assert(key=="tdvr.dev.bootstrapped"); registry[key]=v end
            function tryStartSandbox(id) assert(id=="mansion_sandbox"); levels=levels+1 end
        ''')
        menu.execute(bootstrap.SCRIPTS["menu.lua"][1].decode())
        menu.execute('tick(1); assert(levels==0); now=2; tick(2); tick(3); assert(levels==1 and ticks==6)')
        # Loading a fresh menu callback with the same game registry must not
        # return the user to the sandbox again when they leave the level.
        menu.execute('function tick(dt) ticks=ticks+dt end')
        menu.execute(bootstrap.SCRIPTS["menu.lua"][1].decode())
        menu.execute('tick(1); assert(levels==1 and ticks==7)')

    def test_backup_idempotence_and_exact_removal(self):
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary); game = repo/"local/game-dev/game"
            (game/"data/ui").mkdir(parents=True); (game/"teardown.exe").write_bytes(b"fixture")
            originals = {name:b"-- fixture " + name.encode() for name in bootstrap.SCRIPTS}
            scripts = {name:(hashlib.sha256(data).hexdigest(),bootstrap.SCRIPTS[name][1]) for name,data in originals.items()}
            for name,data in originals.items(): (game/"data/ui"/name).write_bytes(data)
            with patch.object(bootstrap,"REPO",repo), patch.object(bootstrap,"GAME_SHA",hashlib.sha256(b"fixture").hexdigest()), patch.object(bootstrap,"SCRIPTS",scripts):
                self.assertEqual(bootstrap.stage(game)["changed"],2)
                self.assertEqual(bootstrap.stage(game)["changed"],0)
                for name,data in originals.items():
                    self.assertEqual((repo/"local/game-dev/backups/development-bootstrap"/name).read_bytes(),data)
                self.assertEqual(bootstrap.stage(game,True)["changed"],2)
                for name,data in originals.items(): self.assertEqual((game/"data/ui"/name).read_bytes(),data)
                write = bootstrap.atomic_write
                def fail_second(path,data):
                    if path.parent.name == "ui" and path.name == "menu.lua": raise OSError("fixture write failure")
                    write(path,data)
                with patch.object(bootstrap,"atomic_write",side_effect=fail_second):
                    with self.assertRaises(OSError): bootstrap.stage(game)
                for name,data in originals.items(): self.assertEqual((game/"data/ui"/name).read_bytes(),data)
                # Unknown second file must prevent changing the valid first file.
                (game/"data/ui/menu.lua").write_bytes(b"user edit")
                with self.assertRaisesRegex(ValueError,"Unknown UI"):
                    bootstrap.stage(game)
                self.assertEqual((game/"data/ui/splash.lua").read_bytes(),originals["splash.lua"])
                outside = repo/"live"; outside.mkdir()
                with self.assertRaisesRegex(ValueError,"restricted"):
                    bootstrap.stage(outside)
                (game/"teardown.exe").write_bytes(b"different game")
                with self.assertRaisesRegex(ValueError,"Unknown development executable"):
                    bootstrap.stage(game)
