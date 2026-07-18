# Development Rules

This document defines the development principles used throughout Argus projects.
These rules exist to improve reliability, maintainability, portability, and long-term project success.
When in doubt, follow these rules before introducing new complexity.

## Rule #1: Verify Before Concluding

The most dangerous assumptions are subconscious assumptions.
Before diagnosing a problem, identify:
- What is known
- What is believed
- What has been verified
Experience creates hypotheses.
Verification creates conclusions.
Evidence outranks experience.
Reality is authoritative.

## Rule #2: Never Troubleshoot Through Assumption

Assumptions are unverified variables.
Every assumption increases uncertainty.
Troubleshooting should systematically eliminate assumptions through observation and measurement.
May-haves are assumptions wearing a tuxedo.
They may sound careful, but they are still unverified until measured, observed, or reproduced.
Preferred workflow:
1.	Verify the symptom.
2.	Verify inputs.
3.	Verify outputs.
4.	Verify communication paths.
5.	Verify configuration.
6.	Isolate the fault.
7.	Implement the fix.
Never skip verification because something "should" be working.
Never promote a may-have into a conclusion.

## Rule #3: Deterministic Beats Clever

Predictable behavior is preferred over clever behavior.
Systems should be understandable.
Future developers should be able to determine:
- What the code does
- Why it does it
- When it does it
without reverse engineering hidden logic.
If a solution is difficult to explain, reconsider it.

## Rule #4: Simplicity Wins

Complexity must justify itself.
When multiple solutions solve the same problem:
Choose the simplest solution that meets requirements.
Simple systems are:
- Easier to test
- Easier to troubleshoot
- Easier to teach
- Easier to maintain
Complexity is a cost.
Spend it carefully.

## Rule #5: Explicit Beats Implicit

System behavior should be visible.
Avoid:
- Hidden dependencies
- Hidden state
- Hidden assumptions
- Hidden configuration
A developer should not need tribal knowledge to understand system behavior.

## Rule #6: Human Readable Always

Information intended for humans should be presented in human-readable form.
Examples:
Good:
- 700 RPM
- 85 PSI
- RUNNING
- HOLDING
Poor:
- scaled values
- encoded status values
- unexplained abbreviations
Operator understanding takes priority.

## Rule #7: State Matters More Than Data

Raw data is important.
State is actionable.
Whenever practical:
Communicate state before raw values.
Examples:
Better:
RUNNING
FAULT
OFFLINE
STARTING
Less useful:
Boolean flags without context.

## Rule #8: Backward Compatibility Matters

Field systems exist to perform work.
Changes should not unnecessarily break deployed equipment.
When changes are required:
- Preserve compatibility when practical
- Provide migration paths
- Document changes clearly
Operational reliability takes priority over architectural perfection.

## Rule #9: Design For Failure

Failures are expected.
Systems should anticipate:
- power loss
- network loss
- sensor failure
- controller restart
- communication interruption
The goal is graceful recovery.
Not perfect operation.

## Rule #10: Controllers Own Control

Control logic belongs within controllers.
User interfaces do not own machine state.
MQTT does not own machine state.
Node-RED does not own machine state.
Displays communicate.
Controllers control.
This boundary should remain clear.

## Rule #11: Safety Is Never A Convenience Feature

Safety systems should never be compromised for convenience.
Convenience features:
- may fail
Safety systems:
- must remain dependable
When convenience conflicts with safety:
Safety wins.
Every time.

## Rule #12: Build Observable Systems

A system should be capable of answering:
- Am I online?
- Am I healthy?
- What am I doing?
- Why am I doing it?
Use:
- heartbeats
- diagnostics
- status indicators
- fault reporting
Invisible systems are difficult to trust.

## Rule #13: Document Decisions

Document reasoning.
Not just outcomes.
Future developers should understand:
- Why a decision was made
- What alternatives were considered
- What assumptions existed
The reasoning behind a decision is often more valuable than the decision itself.

## Rule #14: Test Reality, Not Theory

A design is not validated because it works in theory.
A design is validated because it works in reality.
Documentation is not proof.
Simulation is not proof.
Assumption is not proof.
Field performance is proof.
Whenever possible:
Test against real hardware.
Test against real users.
Test against real operating conditions.

## Rule #15: Preserve Trust

Trust is difficult to earn and easy to lose.
Argus systems should:
- Tell the truth
- Report uncertainty honestly
- Communicate failures clearly
- Avoid misleading information
An honest fault is preferable to a misleading success.
Trust is a feature.
Protect it.

## Rule #16: Leave The Next Person Better Off

Every project should be easier to understand than when it was found.
Improve:
- documentation
- naming
- comments
- architecture
- consistency
The next developer may be:
- another engineer
- a contractor
- an AI assistant
- future you
Build accordingly.

## Final Rule

If a decision creates confusion, increases assumptions, hides system behavior, or makes troubleshooting harder, reconsider it.
Argus exists to improve understanding.
Development practices should do the same.
When in doubt, separate what is known, what is suspected, and what still needs proof.
