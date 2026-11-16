"""Headset-free checks against the actual Lua 5.1 companion script.

Install: python -m pip install -r tests/requirements.txt
Run: python -m unittest discover -s tests -v
"""
from pathlib import Path
import unittest
from lupa.lua51 import LuaRuntime

SOURCE = (Path(__file__).resolve().parents[1] / "mod/main.lua").read_text(encoding="utf-8")


class CompanionTests(unittest.TestCase):
    def runtime(self):
        lua = LuaRuntime()
        # Minimal engine math constructors used while loading the script.
        lua.execute('''
            function Vec(x,y,z) return {x or 0,y or 0,z or 0} end
            function Quat(x,y,z,w) return {x or 0,y or 0,z or 0,w or 1} end
            function Transform(p,q) return {pos=p or Vec(),rot=q or Quat()} end
        ''')
        lua.execute(SOURCE)
        return lua

    def test_bridge_initialization(self):
        lua = self.runtime()
        lua.execute('''
            init()
            assert(#TDVR_BRIDGE == 256)
            assert(TDVR_BRIDGE[4] == 3 and TDVR_BRIDGE[29] == 22)
            assert(TDVR_BRIDGE[7] == 256 and TDVR_BRIDGE[10] == -1)
            for k,v in pairs(TDVR_BRIDGE) do
                assert(type(k) == "number" and type(v) == "number")
            end
        ''')

    def test_active_update_with_optional_motion_blur_api(self):
        for available in (False, True):
            with self.subTest(available=available):
                lua = self.runtime()
                # Isolate engine-facing dependencies, execute the real updateActive.
                lua.execute('''
                    blurCalls, cameraCalls, hapticCalls = 0, 0, 0
                    function updateVehicleHeading() return false end
                    function rearmRightStick() end
                    function turnDelta() return 0 end
                    function RequestFirstPerson(v) assert(v == false) end
                    function updateCameraMode() end
                    function attachTick() end
                    function updateAimAndTool() return true end
                    function updateToolSwitch() end
                    function submitCamera() cameraCalls = cameraCalls + 1 end
                    function placeHandsAndLaser() end
                    function placeToolReplica() end
                    function updateHaptics() hapticCalls = hapticCalls + 1 end
                    for i=1,60 do
                        local name,value = debug.getupvalue(updateActive,i)
                        if not name then break end
                        if type(value) == "function" and type(_G[name]) == "function" then
                            debug.setupvalue(updateActive,i,_G[name])
                        end
                        if name == "nat" then value.recenter = 0 end
                        if name == "computeCamera" then
                            debug.setupvalue(updateActive,i,function()
                                return Transform(),Transform(),false
                            end)
                        end
                    end
                ''')
                if available:
                    lua.execute('function DisableMotionBlur() blurCalls = blurCalls + 1 end')
                lua.execute('''
                    assert(updateActive(1/60,0) == true)
                    assert(cameraCalls == 1 and hapticCalls == 1)
                ''')
                self.assertEqual(lua.globals().blurCalls, int(available))
