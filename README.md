# RescueFlow

**Emergency Vehicle Traffic Signal Priority System — SUMO Simulation + ESP32 Prototype**

RescueFlow is an experimental traffic-signal priority system designed to reduce emergency-vehicle delays at an intersection. It combines a traffic simulation built with **Eclipse SUMO**, a **Python/TraCI** priority controller, and an **ESP32** prototype that reproduces the simulated traffic lights using six LEDs.

The project demonstrates emergency-vehicle detection within the simulation, signal preemption, controlled yellow/all-red transitions, and synchronized operation between simulated and physical signals.

> **Scope:** RescueFlow is a research and portfolio demonstration, **not** a certified controller for real-world road intersections.

## Demonstrations

**SUMO simulation**

[Watch the SUMO simulation](Recording%20SUMO%20RescueFlow%20Simulation.mp4)


## Features

- Emergency-vehicle priority at a four-way intersection using SUMO and TraCI.
- East–West and North–South traffic-light control with green, yellow, and all-red transition phases.
- Priority handling for an approaching ambulance and subsequent return to normal operation.
- Physical demonstration using an ESP32 and six individually controlled LEDs.
- Wi-Fi synchronization of physical LEDs with the SUMO traffic-light state.
- ESP32 standalone traffic-light operation without an active SUMO connection.
- Hybrid mode with automatic return to standalone operation when synchronization stops.
- Local, phone-accessible control interface for priority requests and operating-mode selection.
- Optional laptop-side agent for launching the simulation from the phone interface (requires a running agent on the laptop).

## Architecture

```text
                 SUMO intersection simulation
                            |
                    TraCI (Python)
                            |
                 rescueflow_controller.py
                            |
                     esp32_bridge.py
                            |
                      Local Wi-Fi
                            |
               ESP32 traffic-light controller
                      /             \
                 Six LEDs       Phone web UI

Optional: iPhone web UI -> ESP32 -> laptop agent -> SUMO process
```

The ESP32 is powered over USB; its connection to the simulation uses local Wi-Fi. The phone and laptop can connect to the ESP32's access-point network for local control and synchronization.

### Operating modes

| Mode | Behavior |
| --- | --- |
| **Automatic Hybrid** | ESP32 operates locally and follows SUMO when synchronization is active; it returns to standalone operation after communication is lost. |
| **Standalone** | ESP32 runs its own signal sequence and supports local emergency-priority requests. |
| **SUMO Sync** | ESP32 follows traffic-light commands from the simulation. If synchronization is lost, the demonstration outputs all red. |

The mobile interface can select modes locally. Running SUMO still requires the laptop-side Python controller or the optional laptop agent.

## Hardware

- ESP32 development board
- Breadboard and jumper wires
- Six LEDs: red, yellow, and green for each of two traffic directions
- One suitable current-limiting resistor per LED
- USB cable for ESP32 power and firmware upload

### ESP32 pin assignments

| Signal | GPIO |
| --- | ---: |
| East–West red | 18 |
| East–West yellow | 19 |
| East–West green | 21 |
| North–South red | 22 |
| North–South yellow | 23 |
| North–South green | 25 |

The LED outputs use **active-low** logic: `LOW = ON`, `HIGH = OFF`. The demonstration wiring connects each LED and its series resistor between 3.3 V and the corresponding GPIO pin. **Never connect LEDs without appropriate current-limiting resistors.**

## Simulation results

In the recorded baseline-versus-priority simulation scenario:

| Ambulance metric | Baseline | RescueFlow | Change |
| --- | ---: | ---: | ---: |
| Journey time | 57 s | 20 s | **−64.9%** |
| Travel time | 56 s | 19 s | **−66.1%** |
| Waiting time | 43 s | 6 s | **−86.0%** |

Priority can impose a cost on non-emergency traffic. In that comparison, regular-vehicle average travel time increased from **12.36 s to 17.42 s**, and average waiting time increased from **5.88 s to 10.34 s**. These figures are from a specific simulated scenario and do not establish performance on public roads.

Additional simulated traffic scenarios and phase-transition tests were used during development. Physical testing confirmed ESP32 phase control, Wi-Fi synchronization, and hybrid-mode fallback on the breadboard prototype.

## Repository layout

```text
RescuFlow/
├── hardware/
│   ├── platformio.ini
│   ├── include/
│   │   ├── pin_config.h
│   │   └── traffic_control.h
│   └── src/
│       └── esp32_controller.cpp
├── media/
│   └── sumo_simulation.mp4
├── rescueflow_controller.py
├── esp32_bridge.py
├── rescueflow_laptop_agent.py       # Optional; if included
├── rescueflow.net.xml
├── rescueflow.rou.xml
├── rescueflow.sumocfg
├── cross_traffic.rou.xml            # If used by your scenario
└── README.md
```

## Getting started

### 1. Prerequisites

- **Eclipse SUMO**, including `sumo-gui` and TraCI support
- **Python 3** with the SUMO/TraCI Python package accessible to your environment
- **VS Code + PlatformIO** for the ESP32 firmware
- An ESP32 development board and the LED circuit shown above

Install SUMO according to its official instructions and ensure its executables are on your `PATH` (or that your Python controller can locate them).

### 2. Run the SUMO simulation

From the repository root on Windows:

```powershell
py rescueflow_controller.py
```

This starts the simulation and applies the RescueFlow emergency-priority logic. The physical LEDs synchronize when the ESP32 is powered, the updated firmware is installed, and your laptop is connected to the ESP32's local Wi-Fi.

### 3. Upload ESP32 firmware

Open the `hardware/` directory as a PlatformIO project and use **Upload**, or run:

```powershell
platformio run -d hardware -t upload
```

The command requires the PlatformIO CLI to be installed and accessible. On some Windows installations, it is available through `~/.platformio/penv/Scripts/platformio.exe` instead.

### 4. Use the mobile interface

1. Power the ESP32 using USB.
2. Connect your phone to the ESP32's local access-point network.
3. Open `http://192.168.4.1` in a browser.
4. Select an available operating mode or submit an emergency-priority request.

For remote starting of SUMO from the phone, the optional `rescueflow_laptop_agent.py` must first be running on the Windows laptop. The agent is not required for ordinary standalone LED operation.

## Security and limitations

- **Demonstration only:** Do not connect this design to real traffic-signal infrastructure.
- Wi-Fi and API access are local prototype controls, not production-grade authentication.
- Review and replace demonstration credentials in the firmware before sharing or deploying the source code. Avoid publishing private passwords or tokens.
- LED outputs are visual indicators; they do not represent a safety-certified signal driver.
- A successful simulated priority event does not establish vehicle detection, wireless reliability, or road safety under real-world conditions.

## Project status

**Functional simulation and breadboard prototype.** SUMO priority control, ESP32 LED outputs, Wi-Fi phase synchronization, and standalone/hybrid operation have been demonstrated. Further work would be required for rigorous hardware-in-the-loop testing, communications fault analysis, and any safety-critical application.
