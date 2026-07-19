# Phase 4B.2 — Pure-Test Isolation and Reset Truthfulness Closure

> **SUPERSEDED** — This document describes the state as of commit `815f417`.
> The reset-marker architecture described here (storing `rst_pend` in
> `ARGUS_NVS_NS_SYS`) was superseded by the durable reset-pending marker
> correction in commit `0abc6e5`, which moves the marker to a dedicated
> `ARGUS_NVS_NS_RST` namespace. The test orchestration was subsequently
> corrected to exercise the production-used core helpers.
>
> For current state, see:
> **Phase 4B.2 — Reset Transaction Orchestration Correction.md**

---

The original content below is preserved for historical reference only.

---

**Commit:** `815f417` on `phase4b-config-portal`  
**Parent:** `ac7f286` (production NVS seam wiring)  
**Build:** ESP-IDF 5.5.1 full-clean — 981,605 bytes (6% free in 1MB app partition)  
**Tests:** 70 distinct cases

## Issue 1: Production-Singleton Mutation from Test Functions

Tests T61–T68 and T70 called production singleton APIs, which mutated global
`s_custom_driver`. This was corrected by moving all tests to caller-owned
`argus_nvs_core_t` instances.

## Issue 2: Factory-Reset Marker Truthfulness

The factory reset wrote `rst_pend` into `ARGUS_NVS_NS_SYS`, but `prod_erase_all`
also erased that namespace. This left no recovery marker after power loss.

**This defect was corrected in commit `0abc6e5`.** See the current document
for the durable `ARGUS_NVS_NS_RST` architecture.
