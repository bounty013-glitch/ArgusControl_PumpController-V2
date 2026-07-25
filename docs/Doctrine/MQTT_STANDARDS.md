# MQTT Standards

This document defines MQTT conventions used throughout the Argus ecosystem.
The purpose of these standards is to create a consistent communication model between all Argus products, field units, customer deployments, and services.
MQTT topics should be predictable, discoverable, human-readable, and field-support friendly.
A technician, operator, developer, or support person should be able to locate a unit's data by knowing the customer and the field unit name.

## Core Philosophy

MQTT transports information.
MQTT is not authoritative.
MQTT messages communicate:
- intent
- state
- telemetry
- events
- health
- configuration
Authority remains with controllers and safety systems.
MQTT should make the system easier to understand, not harder to navigate.

## Primary Topic Structure

All standard Argus topics follow this format:
argus/<client>/<unit_id>/<category>/<device_or_system>/<item>
Examples:
argus/paladin/skid7/telemetry/pump1/rpm
argus/paladin/skid7/state/pump1/mode
argus/paladin/skid7/status/core/mqtt
argus/paladin/skid7/command/pump1/start
argus/paladin/skid7/alarm/engine1/high_temp
argus/paladin/skid7/config/node1/publish_interval_ms
The first two identifiers should answer the field support question:
Who is the customer?
Which unit is it?
Example:
argus/paladin/skid7/...
This allows support to navigate MQTT data the same way field personnel describe equipment.

## Why Client And Unit Come First

Field support must be fast.
Operators should be able to say:
I'm with Paladin on Skid 7.
Support should immediately know where to look:
argus/paladin/skid7/
Argus does not rely on hard-to-read enclosure serial numbers or arbitrary device IDs as the primary navigation method.
Serial numbers and hardware IDs may still exist as metadata, but they should not be the primary support path.

## Client Naming

Client names should be:
- lowercase
- short
- human-readable
- stable
- recognizable to field personnel
Examples:
paladin
stealth
ironside
prime
Avoid:
customer001
cust-a
14962946
Client names should match how the customer is commonly identified in operations.

## Unit ID Naming

Unit IDs should be descriptive and field-recognizable.
Examples:
skid7
skid4
coil1
pump301
plant11
Avoid:
unit001
box14962946
esp32a7f31
tempnode
The unit ID should match the name operators, managers, and support personnel actually use.
If the field calls it Skid 7, the MQTT unit ID should be:
skid7

## Device And System Naming

Within a unit, devices should be named by function.
Examples:
pump1
pump2
engine1
core
node1
hmi1
ai_node1
j1939_node1
flowmeter1
pressure1
Device names should be consistent within the unit.
Avoid vague names:
thing1
board2
esp1
data
sensor

## Standard Categories

The following categories are reserved.

### telemetry

Measured or calculated process values.
Examples:
argus/paladin/skid7/telemetry/pump1/rpm
argus/paladin/skid7/telemetry/pump1/pressure
argus/paladin/skid7/telemetry/engine1/coolant_temp
argus/paladin/skid7/telemetry/ai_node1/ch1/raw
argus/paladin/skid7/telemetry/ai_node1/ch1/eng
Telemetry represents values.

### state

Current operating state.
Examples:
argus/paladin/skid7/state/pump1/mode
argus/paladin/skid7/state/pump1/health
argus/paladin/skid7/state/core/system
argus/paladin/skid7/state/node1/online
Example payloads:
RUNNING
HOLDING
STOPPED
OFFLINE
FAULT
STARTING
State should be human-readable whenever practical.

### command

Operator or supervisory intent.
Examples:
argus/paladin/skid7/command/pump1/start
argus/paladin/skid7/command/pump1/stop
argus/paladin/skid7/command/pump1/target_pct
argus/paladin/skid7/command/core/reboot
Commands express intent.
Commands do not guarantee action.
Controllers determine actual behavior and publish state in response.

### status

Subsystem health and communication status.
Examples:
argus/paladin/skid7/status/core/wifi
argus/paladin/skid7/status/core/mqtt
argus/paladin/skid7/status/hmi1/link
argus/paladin/skid7/status/node1/uptime_s
Status describes health, connectivity, and readiness.

### event

