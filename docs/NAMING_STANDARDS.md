# Naming Standards

This document defines naming conventions used throughout the Argus ecosystem.
Consistent naming improves:
- readability
- maintainability
- discoverability
- supportability
- documentation quality
Names should communicate purpose.
Names should remain stable.
Names should be understandable by both technical and non-technical users.

## Naming Philosophy

A name should answer:
"What is this thing?"
without requiring additional explanation.
Names should describe purpose rather than implementation.
Good names reduce cognitive load.

## Company Name

Company:
Argus Edge
Argus Edge is the company.
Argus Edge develops and maintains the Argus ecosystem.

## Platform Generations

Major platform generations use distinct names.
Current generation:
Argus Forge
Future ecosystem generation:
Argus Atlantis
These names represent platform generations and ecosystem evolution.
They are not individual products.

## Product Naming

Product names use:
Argus + ProductName
No spaces.
Examples:
ArgusCore
ArgusNode
ArgusNet
ArgusLink
ArgusControl
Future products should follow the same pattern whenever practical.

## Product Definitions

ArgusCore
Primary appliance hardware.
Provides local user interaction and system access.
Acts as the face of the system.

## ArgusNode

Distributed edge devices.
Responsible for:
- data acquisition
- control
- monitoring
- protocol integration
Nodes exist close to equipment.

## ArgusNet

Communications infrastructure.
Responsible for:
- networking
- connectivity
- transport
- communications services

## ArgusLink

Integration layer.
Responsible for:
- external systems
- third-party integrations
- interoperability

## ArgusControl

Operator interaction layer.
Responsible for:
- dashboards
- control interfaces
- visualization
- operator workflows

## Project Naming

Repositories should use clear project names.
Examples:
ArgusCore-P4
ArgusNode-AI
ArgusNode-J1939
ArgusControl-PumpHMI
ArgusLink-Modbus
Project names should describe function.
Avoid generic names.
Examples to avoid:
test
newproject
project2
firmware

## Device Naming

Devices should be named according to their field function.
Examples:
pump1
pump2
engine1
engine2
core
node1
flowmeter1
pressure1
ai_node1
j1939_node1
Names should match operational language whenever practical.

## Client Naming

Use names commonly used in operations.
Examples:
paladin
stealth
ironside
prime
Avoid:
customer01
client002
acct123

## Unit Naming

Unit names should match field terminology.
Examples:
skid7
skid4
plant11
coil1
pump301
Operators should immediately recognize the name.
If the field calls it Skid 7, the system should call it Skid 7.

## MQTT Naming

MQTT topics use:
lowercase only
Examples:
argus/paladin/skid7/telemetry/pump1/rpm
argus/paladin/skid7/state/core/system
MQTT names should be descriptive and human-readable.

## Firmware Naming

Firmware projects should follow:
Product-Function
Examples:
ArgusCore-P4
ArgusNode-AI
ArgusNode-J1939
ArgusControl-PumpHMI
Firmware names should indicate purpose.

## Document Naming

Documents use:
UPPERCASE_WITH_UNDERSCORES.md
Examples:
WHY.md
ARGUS_PHILOSOPHY.md
ARCHITECTURE_PRINCIPLES.md
MQTT_STANDARDS.md
CURRENT_STATE.md
DIRECTION.md
Consistent document naming improves navigation.

## Version Naming

Version format:
Major.Minor.Patch
Examples:
1.0.0
1.2.3
2.0.0
Meaning:
Major

- breaking change
Minor

- feature addition
Patch

- bug fix

## Reserved Terms

The following names are reserved for ecosystem-level concepts:
Argus Edge

- Company
Argus Forge

- First-generation platform
Argus Atlantis

- Future ecosystem platform
ArgusCore

- Appliance hardware
ArgusNode

- Distributed field devices
ArgusNet

- Communications layer
ArgusLink

- Integration layer
ArgusControl

- Operator interaction layer
These names should not be repurposed.

## Final Rule

Names should help people understand the system.
A name that requires explanation is usually a candidate for improvement.
The best names communicate purpose immediately.
