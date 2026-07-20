# Phase 4B.2 — Final Acceptance

**Branch:** `phase4b-config-portal`  
**Final Commit:** `e2ac940`  
**Physical Acceptance:** 2026-07-19 — Operator flashed, confirmed boot sequence correct  
**Build:** ESP-IDF 5.5.1 full-clean — 985,925 bytes (6% free in 1MB app partition)  
**Tests:** 78 distinct cases, 78 RUN_TEST invocations, 234 executions (3 passes)

---

## Phase 4B.2 Scope — Configuration Read/Write via HTTP Portal

Phase 4B.2 implemented the HTTP configuration portal for the ArgusControl Pump
Controller V2. The operator can provision identity (client ID, unit ID, device
name) via the always-available soft-AP web interface.

---

## Commit History

| Commit | Description |
|--------|-------------|
| `de66df2` | Initial HTTP portal: config read/write, coordinated restart |
| `2181055` | Atomic provisioning, restart safety, always-on AP, pure tests |
| `13e7d56` | Extract NVS seams, monotonic provisioning, pure test suite |
| `72c3d3d` | Final correctness closure: HWM monotonicity, dispatch gate, JSON parser |
| `ac7f286` | Wire production NVS to corrected core — eliminate duplicated algorithms |
| `815f417` | Purify T61-T70 test isolation, factory-reset truthfulness |
| `0abc6e5` | Durable reset-pending marker via dedicated NVS namespace |
| `658bd46` | Extract reset orchestration into testable core helpers |
| `e2ac940` | Selective mock injection for T73 factory-reset clear failure |

---

## Architecture Delivered

### HTTP Configuration Portal
- Always-available soft-AP (`ArgusConfig_XXXX`) with HTTP server
- JSON-validated configuration writes with field-level rejection
- Bounded HTTP body reception (configurable max, default 1KB)
- Staged configuration writes in `AP_DISCOVERABLE` state only
- Coordinated restart with HTTP response grace period

### NVS Configuration Manager
- Dual-slot (A/B) configuration storage with selector
- Monotonic high-water-mark (HWM) provisioning lock
- CRC32-protected payload integrity
- Identity lock: once provisioned, cannot be re-provisioned without factory reset

### Durable Factory Reset
- Reset-pending marker in dedicated `ARGUS_NVS_NS_RST` namespace
- Marker survives factory-reset erasure of CFG and SYS namespaces
- Boot-recovery: detects stale marker, re-erases, clears marker
- Power-loss safe: marker preserved on all failure paths

### Production-Tested Core Helpers
- `argus_nvs_core_factory_reset(core, drv)` — caller-owned transaction
- `argus_nvs_core_recovery_check(drv)` — caller-owned boot recovery
- Production wrappers delegate to the same helpers exercised by tests
- `s_initialized` truthful on all paths (true only on success)

### Selective Mock Infrastructure
- `reset_pend_set_error` / `reset_pend_clear_error` — independent injection
- `pend_set_calls`, `pend_clear_calls`, `erase_calls` — exact call ordering
- All injection fields and counters preserved across `mock_erase_all()`

---

## Test Suite Summary (78 Cases)

| Range | Area | Count |
|-------|------|-------|
| T1–T20 | Phase 4A: core NVS, slot management, CRC, selector | 19 |
| T21–T47 | Phase 4B.1: MQTT topics, telemetry formatting, safety | 27 |
| T48–T60 | Phase 4B.2: JSON parsing, HTTP body validation | 13 |
| T61–T67 | Phase 4B.2: NVS core init/commit/get, HWM, lock | 7 |
| T68–T70 | Phase 4B.2: Factory reset success/failure, second-commit lock | 3 |
| T71–T73 | Phase 4B.2: Factory-reset transaction durability (via helper) | 3 |
| T74–T78 | Phase 4B.2: Boot-recovery transaction durability (via helper) | 5 |
| **Total** | | **78** (× 3 passes = 234 executions) |

### Test Fidelity Guarantees
- All T68-T78 call production-used core helpers
- Zero direct driver calls in reset tests
- Zero production singleton calls in any test body
- Selective mock injection proves exact call ordering

---

## Verification Record

| # | Item | Evidence |
|---|------|---------|
| 1 | Full clean build | 1089 objects, zero errors, zero warnings |
| 2 | Binary size | 985,925 bytes (0xf0bc0), 6% free (62,528 bytes) |
| 3 | Test definitions | 78 distinct |
| 4 | RUN_TEST invocations | 78 |
| 5 | Singleton isolation | Zero production singleton calls in tests |
| 6 | Helper delegation | Production wrappers delegate to core helpers |
| 7 | `s_initialized` truthfulness | Set true exactly once (success path) |
| 8 | T73 helper call | Confirmed — calls `argus_nvs_core_factory_reset` |
| 9 | Direct driver calls in reset tests | Zero |
| 10 | `git diff --check` | Clean |
| 11 | Credentials | Clean |
| 12 | **Physical flash** | **PASSED — Operator confirmed 2026-07-19** |
| 13 | **Boot sequence** | **PASSED — Operator confirmed reboot sequence perfect** |

---

## Files Delivered

### Production Source
| File | Purpose |
|------|---------|
| `main/argus_nvs_config.h` | NVS core types, driver interface, helper declarations |
| `main/argus_nvs_config.c` | Dual-slot NVS, HWM lock, factory reset, boot recovery |
| `main/argus_http_server.c` | HTTP portal: config read/write, JSON validation |
| `main/argus_wifi.c` | Always-available AP, APSTA mode |
| `main/argus_tests_4a.c` | 78-case pure test suite with selective mock |

### Documentation
| File | Purpose |
|------|---------|
| `docs/Phase 4B.2 — Final Acceptance.md` | This document |
| `docs/Phase 4B.2 — Reset Transaction Orchestration Correction.md` | Current architecture |
| `docs/Phase 4B.2 — Durable Reset-Pending Marker Correction.md` | Marker architecture |
| `docs/Phase 4B.2 — Pure-Test Isolation and Reset Truthfulness Closure.md` | Superseded |

---

## Phase 4B.2 Status: **ACCEPTED**

All production source, test, and documentation corrections are complete.
Physical flash and operator acceptance are confirmed.
Phase 4B.2 is closed.
