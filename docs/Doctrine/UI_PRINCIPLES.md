# UI Principles

This document defines the user interface philosophy used throughout Argus systems.
User interfaces exist to communicate understanding.
They do not exist to display as much information as possible.
The purpose of an Argus interface is to reduce cognitive load and improve decision-making.

## The Primary Objective

A good interface should answer:
- What is happening?
- Why is it happening?
- What should I pay attention to?
- What should I do next?
The user should not be forced to assemble these answers from raw values.
The interface should help communicate them directly.

## State Over Data

State is more valuable than raw data.
Examples:
Less useful:
RPM = 0
More useful:
HOLDING
Less useful:
Pump Command = TRUE
More useful:
STARTING
Less useful:
Pressure = 0
More useful:
PUMP OFFLINE
Whenever practical, interfaces should communicate state first and raw values second.

## Tell Me What Is Happening

Argus interfaces should explain the machine.
The operator should not need to infer system behavior.
Examples:
Good:
- RUNNING
- HOLDING
- ACCELERATING
- OFFLINE
- WAITING FOR FLOW
- E-STOP ACTIVE
Poor:
- Status = 4
- Output = TRUE
- Mode = 2
Machines should communicate intent clearly.

## Operators Glance, They Do Not Study

Industrial operators make decisions quickly.
Interfaces should prioritize:
- readability
- clarity
- hierarchy
- recognition
Important information should be visible within seconds.
If an operator must search for information, the design should be reconsidered.

## Reduce Cognitive Load

Every element on the screen should justify its existence.
Questions to ask:
- Does this help decision-making?
- Does this explain system state?
- Does this reduce confusion?
If not, it may not belong on the screen.

## Human Readable Always

Information should be presented in forms that make sense to humans.
Examples:
Good:
- 700 RPM
- 85 PSI
- 4.2 GPM
Poor:
- scaled values
- encoded status codes
- engineering shorthand without context
The goal is understanding.
Not technical precision at the expense of usability.

## Visual Hierarchy Matters

The most important information should be the most visible.
Priority should generally be:
1.	Safety conditions
2.	Machine state
3.	Active alarms
4.	Primary process values
5.	Secondary information
6.	Diagnostics
Everything should not compete equally for attention.

## Motion Has Meaning

Motion should communicate information.
Examples:
- heartbeat indicators
- communication activity
- active states
- transitions
Motion should not exist solely for decoration.
If an animation does not communicate information, it should be removed.

## Color Has Meaning

Color is communication.

Argus Edge brand colors are:
- black
- silver
- deep royal violet

The violet should read closer to a deep plum or royal purple than a bright blue-purple.

Customer branding is a first-class HMI requirement.
Oilfield customers care deeply about brand identity, and field-facing HMIs should respect that without requiring screen rewrites.
LVGL screens should pull colors from a named theme or palette boundary.
Client color changes should be intentional configuration work, not scattered edits across widgets.

Examples:
Green

- running
- healthy
- active
Blue

- informational
- connected
- neutral
Amber

- warning
- degraded operation
Red

- fault
- alarm
- emergency condition
Colors should remain consistent throughout the ecosystem.

## Consistency Creates Trust

Similar conditions should look similar.
Similar actions should behave similarly.
Users should not need to relearn interface behavior between products.
Consistency reduces training requirements and improves confidence.

## Interfaces Are Not Authority

User interfaces communicate.
Controllers control.
Interfaces may:
- display state
- display alarms
- accept commands
- communicate intent
Interfaces do not own machine state.
Interfaces do not own safety authority.
This separation is fundamental to Argus architecture.

## Explain Failure Clearly

Failures should be obvious.
Examples:
Good:
MQTT DISCONNECTED
NODE OFFLINE
FLOW SENSOR FAULT
Poor:
Data stops updating
Blank screen
Missing values
Users should never be left guessing whether a system is functioning.

## The Best Interface

The best interface is not the one with the most information.
The best interface is the one that answers the user's question before they ask it.
Every design decision should support one objective:
Help the user understand what is happening and confidently decide what to do next.
