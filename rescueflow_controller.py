# RESCUEFLOW_ESP32_SYNC (installed once)
from esp32_bridge import ESP32Bridge
_rescueflow_esp32 = ESP32Bridge()
import csv
from pathlib import Path
import traci

# ============================================
# RescueFlow - Optimized Traffic Controller
# ============================================

BASE = Path(__file__).resolve().parent

SUMO = r"C:\Program Files (x86)\SUMO\bin\sumo-gui.exe"
CONFIG = BASE / "rescueflow.sumocfg"

RESULTS = BASE / "results"
RESULTS.mkdir(exist_ok=True)

TLS = "J0"

MIN_NS_GREEN = 5.0
MIN_EW_RECOVERY_GREEN = 10.0
MAX_RECOVERY_GREEN = 18.0
MAX_EW_GREEN = 50.0

SIMULATION_DELAY = 1000

events = []

preemptions = 0
recoveries = 0
extensions = 0
conflicts = 0
invalid_transitions = 0

recovery_pending = False
priority_green_seen = False

detected_ids = set()
granted_ids = set()
cleared_ids = set()

last_phase = None
phase_started = None


# ============================================
# Event Logger
# ============================================

def log_event(now, event, vehicle="", phase=None):

    events.append({
        "time_s": round(now, 2),
        "event": event,
        "vehicle_id": vehicle,
        "phase": phase
    })

    print(f"[{now:.0f}s] {event} {vehicle}")


# ============================================
# Install Six-Phase Traffic Signal
# ============================================

def install_signals():

    logics = traci.trafficlight.getAllProgramLogics(TLS)

    original = next(
        (logic for logic in logics
         if len(logic.phases) == 4),
        None
    )

    if original is None:
        raise RuntimeError(
            "Original four-phase signal program missing"
        )

    states = [p.state for p in original.phases]

    if any(len(s) != 12 for s in states):
        raise RuntimeError(
            "Unexpected traffic signal configuration"
        )

    Phase = traci.trafficlight.Phase
    red = "r" * 12

    phases = [
        Phase(42, states[0]),
        Phase(3, states[1]),
        Phase(2, red),
        Phase(42, states[2]),
        Phase(3, states[3]),
        Phase(2, red)
    ]

    program = traci.trafficlight.Logic(
        "rescueflow_optimized",
        0,
        0,
        phases
    )

    traci.trafficlight.setProgramLogic(
        TLS, program
    )

    traci.trafficlight.setProgram(
        TLS, "rescueflow_optimized"
    )

    traci.trafficlight.setPhase(TLS, 0)

    links = traci.trafficlight.getControlledLinks(TLS)

    if len(links) != 12:
        raise RuntimeError(
            "Unexpected controlled link configuration"
        )

    def incoming_lanes(state):

        lanes = set()

        for index, signal in enumerate(state):

            if signal not in "gG":
                continue

            for link in links[index] or []:
                if link and link[0]:
                    lanes.add(link[0])

        return sorted(lanes)

    ew_lanes = incoming_lanes(states[0])
    ns_lanes = incoming_lanes(states[2])

    if not ew_lanes or not ns_lanes:
        raise RuntimeError("Approach lanes missing")

    if set(ew_lanes) & set(ns_lanes):
        raise RuntimeError(
            "Overlapping approach groups"
        )

    ew_indices = [
        i for i, s in enumerate(states[0])
        if s in "gG"
    ]

    ns_indices = [
        i for i, s in enumerate(states[2])
        if s in "gG"
    ]

    return (
        ew_lanes,
        ns_lanes,
        ew_indices,
        ns_indices,
        red
    )


# ============================================
# Queue Monitoring
# ============================================

def queue_count(lanes):

    return sum(
        traci.lane.getLastStepHaltingNumber(lane)
        for lane in lanes
    )


# ============================================
# Start SUMO
# ============================================

if not CONFIG.exists():
    raise FileNotFoundError(CONFIG)

traci.start([
    SUMO,
    "-c", str(CONFIG),
    "--start",
    "--delay", str(SIMULATION_DELAY)
])

