#!/usr/bin/env python3
"""Route Steam game audio and hand off Xwayland focus during Sunshine sessions."""

import argparse
import ctypes as C
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import time


ROOT = Path(__file__).resolve().parents[2]
STATE = Path(os.environ.get("LG_TV_SESSION_STATE_DIR", str(ROOT / "host/runtime/session-assist"))) / "routes.json"
UNIT = Path.home() / ".config/systemd/user/lg-tv-session-assist.service"
SERVICE = f"""# Managed by LG_TV/host/tools/session_assist.py
[Unit]
Description=LG TV Steam focus and audio handoff
After=pipewire-pulse.service graphical-session.target
PartOf=graphical-session.target

[Service]
ExecStart=/usr/bin/python3 -B {Path(__file__).resolve()} run
Restart=on-failure
RestartSec=3
TimeoutStopSec=10

[Install]
WantedBy=default.target
"""


def run(*args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=4)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f"{args[0]} failed")
    return result.stdout


def pulse(kind):
    return json.loads(run("pactl", "-f", "json", "list", kind))


def pulse_snapshot():
    """Read one server snapshot instead of spawning five clients serially."""
    snapshot = json.loads(run("pactl", "-f", "json", "list"))
    required = ("sinks", "sources", "source_outputs", "clients", "sink_inputs")
    if not isinstance(snapshot, dict) or any(not isinstance(snapshot.get(k), list) for k in required):
        raise ValueError("Incomplete Pulse snapshot; routes left unchanged")
    return snapshot


def audio_events(buffer, data):
    """Preserve partial lines; ignore clients created by our own inspection."""
    lines = (buffer + data).split(b"\n")
    dirty = any(re.search(rb" on (?:sink-input|source-output|sink|source|server) #\d+", line)
                for line in lines[:-1])
    return lines[-1], dirty


def process_identity(pid):
    """Read only Steam identifiers, plus a start time to detect PID reuse."""
    try:
        proc = Path("/proc") / str(int(pid))
        if proc.stat().st_uid != os.getuid():
            return None
        environment = dict(item.split(b"=", 1) for item in
                           (proc / "environ").read_bytes().split(b"\0") if b"=" in item)
        app = environment.get(b"SteamAppId", b"").decode("ascii", errors="ignore")
        if not app.isdecimal() or int(app) == 0:
            return None
        start = (proc / "stat").read_text().rsplit(")", 1)[1].split()[19]
        return f"{int(pid)}:{start}:{app}"
    except (OSError, ValueError, IndexError):
        return None


def stream_identity(stream, clients, identify=process_identity):
    props = clients.get(str(stream.get("client")), {}).get("properties", {})
    # Pulse clients expose the bridge daemon as sec.pid, not the application.
    pid = props.get("application.process.id") or \
        stream.get("properties", {}).get("application.process.id") or props.get("pipewire.sec.pid")
    return identify(pid) if pid else None


def capture_sink(sources, outputs, sinks):
    """Follow an actual active Sunshine capture, not just the default output."""
    names = {str(item["index"]): item["name"] for item in sources}
    sinks_by_name = {item["name"]: item for item in sinks}
    targets = set()
    for stream in outputs:
        props = stream.get("properties", {})
        if props.get("application.name", "").lower() != "sunshine" or stream.get("corked"):
            continue
        source = names.get(str(stream.get("source")), "")
        if source.endswith(".monitor"):
            name = source.removesuffix(".monitor")
            if name.startswith("sink-sunshine-") and name in sinks_by_name:
                targets.add(name)
    return sinks_by_name[next(iter(targets))] if len(targets) == 1 else None


