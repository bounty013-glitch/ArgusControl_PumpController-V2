# Safety Model

This document defines the safety philosophy and safety boundaries used throughout Argus systems.
Safety is a separate concern from functionality.
A system may continue operating with reduced functionality.
A system must never compromise safety in order to preserve functionality.

## Core Principle

Safety must survive failure.
Failures are expected.
Safe operation must not depend on components that are expected to fail.
Examples:
- Networks fail.
- Wi-Fi fails.
- MQTT fails.
- Displays fail.
- Computers fail.
- Node-RED flows fail.
- Cloud services fail.
Safety systems must continue functioning when these failures occur.

## Safety Hierarchy

Argus follows a layered safety model.
Level 1: Physical Safety
Highest authority.
Examples:
- E-stop circuits
- Safety relays
- Hardwired interlocks
- Mechanical protections
- Physical disconnects
These systems operate independently of software.
Nothing in software may override physical safety systems.

## Level 2: Controller Safety

Machine-level protection.
Examples:
- Overpressure limits
- Overspeed limits
- Temperature limits
- Process interlocks
- Fault shutdown logic
These protections belong within the controller responsible for the equipment.
Controller safety must not depend on:
- Cloud connectivity
- MQTT availability
- Node-RED
- HMIs

## Level 3: Supervisory Systems

Coordination and awareness.
Examples:
- Node-RED
- ArgusCore
- Dashboards
- Remote monitoring
These systems may:
- Request actions
- Recommend actions
- Coordinate actions
They do not own safety authority.

## Level 4: Advisory Systems

Information and recommendations.
Examples:
- Alerting systems
- Analytics systems
- AI assistants
- Future Atlantis services
These systems provide awareness.
They do not directly control safety-critical equipment.

## Authority Rules

Authority must always be clear.
Examples:
Physical E-Stop
-> authoritative
Controller Safety Logic
-> authoritative
MQTT Command
-> request only
HMI Button
-> request only
Node-RED Flow
-> request only
Cloud Service
-> request only
Safety authority should never be ambiguous.

## Fail Safe vs Fail Operational

Argus uses both approaches depending on risk.
Fail Safe
When continued operation creates unacceptable risk.
Examples:
- Emergency stop
- Critical overpressure
- Critical overspeed
System action:
Stop operation.

## Fail Operational

When continued operation is acceptable and safe.
Examples:
- Telemetry loss
- Display failure
- Internet failure
- Cloud outage
System action:
Continue operation.
Report fault.
Maintain safe control.

## Safety Through Simplicity

Complex safety systems are difficult to verify.
Whenever practical:
- Prefer simple logic
- Prefer deterministic behavior
- Prefer explicit states
- Prefer physical protection over software protection
Complexity must justify itself.

## Safety Verification

Safety assumptions must be verified.
May-haves are assumptions wearing a tuxedo, and safety systems do not accept formal-looking guesses.
Never assume:
- A sensor is functioning.
- A relay is operating.
- An interlock is active.
- A safety circuit is wired correctly.
Verification is required.
Subconscious assumptions are particularly dangerous because they often go unnoticed.
The existence of a safety feature does not guarantee its operation.
A safety claim is not true because the design includes it.
It is true only when the real system proves it.

## Human Responsibility

Argus assists operators.
Argus does not remove operator responsibility.
Operators remain responsible for:
- Following procedures
- Understanding equipment limitations
- Responding appropriately to conditions
- Maintaining safe operation
Automation should improve decision-making, not eliminate accountability.

## AI Safety Boundary

Artificial intelligence may:
- Explain conditions
- Interpret trends
- Recommend actions
- Improve awareness
Artificial intelligence may not:
- Silently modify safety settings
- Override controller protections
- Override physical safety systems
- Assume control of safety-critical equipment
AI remains advisory.
Human approval remains required.

## Final Principle

A feature can fail.
A display can fail.
A network can fail.
A controller can fail.
Safety must remain.
Every safety decision should be evaluated against a single question:
"If this component disappeared right now, would the system remain safe?"
If the answer is no, the design should be reconsidered.
