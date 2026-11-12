"""RescueFlow Your Phone to Windows launcher (local ESP32 Wi-Fi demonstration only).

The ESP32 hosts a fixed start/stop request flag. This agent polls that flag and
starts ONLY the local RescueFlow controller.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

ESP32 = "http://192.168.4.1"
KEY = "1234"  # Must match the ESP32 firmware and esp32_bridge.py
ROOT = Path(__file__).resolve().parent
CONTROLLER = ROOT / "rescueflow_controller.py"
LOG = ROOT / "rescueflow_laptop.log"
HTTP_TIMEOUT = 1.0
POLL_SECONDS = 0.55
OFFLINE_STOP_SECONDS = 6.0


def request(path: str, values: dict[str, str] | None = None):
    if values is None:
        query = urllib.parse.urlencode({"key": KEY})
        req = urllib.request.Request(f"{ESP32}{path}?{query}")
    else:
        values = {"key": KEY, **values}
        encoded = urllib.parse.urlencode(values).encode("ascii")
        req = urllib.request.Request(
            ESP32 + path, data=encoded, method="POST",
            headers={"Content-Type": "application/x-www-form-urlencoded"},
        )
    with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as response:
        return json.load(response)


class Launcher:
    def __init__(self):
        self.process: subprocess.Popen | None = None
        self.log_handle = None
        self.state = "READY"
        self.last_request_seen = time.monotonic()
        self.last_reported = 0.0
        self.was_requested = False
        self.attempted = False
        self.connection_lost_stopped = False

    def report(self, force=False):
        if not force and time.monotonic() - self.last_reported < 1.0:
            return
        request("/api/laptop/heartbeat", {"state": self.state})
        self.last_reported = time.monotonic()

    def start(self):
        self.attempted = True
        self.connection_lost_stopped = False
        if not CONTROLLER.is_file():
            self.state = "ERROR"
            print(f"[ERROR] Controller missing: {CONTROLLER}", flush=True)
            return
        try:
            self.log_handle = LOG.open("a", encoding="utf-8")
            self.log_handle.write("\n=== Remote SUMO start ===\n")
            self.log_handle.flush()
            kwargs = {}
            if os.name == "nt":
                kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
            else:
                kwargs["start_new_session"] = True
            self.process = subprocess.Popen(
                [sys.executable, "-u", str(CONTROLLER)],
                cwd=ROOT,
                stdout=self.log_handle,
                stderr=subprocess.STDOUT,
                **kwargs,
            )
            self.state = "RUNNING"
            print(f"[RUNNING] Launched SUMO controller (PID {self.process.pid})", flush=True)
        except OSError as exc:
            self.state = "ERROR"
            print(f"[ERROR] Could not start SUMO: {exc}", flush=True)
            self._close_log()

    def _close_log(self):
        if self.log_handle:
            self.log_handle.close()
            self.log_handle = None

    def stop(self, reason="Stopped by iPhone"):
        p = self.process
        if p is not None and p.poll() is None:
            print(f"[STOP] {reason}", flush=True)
            try:
                if os.name == "nt":
                    subprocess.run(
                        ["taskkill", "/PID", str(p.pid), "/T", "/F"],
                        capture_output=True, text=True, timeout=6, check=False,
                    )
                else:
                    os.killpg(p.pid, signal.SIGTERM)
                try:
                    p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    if os.name == "nt":
                        p.kill()
                    else:
                        os.killpg(p.pid, signal.SIGKILL)
                    p.wait(timeout=2)
            except (OSError, subprocess.TimeoutExpired) as exc:
                print(f"[WARN] Process cleanup: {exc}", flush=True)
        self.process = None
        self._close_log()

    def tick(self):
        now = time.monotonic()
        try:
            control = request("/api/laptop/control")
            desired = control.get("run")
            if not isinstance(desired, bool):
                raise ValueError("ESP32 returned an invalid request")
            self.last_request_seen = time.monotonic()
            # Only a new user Start request may launch a process. No restart loop.
            if desired != self.was_requested:
                self.attempted = False
                self.connection_lost_stopped = False
                self.was_requested = desired
            if not desired:
                if self.process is not None:
                    self.stop()
                self.state = "READY"
            elif not self.attempted:
                self.state = "STARTING"
                self.report(force=True)
                self.start()
            elif self.process is not None:
                exit_code = self.process.poll()
                if exit_code is not None:
                    print(f"[EXITED] Controller stopped with code {exit_code}. Check {LOG.name}", flush=True)
                    self.process = None
                    self._close_log()
                    self.state = "EXITED"
                else:
                    self.state = "RUNNING"
            elif self.connection_lost_stopped:
                self.state = "ERROR"
            # If launch failed or the simulation exited, require Stop then Start.
            self.report()
        except (OSError, ValueError, TimeoutError) as exc:
            if now - self.last_request_seen >= OFFLINE_STOP_SECONDS:
                if self.process is not None and self.process.poll() is None:
                    self.stop("ESP32 Wi-Fi unreachable; stopping SUMO safely")
                    self.connection_lost_stopped = True
                    self.attempted = True
                    self.state = "ERROR"
                # Continue reconnect attempts without restarting SUMO automatically.
            if int(now) % 10 == 0:
                print(f"[WAIT] ESP32 unreachable ({exc}); reconnect Windows to RescueFlow-Demo", flush=True)


def main():
    launcher = Launcher()
    print("RescueFlow laptop launcher ready. Connect this laptop to RescueFlow-Demo Wi-Fi.", flush=True)
    print("Open http://192.168.4.1 on your iPhone and press Start SUMO + Physical Test.", flush=True)
    print(f"SUMO log: {LOG}", flush=True)
    try:
        while True:
            launcher.tick()
            time.sleep(POLL_SECONDS)
    except KeyboardInterrupt:
        print("\nExiting launcher; stopping its SUMO process.", flush=True)
    finally:
        launcher.stop("Laptop launcher exiting")


if __name__ == "__main__":
    main()