try:

    (
        ew_lanes,
        ns_lanes,
        ew_indices,
        ns_indices,
        all_red
    ) = install_signals()

    print("\nRESCUEFLOW CONTROLLER ACTIVE")
    print("Optimized traffic control enabled\n")

    while traci.simulation.getMinExpectedNumber() > 0:

        traci.simulationStep()
        _rescueflow_esp32.sync(traci)

        now = traci.simulation.getTime()

        phase = traci.trafficlight.getPhase(TLS)

        # ====================================
        # Ambulance Detection
        # ====================================

        active = set()

        for vid in traci.vehicle.getIDList():

            if traci.vehicle.getTypeID(vid) != "ambulance":
                continue

            road = traci.vehicle.getRoadID(vid)

            if road == "-E3" or road.startswith(":J0"):
                active.add(vid)

                if vid not in detected_ids:
                    detected_ids.add(vid)
                    log_event(
                        now,
                        "AMBULANCE DETECTED",
                        vid,
                        phase
                    )

        # Identify cleared ambulances
        for vid in detected_ids - active - cleared_ids:

            cleared_ids.add(vid)

            log_event(
                now,
                "AMBULANCE CLEARED",
                vid,
                phase
            )

        ambulance_active = bool(active)

        # ====================================
        # Emergency Preemption
        # ====================================

        if ambulance_active and phase == 3:

            spent = traci.trafficlight.getSpentDuration(
                TLS
            )

            if spent >= MIN_NS_GREEN:

                traci.trafficlight.setPhase(TLS, 4)

                recovery_pending = True
                priority_green_seen = False

                preemptions += 1

                log_event(
                    now,
                    "NS PREEMPTION",
                    phase=4
                )

        # ====================================
        # Ambulance Green Priority
        # ====================================

        elif ambulance_active and phase == 0:

            if recovery_pending:
                priority_green_seen = True

            for vid in active:

                if vid not in granted_ids:

                    granted_ids.add(vid)

                    log_event(
                        now,
                        "PRIORITY GRANTED",
                        vid,
                        0
                    )

            spent = traci.trafficlight.getSpentDuration(
                TLS
            )

            remaining = (
                traci.trafficlight.getNextSwitch(TLS)
                - now
            )

            if remaining <= 2 and spent < MAX_EW_GREEN:

                extra = min(
                    3.0,
                    MAX_EW_GREEN - spent
                )

                if extra > 0:

                    traci.trafficlight.setPhaseDuration(
                        TLS, extra
                    )

                    extensions += 1

                    log_event(
                        now,
                        "GREEN EXTENDED",
                        phase=0
                    )

        # ====================================
        # Queue-Aware Traffic Recovery
        # ====================================

        elif (
            not ambulance_active
            and recovery_pending
            and phase == 0
        ):

            priority_green_seen = True

            spent = traci.trafficlight.getSpentDuration(
                TLS
            )

            ew_queue = queue_count(ew_lanes)
            ns_queue = queue_count(ns_lanes)

            should_recover = (
                spent >= MIN_EW_RECOVERY_GREEN
                and (
                    ns_queue > ew_queue
                    or (
                        spent >= MAX_RECOVERY_GREEN
                        and ns_queue > 0
                    )
                )
            )

            if should_recover:

                # Normal yellow and all-red
                # transitions remain active.
                traci.trafficlight.setPhase(TLS, 1)

                recoveries += 1
                recovery_pending = False

                log_event(
                    now,
                    "QUEUE-AWARE RECOVERY",
                    phase=1
                )

        # ====================================
        # Recovery State Management
        # ====================================

        phase = traci.trafficlight.getPhase(TLS)

        if (
            recovery_pending
            and priority_green_seen
            and not ambulance_active
            and phase != 0
        ):
            recovery_pending = False

        # ====================================
        # Signal Safety Monitoring
        # ====================================

        state = traci.trafficlight.getRedYellowGreenState(
            TLS
        )

        ew_green = any(
            state[i] in "gG" for i in ew_indices
        )

        ns_green = any(
            state[i] in "gG" for i in ns_indices
        )

        if ew_green and ns_green:

            conflicts += 1

            log_event(
                now,
                "CONFLICTING GREEN GROUPS",
                phase=phase
            )

        if phase in (2, 5) and state != all_red:

            invalid_transitions += 1

            log_event(
                now,
                "INVALID ALL-RED STATE",
                phase=phase
            )

        if phase == 0:

            spent = traci.trafficlight.getSpentDuration(
                TLS
            )

            if spent > MAX_EW_GREEN + 0.01:
                invalid_transitions += 1

        # ====================================
        # Phase Sequence Verification
        # ====================================

        if phase != last_phase:

            if last_phase is not None:

                elapsed = now - phase_started

                expected = (last_phase + 1) % 6

                if phase != expected:
                    invalid_transitions += 1

                if last_phase in (1, 4) and elapsed < 3:
                    invalid_transitions += 1

                if last_phase in (2, 5) and elapsed < 2:
                    invalid_transitions += 1

            last_phase = phase
            phase_started = now

finally:

    traci.close()

    # ========================================
    # Save Controller Events
    # ========================================

    output = RESULTS / "controller_events.csv"

    with open(
        output,
        "w",
        newline="",
        encoding="utf-8"
    ) as file:

        writer = csv.DictWriter(
            file,
            fieldnames=[
                "time_s",
                "event",
                "vehicle_id",
                "phase"
            ]
        )

        writer.writeheader()
        writer.writerows(events)


# ============================================
# Final Controller Summary
# ============================================

print("\n========== RESCUEFLOW SUMMARY ==========")

print("Ambulances detected:", len(detected_ids))
print("Priority granted:", len(granted_ids))
print("Ambulances cleared:", len(cleared_ids))

print("Preemptions:", preemptions)
print("Green extensions:", extensions)
print("Recovery actions:", recoveries)

print("Conflicting green observations:", conflicts)
print("Invalid transitions:", invalid_transitions)

print("Events saved:", output)

print("========================================")