Significant occurrences.
Examples:
argus/paladin/skid7/event/pump1/started
argus/paladin/skid7/event/pump1/stopped
argus/paladin/skid7/event/node1/rebooted
argus/paladin/skid7/event/core/config_saved
Events are not continuous state.
Events describe something that happened.

### alarm

Alarm or fault conditions.
Examples:
argus/paladin/skid7/alarm/pump1/overpressure
argus/paladin/skid7/alarm/engine1/high_temp
argus/paladin/skid7/alarm/node1/sensor_fault
argus/paladin/skid7/alarm/core/mqtt_lost
Alarm topics should be clear and specific.

### config

Configuration values and configuration commands.
Examples:
argus/paladin/skid7/config/node1/publish_interval_ms
argus/paladin/skid7/config/node1/state
argus/paladin/skid7/config/node1/cmd
argus/paladin/skid7/config/node1/reply
Configuration should be readable, confirmable, and persistent when required.
Configuration changes should be deliberate.

### metadata

Identity and descriptive information.
Examples:
argus/paladin/skid7/metadata/core/serial_number
argus/paladin/skid7/metadata/core/firmware_version
argus/paladin/skid7/metadata/core/location
argus/paladin/skid7/metadata/pump1/model
Metadata may include serial numbers, firmware versions, hardware revisions, and physical labels.
Metadata supports support work.
It should not replace human-readable client and unit naming.

## Command Philosophy

Commands communicate intent.
Examples:
Good:
START
STOP
SET_TARGET
50
Poor:
FORCE_RUNNING
OVERRIDE_STATE
MAKE_TRUE
MQTT should request.
Controllers should decide.
Controllers should publish resulting state after evaluating the request.

## Human-Readable Payloads

Whenever practical, payloads should be understandable without decoding.
Preferred:
RUNNING
STOPPED
HOLDING
ONLINE
OFFLINE
FAULT
Avoid unnecessary encoded values:
0
1
2
3
Numeric values are acceptable when they represent actual process values.
Examples:
700
85.2
4.5
Human readability matters because operators, support personnel, Node-RED, Codex, and future tools may all inspect MQTT traffic directly.

## Raw, Filtered, And Engineering Values

Raw, filtered, and engineering-unit values must not share the same topic.
Example:
argus/paladin/skid7/telemetry/ai_node1/ch1/raw
argus/paladin/skid7/telemetry/ai_node1/ch1/filtered
argus/paladin/skid7/telemetry/ai_node1/ch1/eng
This prevents confusion between raw sensor data and human-facing values.

## Channel Identity And Scaling

AI Node channels are configurable signal slots.
The channel ID identifies the hardware or logical slot, not the meaning of the signal.

Example:
ch1 is a channel ID.
`pump1_discharge_pressure` is a configured signal identity.

Channel meaning should be published as metadata:
argus/paladin/skid7/metadata/ai_node1/ch1/device
argus/paladin/skid7/metadata/ai_node1/ch1/signal
argus/paladin/skid7/metadata/ai_node1/ch1/signal_type
argus/paladin/skid7/metadata/ai_node1/ch1/units
argus/paladin/skid7/metadata/ai_node1/ch1/scale_type
argus/paladin/skid7/metadata/ai_node1/ch1/config_revision

Channel configuration should be readable and deliberately changeable:
argus/paladin/skid7/config/ai_node1/ch1/cmd
argus/paladin/skid7/config/ai_node1/ch1/reply
argus/paladin/skid7/config/ai_node1/ch1/state

Channel health and verification should be visible:
argus/paladin/skid7/state/ai_node1/ch1/health
argus/paladin/skid7/state/ai_node1/ch1/scaling_verified
argus/paladin/skid7/state/ai_node1/ch1/calibration_verified

Scaling and calibration are per-channel concerns.
Live tuning may be supported, but the authoritative node must validate, persist, and publish the resulting channel state.

No consumer should treat `eng` as process truth unless the channel mapping, scaling, and verification state are explicit.

## State Ownership

Ownership must be clear.
Examples:
Controller:
- owns machine state
HMI:
- publishes operator intent
MQTT:
- transports messages
Node-RED:
- coordinates messages
ArgusCore:
- displays, orchestrates, and communicates
Multiple systems should not claim ownership of the same state.
If two systems disagree, the authoritative source must be obvious.

## Retained Messages

