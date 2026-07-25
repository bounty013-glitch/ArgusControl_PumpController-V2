# Argus Philosophy

This document defines the core beliefs that guide the design of all Argus systems.
These principles apply regardless of hardware platform, software stack, communication method, or deployment environment.
Technologies may change.
Tools may change.
These principles should remain stable.

## 1. UI Is Never Authoritative

Displays can fail.
Networks can fail.
Browsers can fail.
Touchscreens can fail.
Control logic must never depend on the existence of a user interface.
The purpose of a user interface is to communicate system state and operator intent.
The purpose of a controller is to execute logic.
These responsibilities must remain separate.

## 2. Control Lives Below The UI

The closer a decision is to physical equipment, the closer the logic should be to that equipment.
Critical decisions belong at the edge.
Controllers control.
Interfaces communicate.
Cloud systems coordinate.
This separation improves reliability, safety, and fault tolerance.

## 3. Safety Must Survive Failure

Safety systems must not depend on:
- MQTT
- Wi-Fi
- Cellular networks
- Cloud services
- User interfaces
- Node-RED
- External databases
Emergency-stop functions and critical protections belong in deterministic control paths.
A failure elsewhere in the system must never remove a safety function.

## 4. Uptime Is Sacred

Industrial systems exist to perform work.
A system that cannot tolerate failure is not reliable.
Argus prioritizes:
- graceful degradation
- recoverability
- fault tolerance
- clear failure behavior
The goal is not perfection.
The goal is continued operation whenever possible.

## 5. Human Readable Always

People should not be required to mentally decode information.
Values should be presented in forms that make sense to operators and technicians.
Examples:
Good:
- 700 RPM
- 85 PSI
- 4.2 GPM
Bad:
- 7.00 x100 RPM
- scaled engineering values without context
- encoded status values
Human understanding takes priority over technical elegance.

## 6. Systems Should Explain Themselves

The purpose of a system is not to display data.
The purpose of a system is to communicate meaning.
A good interface answers:
- What is happening?
- Why is it happening?
- What should I pay attention to?
Whenever possible, state and intent should be visible.
Operators should not be required to infer system behavior from raw data.

## 7. State Is More Valuable Than Raw Data

Raw data is important.
State is actionable.
Examples:
Less useful:
- RPM = 0
More useful:
- HOLDING
Less useful:
- Pump command = true
More useful:
- STARTING
Argus systems should communicate state whenever possible.

## 8. Fail Forward

Failure is inevitable.
Every failure should produce learning.
Every lesson should improve the next version.
The objective is not avoiding failure.
The objective is improving through failure.
Version history should reflect accumulated understanding.

## 9. Simplicity Beats Cleverness

Complexity must justify itself.
Simple systems are:
- easier to troubleshoot
- easier to teach
- easier to maintain
- easier to trust
When two solutions solve the same problem, the simpler solution is preferred.

## 10. Explicit Beats Implicit

Hidden behavior creates confusion.
System behavior should be understandable.
Configuration should be visible.
Commands should be understandable.
Failure modes should be obvious.
Operators and developers should not need to guess what a system is doing.

## 11. AI Is Advisory Only

Artificial intelligence can assist.
Artificial intelligence can interpret.
Artificial intelligence can recommend.
Artificial intelligence must not silently control critical industrial processes.
Humans remain responsible for operational decisions.
The role of AI is to improve awareness, not replace accountability.

## 12. Preserve Trust

Trust is difficult to earn and easy to lose.
Argus systems should:
- tell the truth
- report uncertainty honestly
- avoid misleading information
- clearly communicate failures
An honest fault is preferable to a misleading success.
Trust is a feature.
Protect it accordingly.

## 13. May-Haves Are Not Truth

May-haves are assumptions wearing a tuxedo.
They may look disciplined because they sound cautious, but they are still not evidence.
Argus systems should distinguish:
- known facts
- suspected causes
- unverified hypotheses
- proven behavior
Uncertainty is acceptable.
Pretending uncertainty has been resolved is not.

## Final Principle # 1

Every design decision should support one question:
"Does this help the user understand what is happening and confidently decide what to do next?"
If the answer is no, the decision should be reconsidered.

## Final Principle # 2

The most dangerous assumptions are subconscious assumptions because they are invisible to the person making them.
