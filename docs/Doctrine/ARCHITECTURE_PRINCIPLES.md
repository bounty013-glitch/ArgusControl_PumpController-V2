# Architecture Principles

This document defines the architectural patterns used throughout Argus systems.
These principles exist to improve reliability, maintainability, scalability, and operator trust.
Specific technologies may change over time.
These principles should remain consistent.

## Layered Responsibility

Every component should have a clearly defined responsibility.
Argus systems are built from layers.
Each layer performs a specific role and should not assume responsibility for another layer.

## Control Layer

Purpose:
Execute machine logic.
Examples:
- Pump controllers
- Engine controllers
- PLCs
- ESP32 field controllers
- Safety devices
Responsibilities:
- Execute deterministic control logic
- Maintain machine state
- Enforce safety rules
- Continue operation during network failures
Control systems are authoritative.

## Interface Layer

Purpose:
Communicate state and receive operator intent.
Examples:
- ArgusCore displays
- ArgusControl HMIs
- Local touchscreens
- Mobile dashboards
Responsibilities:
- Display machine state
- Present alarms and events
- Accept operator commands
- Communicate intent
Interfaces are not authoritative.

## Orchestration Layer

Purpose:
Coordinate systems.
Examples:
- Node-RED
- Workflow engines
- Data processing services
Responsibilities:
- Data aggregation
- Workflow coordination
- Cross-system communication
- Business logic
Orchestration should not own machine safety.

## Communication Layer

Purpose:
Move information.
Examples:
- MQTT
- Modbus
- CAN/J1939
- Ethernet
- Wi-Fi
Responsibilities:
- Reliable transport
- State distribution
- Command distribution
Communication layers should remain transport-focused whenever possible.

## Data Layer

Purpose:
Store and retrieve information.
Examples:
- Databases
- Historical logging
- Configuration storage
- NVS
Responsibilities:
- Persistence
- History
- Configuration
Loss of historical data should not stop equipment from operating.

## Edge First

Argus favors edge execution.
Logic should execute as close to the equipment as practical.
Benefits:
- Reduced latency
- Increased reliability
- Reduced dependency on networks
- Improved fault tolerance
Cloud and remote systems should enhance operation, not enable operation.

## Graceful Degradation

Every subsystem should have a failure mode.
The preferred failure mode is degradation, not collapse.
Examples:
Good:
- Loss of telemetry while machine continues operating
- Loss of dashboard while controller continues operating
- Loss of internet while local control remains available
Bad:
- Loss of display stops machine
- Loss of network stops machine
- Loss of cloud prevents operation

## Separation of Authority

Authority must be clearly defined.
Examples:
Controller:
- owns machine state
HMI:
- displays machine state
Operator:
- provides intent
MQTT:
- transports messages
Confusion occurs when multiple systems believe they own the same decision.
Ownership should always be explicit.

## Observable Systems

Systems should expose their state.
A system should be able to answer:
- Am I online?
- Am I healthy?
- What am I doing?
- Why am I doing it?
Health indicators, heartbeats, status messages, and diagnostics are encouraged.
Hidden state creates unnecessary troubleshooting complexity.

## Backward Compatibility

Operational systems should not be broken without justification.
When change is required:
- Provide transition paths
- Preserve compatibility when practical
- Communicate changes clearly
Field reliability takes priority over architectural purity.

## Assumption-Free Troubleshooting

Assumptions are unverified variables.
Troubleshooting should eliminate assumptions through measurement and verification.
May-haves are assumptions wearing a tuxedo.
A may-have should be treated as a hypothesis, not a finding.
Preferred process:
1.	Verify the symptom.
2.	Verify each layer independently.
3.	Confirm data flow.
4.	Confirm control flow.
5.	Isolate the failure.
6.	Implement the fix.
Never assume a device, network, sensor, controller, or configuration is behaving correctly without verification.
If a behavior is only suspected, keep it labeled as suspected until the system proves it.
The fastest path to a solution is usually the most methodical path.

## Long-Term Direction

Argus architecture is intended to evolve toward Atlantis.
Atlantis is not a single product.
Atlantis is the ecosystem formed by:
- ArgusCore
- ArgusNode
- ArgusNet
- ArgusLink
- ArgusControl
- Future services and integrations
The objective is a system that remains understandable, resilient, and deployable at scale.
Every architectural decision should support that direction.
