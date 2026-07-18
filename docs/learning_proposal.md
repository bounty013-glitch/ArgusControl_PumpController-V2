# Learning Proposal: Verification Integrity & Communication Guardrails

## Overview

Based on recent user feedback and project doctrine, this proposal defines a strict communication constraint regarding test claims and physical verification status.

## Identified Behavioral Rule

### Key Invariant
**Never claim or promise test pass outcomes prior to physical execution on-device.**

- **Rationale**: An AI assistant cannot know the physical outcome of hardware/firmware execution prior to actual execution on the physical MCU. Stating that tests "will pass" creates false expectations and violates verification doctrine.
- **Rule Scope**: ArgusCore Project-Scoped Rule (`c:\Users\bount\Dev\Argus\.agents\AGENTS.md`).

---

## Proposed Changes to [AGENTS.md](file:///c:/Users/bount/Dev/Argus/.agents/AGENTS.md)

Append the following section to `c:\Users\bount\Dev\Argus\.agents\AGENTS.md`:

```markdown
---

## 5. Verification Integrity & Communication Guardrails
* **No Premature Test Pass Claims:** Never state or guarantee that tests "will pass" or that code is "guaranteed to work" before physical on-device execution. Only state that code has compiled cleanly, builds pass static analysis, or is ready for user flash and physical verification.
* **Strict Verification Boundaries:** Runtime verification outcomes belong exclusively to the operator's physical test log. Claiming test completion prior to user execution is strictly forbidden.
```

---

## Requested Feedback

Please review this proposed addition to `.agents/AGENTS.md`. Upon approval, I will apply the change to the project rules file.