class Routes:
    def __init__(self, path=STATE):
        self.path = path
        self.moving = {}
        try:
            self.saved = json.loads(path.read_text())
        except (OSError, ValueError):
            self.saved = {}

    def persist(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_suffix(".tmp")
        temporary.write_text(json.dumps(self.saved) + "\n")
        temporary.replace(self.path)

    def reconcile(self, target, streams, clients, sinks, identify=process_identity, move=None, now=None):
        now = time.monotonic() if now is None else now
        move = move or (lambda index, name: run("pactl", "move-sink-input", str(index), name))
        before = {key: dict(value) for key, value in self.saved.items()}
        by_index = {str(item["index"]): item["name"] for item in sinks}
        live = {str(item["index"]): item for item in streams}
        self.moving = {key: value for key, value in self.moving.items() if key in live and value[2] > now}
        if target is None:
            self.moving.clear()
        for index, saved in list(self.saved.items()):
            stream = live.get(index)
            if stream is None or stream_identity(stream, clients, identify) != saved["identity"]:
                del self.saved[index]
                continue
            if target is None:
                # Restore only a route this helper still owns. Respect later user moves.
                current = by_index.get(str(stream["sink"]))
                if current == saved["routed_to"] and saved["original"] in by_index.values():
                    move(index, saved["original"])
                    print(f"Restored audio stream {index} to {saved['original']}", flush=True)
                del self.saved[index]
        if target is not None:
            for stream in streams:
                identity = stream_identity(stream, clients, identify)
                if identity is None:
                    continue
                index = str(stream["index"])
                if str(stream["sink"]) == str(target["index"]):
                    self.moving.pop(index, None)
                    continue
                original = by_index.get(str(stream["sink"]))
                if original is None:
                    continue
                pending = self.moving.get(index)
                if pending and pending[:2] == (identity, target["name"]):
                    continue  # PipeWire applies moves asynchronously; avoid duplicate requests.
                if index not in self.saved:
                    self.saved[index] = {"identity": identity, "original": original,
                                         "routed_to": target["name"]}
                    self.persist()  # Save recovery information before changing the route.
                move(index, target["name"])
                self.moving[index] = (identity, target["name"], now + 0.2)
                self.saved[index]["routed_to"] = target["name"]
                print(f"Routed Steam audio stream {index} to {target['name']}", flush=True)
        if self.saved and self.saved != before:
            self.persist()
        elif not self.saved and self.path.exists():
            self.path.unlink()


class X11:
    """Use window-manager requests; never write focus rules or inject game input."""
    def __init__(self):
        self.x = C.CDLL("libX11.so.6")
        signatures = {
            "XOpenDisplay": (C.c_void_p, [C.c_char_p]),
            "XDefaultRootWindow": (C.c_ulong, [C.c_void_p]),
            "XConnectionNumber": (C.c_int, [C.c_void_p]),
            "XInternAtom": (C.c_ulong, [C.c_void_p, C.c_char_p, C.c_int]),
            "XGetWindowProperty": (C.c_int, [C.c_void_p, C.c_ulong, C.c_ulong,
                C.c_long, C.c_long, C.c_int, C.c_ulong, C.POINTER(C.c_ulong),
                C.POINTER(C.c_int), C.POINTER(C.c_ulong), C.POINTER(C.c_ulong),
                C.POINTER(C.POINTER(C.c_ubyte))]),
            "XFree": (C.c_int, [C.c_void_p]),
            "XSelectInput": (C.c_int, [C.c_void_p, C.c_ulong, C.c_long]),
            "XPending": (C.c_int, [C.c_void_p]),
            "XNextEvent": (C.c_int, [C.c_void_p, C.c_void_p]),
            "XSendEvent": (C.c_int, [C.c_void_p, C.c_ulong, C.c_int, C.c_long, C.c_void_p]),
            "XFlush": (C.c_int, [C.c_void_p]),
            "XCloseDisplay": (C.c_int, [C.c_void_p]),
        }
        for name, (result, args) in signatures.items():
            method = getattr(self.x, name)
            method.restype, method.argtypes = result, args
        self.error_handler = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_void_p)(lambda *_: 0)
        self.x.XSetErrorHandler.argtypes = [C.c_void_p]
        self.x.XSetErrorHandler(self.error_handler)
        self.display = self.x.XOpenDisplay(None)
        if not self.display:
            raise RuntimeError("Xwayland display unavailable")
        self.root = self.x.XDefaultRootWindow(self.display)
        self.fd = self.x.XConnectionNumber(self.display)
        self.atoms = {}
        self.x.XSelectInput(self.display, self.root, (1 << 22) | (1 << 19))
        self.x.XFlush(self.display)
        self.seen = set()
        self.pending = set()
        self.active_stream = False

    def atom(self, name):
        if name not in self.atoms:
            self.atoms[name] = self.x.XInternAtom(self.display, name.encode(), 0)
        return self.atoms[name]

    def property(self, window, name):
        typ, fmt, count, after = C.c_ulong(), C.c_int(), C.c_ulong(), C.c_ulong()
        ptr = C.POINTER(C.c_ubyte)()
        result = self.x.XGetWindowProperty(self.display, window, self.atom(name), 0, 4096,
            0, 0, C.byref(typ), C.byref(fmt), C.byref(count), C.byref(after), C.byref(ptr))
        try:
            if result or not ptr:
                return []
            if fmt.value == 32:
                return list(C.cast(ptr, C.POINTER(C.c_ulong))[:count.value])
            return C.string_at(ptr, count.value) if fmt.value == 8 else []
        finally:
            if ptr:
                self.x.XFree(ptr)

    def reconcile(self, streaming):
        windows = self.property(self.root, "_NET_CLIENT_LIST")
        active = self.property(self.root, "_NET_ACTIVE_WINDOW")
        current = active[0] if active else 0
        active_class = self.property(current, "WM_CLASS") if current else b""
        new_windows = set(windows) - self.seen if self.active_stream else set(windows)
        for window in new_windows:
            self.x.XSelectInput(self.display, window, 1 << 22)
        self.x.XFlush(self.display)
        self.pending = (self.pending | new_windows) & set(windows)
        self.seen, self.active_stream = set(windows), streaming
        if not streaming or (current and active_class not in
                              (b"steamwebhelper\0steam\0", b"Steam\0Steam\0")):
            self.pending.clear()
            return
        for window in reversed(windows):
            if window not in self.pending:
                continue
            cls = self.property(window, "WM_CLASS")
            if not isinstance(cls, bytes):
                continue
            match = re.fullmatch(rb"steam_app_([0-9]+)\0steam_app_\1\0", cls)
            if not match or self.atom("_NET_WM_STATE_HIDDEN") in self.property(window, "_NET_WM_STATE"):
                continue
            pid = self.property(window, "_NET_WM_PID")
            identity = process_identity(pid[0]) if pid else None
            if identity is None or identity.rsplit(":", 1)[1] != match[1].decode():
                continue
            # XClientMessageEvent is 12 native longs on LP64; XEvent is24.
            class Data(C.Union):
                _fields_ = [("l", C.c_long * 5), ("b", C.c_char * 20)]
            class Message(C.Structure):
                _fields_ = [("type", C.c_int), ("serial", C.c_ulong), ("send", C.c_int),
                    ("display", C.c_void_p), ("window", C.c_ulong), ("atom", C.c_ulong),
                    ("format", C.c_int), ("data", Data)]
            event = (C.c_long * 24)()
            message = C.cast(event, C.POINTER(Message)).contents
            message.type, message.window = 33, window
            message.display, message.atom, message.format = self.display, self.atom("_NET_ACTIVE_WINDOW"), 32
            message.data.l[0], message.data.l[2] = 2, current
            self.x.XSendEvent(self.display, self.root, 0, (1 << 19) | (1 << 20), event)
            self.x.XFlush(self.display)
            self.pending.clear()
            print(f"Requested focus for ready Steam app {match[1].decode()} window {window:#x}", flush=True)
            break

    def drain(self):
        event = (C.c_long * 24)()
        while self.x.XPending(self.display):
            self.x.XNextEvent(self.display, event)

    def close(self):
        self.x.XCloseDisplay(self.display)