Use retained messages for:
- current online state
- current operating state
- current configuration state
- device metadata
- last known health status
Avoid retained messages for:
- momentary commands
- transient events
- one-time alarms
- heartbeat pulses
Retained messages should help a newly connected system understand current state without replaying history.

## Heartbeats

All active devices should publish heartbeat or online status.
Recommended:
argus/<client>/<unit_id>/status/<device>/heartbeat
argus/<client>/<unit_id>/state/<device>/online
Example:
argus/paladin/skid7/status/node1/heartbeat
argus/paladin/skid7/state/node1/online
Recommended payloads:
ONLINE
OFFLINE
or a monotonically increasing heartbeat counter when useful.
Heartbeat intervals should be consistent within a project.

## Last Will

MQTT last will topics should use the standard state path.
Example:
argus/paladin/skid7/state/node1/online
Last will payload:
OFFLINE
Online payload after connection:
ONLINE
This allows dashboards and support tools to identify offline devices quickly.

## Backward Compatibility

Do not break deployed topic structures without a transition plan.
When improving a topic structure:
- keep old topics temporarily if needed
- publish both old and new during migration
- document the transition
- remove old topics only after field systems are updated
Field reliability takes priority over clean architecture.

## Phase 4C Pump Controller Profile

The V2 pump controller's accepted Phase 4C implementation is a strict deployment profile of these ecosystem standards:

- Its root is built from commissioned identity as `argus/<client_id>/<unit_id>`; the accepted controller is `argus/paladin/pump_001`.
- External publishers may write only the seven exact `command/pump1/...` topics and `status/supervisor/heartbeat`. Metadata, state, status, telemetry, events, alarms, and configuration are controller-owned.
- Commands require QoS 1, RETAIN false, a current broker-lifecycle session, a connection-bound fresh heartbeat lease, a newer nonzero uint32 sequence, a bounded command ID, and a strict topic-specific value.
- PUBACK means transport receipt only. `event/pump1/command_result` reports the application decision, while retained controller state remains authoritative.
- Heartbeats are sent every two seconds; after six seconds the lease becomes stale. Communication loss changes link observability only and does not stop or otherwise mutate motion.
- Telemetry distinguishes configured target, trajectory target, applied output, generated rate, and generated step count. `feedback_available=false` is published until a real feedback provider exists; no `actual_rpm` is fabricated.
- The legacy `argus/peristaltic/cmd/...` interface is not a compatibility path and cannot dispatch commands.

Sessions, sequences, connection IDs, client IDs, and topic policy provide lifecycle freshness and deterministic local ownership, not cryptographic identity. Phase 4D.1 defines independently revocable machine identities and per-client publish/subscribe permissions as future admission gates before the accepted Phase 4C contract. It does not implement those gates. Until MQTT TLS and credential handling are separately implemented and accepted, deployment remains limited to the approved protected local-network boundary; Wi-Fi encryption is not MQTT end-to-end transport security. The exact deployed protocol remains defined in `PHASE_4C_MQTT_CONTRACT.md`, and the future security boundary is defined in `PHASE_4D_SECURITY_CONTRACT.md`.

## Support-Friendly Design

MQTT should support field troubleshooting.
A support person should be able to:
1.	Identify the customer.
2.	Identify the unit.
3.	Open the unit topic folder.
4.	Locate the relevant device.
5.	See telemetry, state, alarms, config, and status.
If a topic structure makes support harder, it should be reconsidered.

## Example Unit Tree

Example for Paladin Skid 7:
argus/paladin/skid7/
telemetry/
pump1/
rpm
pressure
flow
engine1/
rpm
coolant_temp
ai_node1/
ch1/
raw
filtered
eng

state/
pump1/
mode
health
node1/
online
core/
system

command/
pump1/
start
stop
target_pct

status/
core/
wifi
mqtt
uptime_s
node1/
heartbeat

alarm/
pump1/
overpressure
engine1/
high_temp

config/
node1/
cmd
reply
state

metadata/
core/
firmware_version
serial_number
location

## Final Rule

A field technician should be able to describe a problem in plain language, and the MQTT structure should make it obvious where to look.
If someone says:
I'm with Paladin on Skid 7.
the support path should begin at:
argus/paladin/skid7/
MQTT should be organized around how the field actually works.
