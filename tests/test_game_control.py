"""MCP routing is tested without a game, runtime, UI or headset."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("game_control", Path(__file__).resolve().parents[1] / "scripts/Game-Control.py")
CONTROL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONTROL)


class GameControlTests(unittest.TestCase):
    def test_discovery_does_not_contact_game(self):
        with patch.object(CONTROL, "exchange", side_effect=AssertionError("unexpected game access")):
            response = CONTROL.rpc({"id": 1, "method": "initialize", "params": {"protocolVersion": "2024-11-05"}})
            self.assertEqual(response["result"]["protocolVersion"], "2024-11-05")
            tools = CONTROL.rpc({"id": 2, "method": "tools/list"})["result"]["tools"]
            self.assertEqual({t["name"] for t in tools}, set(CONTROL.COMMANDS))
            self.assertTrue(next(t for t in tools if t["name"] == "tdvr_status")["annotations"]["readOnlyHint"])
            self.assertFalse(next(t for t in tools if t["name"] == "tdvr_request_exit")["annotations"]["readOnlyHint"])
            self.assertIsNone(CONTROL.rpc({"method": "notifications/initialized"}))

    def test_command_routes_and_preserves_request_id(self):
        for name, command in CONTROL.COMMANDS.items():
            with patch.object(CONTROL, "exchange", return_value={"ok": True}) as send:
                result = CONTROL.rpc({"id": "abc", "method": "tools/call", "params": {"name": name, "arguments": {"pid": 123}}})
                send.assert_called_once_with(123, command)
                self.assertEqual(result["id"], "abc")
                self.assertFalse(result["result"].get("isError", False))

    def test_unknown_commands_and_transport_errors_are_explicit(self):
        self.assertEqual(CONTROL.rpc({"id": 1, "method": "nope"})["error"]["code"], -32601)
        self.assertEqual(CONTROL.rpc({"id": 1, "method": "tools/call", "params": {"name": "shell"}})["error"]["code"], -32602)
        with patch.object(CONTROL, "exchange", side_effect=TimeoutError("no response")):
            result = CONTROL.rpc({"id": 1, "method": "tools/call", "params": {"name": "tdvr_status", "arguments": {"pid": 123}}})
            self.assertTrue(result["result"]["isError"])

    def test_invalid_target_rejected_before_windows_access(self):
        for pid, command in ((0, "status"), (-1, "status"), (True, "status"), (123, "shell")):
            with self.assertRaises(ValueError):
                CONTROL.exchange(pid, command)
