from __future__ import annotations

import json
import time
import urllib.error
import urllib.parse
import urllib.request

ESP32_URL = "http://192.168.4.1"
API_KEY = "1234"
TRAFFIC_LIGHT_ID = "J0"
HEARTBEAT_SECONDS = 0.8  # keep ESP32's 3.5s link watchdog alive
REQUEST_TIMEOUT_SECONDS = 0.5

PHASE_NAMES = (
    "EW Green", "EW Yellow", "All Red EW->NS",
    "NS Green", "NS Yellow", "All Red NS->EW",
)


class ESP32Bridge:
    def __init__(self, base_url=ESP32_URL, api_key=API_KEY,
                 traffic_light_id=TRAFFIC_LIGHT_ID):
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.traffic_light_id = traffic_light_id
        self._last_phase = None
        self._last_ok = 0.0
        self._last_warning = 0.0
        self._checked_phase_count = False

    def _post_phase(self, phase: int) -> bool:
        data = urllib.parse.urlencode({
            "key": self.api_key, "phase": str(phase)
        }).encode("ascii")
        req = urllib.request.Request(
            self.base_url + "/api/sync", data=data, method="POST",
            headers={"Content-Type": "application/x-www-form-urlencoded"},
        )
        with urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT_SECONDS) as response:
            payload = json.load(response)
            if not payload.get("ok"):
                raise RuntimeError("ESP32 did not acknowledge phase")
            return bool(payload.get("ignored", False))

    def sync(self, traci_module) -> None:
        """Call once per simulation step, after existing phase-control logic."""
        if not self._checked_phase_count:
            logics = traci_module.trafficlight.getAllProgramLogics(
                self.traffic_light_id
            )
            program_id = traci_module.trafficlight.getProgram(
                self.traffic_light_id
            )
            logic = next((x for x in logics if x.programID == program_id), None)
            if logic is None or len(logic.phases) != 6:
                raise RuntimeError(
                    f"{self.traffic_light_id}: expected active 6-phase SUMO program "
                    "(0 EW green, 1 EW yellow, 2 all red, "
                    "3 NS green, 4 NS yellow, 5 all red). "
                    "Inspect the SUMO network phase definitions."
                )
            self._checked_phase_count = True

        phase = int(traci_module.trafficlight.getPhase(self.traffic_light_id))
        if not 0 <= phase < len(PHASE_NAMES):
            raise RuntimeError(f"Unexpected SUMO phase {phase}; refusing to sync")

        now = time.monotonic()
        if phase == self._last_phase and now - self._last_ok < HEARTBEAT_SECONDS:
            return
        try:
            ignored = self._post_phase(phase)
        except (OSError, ValueError, RuntimeError) as exc:
            if now - self._last_warning >= 3.0:
                print(f"[ESP32] Sync unavailable: {exc}")
                self._last_warning = now
            # Retry on subsequent steps. SUMO continues normally.
            return

        if ignored:
            # An iPhone-selected standalone override wins over Python.
            # Keep checking periodically so sync resumes when the user allows it.
            if self._last_phase is not None:
                print("[ESP32] Manual standalone override: phase mirroring paused")
            self._last_phase = None
            self._last_ok = now
            return

        if phase != self._last_phase:
            print(f"[ESP32] Mirroring SUMO phase {phase}: {PHASE_NAMES[phase]}")
        self._last_phase = phase
        self._last_ok = now

    def stop(self) -> None:
        """Request both RED when the simulation ends; may fail if Wi-Fi drops."""
        try:
            self._post_phase(2)
        except (OSError, ValueError, RuntimeError):
            pass