def serve():
    routes, display = Routes(), X11()
    subscriber = subprocess.Popen(["stdbuf", "-oL", "pactl", "subscribe"], stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, env={**os.environ, "LC_ALL": "C"})
    os.set_blocking(subscriber.stdout.fileno(), False)
    running = True
    def stop(*_):
        nonlocal running
        running = False
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    deadline = 0
    event_buffer = b""
    try:
        while running:
            if time.monotonic() >= deadline:
                try:
                    snapshot = pulse_snapshot()
                    sinks = snapshot["sinks"]
                    target = capture_sink(snapshot["sources"], snapshot["source_outputs"], sinks)
                    clients = {str(item["index"]): item for item in snapshot["clients"]}
                    routes.reconcile(target, snapshot["sink_inputs"], clients, sinks)
                    display.reconcile(target is not None)
                except (OSError, RuntimeError, ValueError) as error:
                    print(f"Session assist: {error}", flush=True)
                deadline = time.monotonic() + 5
                if routes.moving:
                    deadline = min(deadline, min(value[2] for value in routes.moving.values()))
            ready, _, _ = select.select([subscriber.stdout, display.fd], [], [],
                                        min(1, max(0, deadline - time.monotonic())))
            if subscriber.stdout in ready:
                data = os.read(subscriber.stdout.fileno(), 65536)
                if not data:
                    if not running:
                        break
                    raise RuntimeError("Audio event subscription ended")
                event_buffer, dirty = audio_events(event_buffer, data)
                if dirty:
                    deadline = 0  # React now; all queued events already share this snapshot.
            if display.fd in ready or display.x.XPending(display.display):
                display.drain()
                # Window events need only focus reconciliation, not an audio snapshot.
                display.reconcile(display.active_stream)
    finally:
        subscriber.terminate()
        try:
            subscriber.wait(timeout=2)
        except subprocess.TimeoutExpired:
            subscriber.kill()
            subscriber.wait()
        subscriber.stdout.close()
        display.close()
        try:
            snapshot = pulse_snapshot()
            clients = {str(item["index"]): item for item in snapshot["clients"]}
            routes.reconcile(None, snapshot["sink_inputs"], clients, snapshot["sinks"])
        except (OSError, RuntimeError, ValueError) as error:
            print(f"Audio recovery retained in {STATE}: {error}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("install", "uninstall", "run"))
    args = parser.parse_args()
    if args.action == "run":
        serve()
        return
    if UNIT.exists() and UNIT.read_text() != SERVICE:
        raise RuntimeError(f"Unrecognized unit left untouched: {UNIT}")
    if args.action == "install":
        UNIT.parent.mkdir(parents=True, exist_ok=True)
        UNIT.write_text(SERVICE)
        run("systemctl", "--user", "daemon-reload")
        run("systemctl", "--user", "enable", "--now", UNIT.name)
        print("Installed and started", UNIT.name)
    elif UNIT.exists():
        run("systemctl", "--user", "disable", "--now", UNIT.name)
        UNIT.unlink()
        run("systemctl", "--user", "daemon-reload")
        print("Removed", UNIT.name)


if __name__ == "__main__":
    main()
