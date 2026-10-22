#!/usr/bin/env python3
"""Check real audio-route decisions, including Wine's client-level PID metadata."""

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("session_assist", Path(__file__).with_name("session_assist.py"))
assist = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assist)


class AudioEvents(unittest.TestCase):
    def test_snapshot_uses_one_call_and_rejects_missing_metadata(self):
        snapshot = {k: [] for k in ("sinks", "sources", "source_outputs", "clients", "sink_inputs")}
        with patch.object(assist, "run", return_value=json.dumps(snapshot)) as run:
            self.assertEqual(assist.pulse_snapshot(), snapshot)
            run.assert_called_once_with("pactl", "-f", "json", "list")
        with patch.object(assist, "run", return_value='{"sinks": []}'):
            with self.assertRaises(ValueError):
                assist.pulse_snapshot()

    def test_partial_event_is_retained_then_processed(self):
        buffer, dirty = assist.audio_events(b"", b"Event 'new' on sink-in")
        self.assertFalse(dirty)
        buffer, dirty = assist.audio_events(buffer, b"put #42\n")
        self.assertTrue(dirty)
        self.assertEqual(buffer, b"")

    def test_snapshot_client_churn_does_not_trigger_more_snapshots(self):
        buffer, dirty = assist.audio_events(b"", b"Event 'new' on client #4\nEvent 'remove' on client #4\n")
        self.assertFalse(dirty)
        self.assertEqual(buffer, b"")


class AudioRoutes(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="lg-tv-session-test-", dir="/tmp")
        self.addCleanup(temporary.cleanup)
        self.routes = assist.Routes(Path(temporary.name) / "routes.json")
        self.sinks = [{"index": 63, "name": "speakers"}, {"index": 99, "name": "sink-sunshine-stereo"}]
        self.clients = {"12": {"properties": {"pipewire.sec.pid": "42"}}}
        self.streams = [{"index": 7, "client": "12", "sink": 63, "properties": {}}]
        self.moves = []
        self.identity = "42:123:4020959543"

    def reconcile(self, target):
        self.routes.reconcile(target, self.streams, self.clients, self.sinks,
            identify=lambda pid: self.identity if pid == "42" else None,
            move=lambda index, name: self.moves.append((index, name)))

    def test_wine_client_pid_routes_then_restores(self):
        self.reconcile(self.sinks[1])
        self.assertEqual(self.moves, [("7", "sink-sunshine-stereo")])
        self.streams[0]["sink"] = 99
        self.reconcile(None)
        self.assertEqual(self.moves[-1], ("7", "speakers"))
        self.assertFalse(self.routes.path.exists())

    def test_no_capture_or_non_game_means_no_move(self):
        self.reconcile(None)
        self.identity = None
        self.reconcile(self.sinks[1])
        self.assertEqual(self.moves, [])

    def test_pulse_bridge_pid_does_not_hide_real_game_pid(self):
        self.clients["12"]["properties"] = {"pipewire.sec.pid": "1", "application.process.id": "42"}
        self.reconcile(self.sinks[1])
        self.assertEqual(self.moves, [("7", "sink-sunshine-stereo")])

    def test_asynchronous_route_is_not_sent_twice_and_can_retry(self):
        for now in (0, 0.05, 0.3):
            self.routes.reconcile(self.sinks[1], self.streams, self.clients, self.sinks,
                identify=lambda _: self.identity, move=lambda *args: self.moves.append(args), now=now)
        self.assertEqual(self.moves, [("7", "sink-sunshine-stereo")] * 2)

    def test_disconnect_clears_pending_move_before_fast_reconnect(self):
        for target, now in ((self.sinks[1], 0), (None, 0.02), (self.sinks[1], 0.04)):
            self.routes.reconcile(target, self.streams, self.clients, self.sinks,
                identify=lambda _: self.identity, move=lambda *args: self.moves.append(args), now=now)
        self.assertEqual(self.moves, [("7", "sink-sunshine-stereo")] * 2)

    def test_restart_keeps_original_route(self):
        self.reconcile(self.sinks[1])
        self.routes = assist.Routes(self.routes.path)
        self.streams[0]["sink"] = 99
        self.reconcile(self.sinks[1])
        self.assertEqual(len(self.moves), 1)
        self.assertTrue(self.routes.path.exists())
        self.reconcile(None)
        self.assertEqual(self.moves[-1], ("7", "speakers"))

    def test_pid_reuse_and_manual_move_are_not_restored(self):
        self.reconcile(self.sinks[1])
        self.identity = "42:999:4020959543"
        self.streams[0]["sink"] = 99
        self.reconcile(None)
        self.assertEqual(len(self.moves), 1)
        self.streams[0]["sink"] = 63
        self.reconcile(self.sinks[1])
        self.reconcile(None)
        self.assertEqual(len(self.moves), 2)

    def test_only_one_active_sunshine_monitor_enables_routing(self):
        sources = [{"index": 99, "name": "sink-sunshine-stereo.monitor"}]
        outputs = [{"source": 99, "corked": False, "properties": {"application.name": "sunshine"}}]
        self.assertEqual(assist.capture_sink(sources, outputs, self.sinks), self.sinks[1])
        outputs[0]["corked"] = True
        self.assertIsNone(assist.capture_sink(sources, outputs, self.sinks))
        outputs[0]["corked"] = False
        sources[0]["name"] = "speakers.monitor"
        self.assertIsNone(assist.capture_sink(sources, outputs, self.sinks))


class FocusHandoff(unittest.TestCase):
    def setUp(self):
        self.focus = assist.X11.__new__(assist.X11)
        self.sent = []
        self.focus.x = SimpleNamespace(XSelectInput=lambda *_: 0, XFlush=lambda *_: 0,
            XSendEvent=lambda *args: self.sent.append(args))
        self.focus.display, self.focus.root = 1, 1
        self.focus.seen, self.focus.pending = set(), set()
        self.focus.active_stream = False
        self.focus.atom = lambda name: {"_NET_WM_STATE_HIDDEN": 10, "_NET_ACTIVE_WINDOW": 11}[name]
        self.props = {(1, "_NET_CLIENT_LIST"): [2], (1, "_NET_ACTIVE_WINDOW"): [3],
                      (3, "WM_CLASS"): b"steamwebhelper\0steam\0"}
        self.focus.property = lambda window, name: self.props.get((window, name), [])
        context = patch.object(assist, "process_identity", return_value="12:42:123")
        context.start()
        self.addCleanup(context.stop)

    def ready(self):
        self.props[2, "WM_CLASS"] = b"steam_app_123\0steam_app_123\0"
        self.props[2, "_NET_WM_PID"] = [12]

    def test_waits_for_late_window_metadata_then_hands_off_once(self):
        self.focus.reconcile(True)
        self.assertEqual(len(self.sent), 0)
        self.ready()
        self.focus.reconcile(True)
        self.assertEqual(len(self.sent), 1)
        self.focus.reconcile(True)
        self.assertEqual(len(self.sent), 1)

    def test_inactive_stream_or_non_steam_focus_never_hands_off(self):
        self.ready()
        self.focus.reconcile(False)
        self.assertEqual(len(self.sent), 0)
        self.props[3, "WM_CLASS"] = b"browser\0browser\0"
        self.focus.reconcile(True)
        self.assertEqual(len(self.sent), 0)


if __name__ == "__main__":
    unittest.main()
