# ArgusControl ESP-IDF Firmware

This project is now a native Argus motor-control firmware base for the ESP32-S3 control node, not a direct STM32 demo port.

## Current Firmware Model

The active build is a self-hosted MQTT continuous output-shaft RPM controller:

1. Set an output speed target with `argus/peristaltic/cmd/speed_pct`
2. Send `argus/peristaltic/cmd/run=true` to enable output and ramp toward the target
3. Send `argus/peristaltic/cmd/stop=true` or `cmd/run=false` to soft-stop by ramping to zero
4. Send `argus/peristaltic/cmd/e_stop=true` to immediately kill pulses and hold the motor locked
5. Send `argus/peristaltic/cmd/unlock=true` only when you intentionally want to release holding torque

The control node now hosts the MQTT broker on both its infrastructure Wi-Fi address and its local access-point address at port `1883`. Normal stop and e-stop both leave the driver enabled so the motor holds position. `unlock` is the explicit release path.

## Current Features

1. `STEP` output generated with `LEDC`
2. `DIR` output controlled directly from the speed sign
3. Optional `EN` driver control
4. Step counting with `PCNT`
5. Local MQTT broker for the pump-mounted HMI and temporary service tools
6. MQTT command interface for run, speed percent, soft stop, e-stop, and unlock
7. Configurable speed ramp to avoid abrupt pulse-frequency jumps
8. TWAI/CAN source retained for a possible future move back to CAN

## Local MQTT Setup

The pump control board is the broker. The normal HMI path is the controller's local access point:

```text
AP SSID: ArgusMotorTest
AP password: ArgusPump123
Broker host from AP clients: 192.168.4.1
Broker port: 1883
Protocol: MQTT 3.1.1
TLS/auth: disabled
```

The controller also still joins the configured external Wi-Fi network when it is available. For Node-RED or service laptop testing on that network, use the control node's station IP address:

```text
Broker host: <control-node-ip>
Broker port: 1883
Protocol: MQTT 3.1.1
TLS/auth: disabled
```

If external Wi-Fi is not available, the controller still starts the local AP and broker. The current station hostname is `ArgusMotorTestNode`.

## Pump HMI

The pump-mounted 2.4 inch ESP32 "cheap yellow display" HMI is currently an Arduino sketch. A reference copy is stored with this firmware repo:

1. [hmi/ArgusControl_HMI-Peristaltic_Pump/ArgusControl_HMI-Peristaltic_Pump.ino](./hmi/ArgusControl_HMI-Peristaltic_Pump/ArgusControl_HMI-Peristaltic_Pump.ino)

The HMI joins the controller AP, connects to MQTT broker `192.168.4.1:1883`, publishes pump commands, and subscribes to retained status topics.

## MQTT Topics

Command topics:

1. `argus/peristaltic/cmd/speed_pct`
   Payload: `0` to `100`
2. `argus/peristaltic/cmd/run`
   Payload: `true` starts/runs; `false` requests a soft stop
3. `argus/peristaltic/cmd/stop`
   Payload: `true` requests a soft stop
4. `argus/peristaltic/cmd/e_stop`
   Payload: `true` immediately stops pulses, clears the stored target, and locks the driver
5. `argus/peristaltic/cmd/unlock`
   Payload: `true` stops pulses and disables the driver so the motor can move freely

Status topics:

1. `argus/peristaltic/status/run`
2. `argus/peristaltic/status/rpm`
3. `argus/peristaltic/status/locked`
4. `argus/peristaltic/status/e_stop`
5. `argus/peristaltic/status/online`

Status topics are retained by the local broker so newly connected dashboards and displays immediately see the latest state. Command topics should not be retained.

## Retained CAN Protocol

The CAN files are still in `main/`, but they are not part of the active MQTT build. If the firmware moves back to CAN later, the retained protocol is:

1. `0x100` `SET_SPEED`
   Bytes `0..3`: signed little-endian `int32` output RPM in milli-RPM
2. `0x101` `START`
   Payload ignored
3. `0x102` `STOP`
   Payload ignored

Status frames:

1. `0x180` `SPEED_STATUS`
   Bytes `0..3`: signed commanded output RPM in milli-RPM
   Bytes `4..7`: signed applied output RPM in milli-RPM
2. `0x181` `STATE_STATUS`
   Bytes `0..3`: unsigned step counter
   Byte `4`: state flags
   Byte `5`: fault code
   Bytes `6..7`: reserved

State flags:

1. Bit `0`: driver enabled
2. Bit `1`: motion active
3. Bit `2`: forward direction
4. Bit `3`: run command active

## Project Layout

1. [main/app_main.c](./main/app_main.c)
   Application wiring, local MQTT broker callbacks, and status publishing
2. [main/argus_stepper.c](./main/argus_stepper.c)
   Continuous run/stop/speed control for the stepper interface
3. [main/argus_mqtt_broker.c](./main/argus_mqtt_broker.c)
   Lightweight local MQTT broker for the pump control network
4. [main/argus_can.c](./main/argus_can.c)
   Retained TWAI transport for Argus command/status frames
5. [main/argus_protocol.h](./main/argus_protocol.h)
   Protocol IDs and payload definitions
6. [hmi/ArgusControl_HMI-Peristaltic_Pump/ArgusControl_HMI-Peristaltic_Pump.ino](./hmi/ArgusControl_HMI-Peristaltic_Pump/ArgusControl_HMI-Peristaltic_Pump.ino)
   Arduino reference sketch for the pump-mounted display HMI

## Current Assumptions

1. Control board target: `ESP32-S3`
2. CAN bitrate: `500 kbps`
3. Motor full steps per rev: `200`
4. Microstep setting: `32`
5. Gearbox ratio: `10:1`
6. Output steps per rev: `64000`
7. Motor max speed: `2000 RPM`
8. Output max speed at 10:1: `200 RPM`
9. Driver enable settle delay: `20 ms`
10. Active MQTT max speed command: `72 RPM`
11. Default ramp rate: `10 RPM/sec`
12. Local MQTT broker port: `1883`

## Build

```powershell
cd c:\Users\bount\Dev\Argus\ArgusControl_PumpController-V2
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

## Next Recommended Steps

1. Tune `ARGUS_RAMP_RPM_PER_SEC_MILLI` after hardware testing
2. Flash and verify the HMI on the controller AP at `192.168.4.1`
3. Decide whether AP credentials should move out of source before production
4. Decide whether the retained CAN path should be restored later
