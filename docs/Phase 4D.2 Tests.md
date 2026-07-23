# Phase 4D.2 Tests and Acceptance Record

**Status:** ACCEPTED

**Date:** July 22, 2026

**Firmware:** `v2-phase4d.2-dev`

**Branch:** `phase4d2-security-storage-and-recovery`

## 1. Starting State

Work began from clean synchronized `main` at `b454fbc4718411320c88b54d810c8555431248b1`. Annotated tag `v2-phase4d.1-security-contract-accepted` resolved to the same commit. The ESP32-S3 was verified on COM5, commissioned identity and network state were captured without credentials, and the motor was physically absent.

Identity was established first in `e523243` (`chore: establish Phase 4D.2 identity`). The Client Admin and recovery decisions were recorded in `6e0a81d` before functional implementation.

## 2. Storage, Schema, and Partition Evidence

The security store uses explicit schema/version fields, bounded counts and strings, generation, payload length, CRC32, final-valid marker, dual slots, and an authoritative selector. Missing, corrupt, unsupported, and unavailable states remain distinct and fail closed.

Compiled record sizes:

| Record | Bytes |
|---|---:|
| Role | 57 |
| Human account | 238 |
| Machine client | 402 |
| Manifest payload | 562 |
| Complete slot | 582 |

Maximum populations are 16 roles, 16 human accounts, and 16 machine clients. The conservative two-generation raw-record calculation is 22,744 bytes before NVS page, wear, and future-schema reserves.

The accepted 16 MiB layout appends:

| Partition | Offset | Size |
|---|---:|---:|
| `sec_keys` | `0x660000` | `0x1000` |
| `sec_store` | `0x661000` | `0x40000` |
| `sec_audit` | `0x6a1000` | `0x40000` |

Both 3 MiB OTA slots retain their prior offsets and sizes. The layout ends at `0x6e1000` and leaves `0x91f000` (9,564,160 bytes) unused.

## 3. Storage and Migration Tests

The controller suite covers blank initialization, atomic dual-slot commit and readback, interrupted writes, selector-write failure, first-selector failure, corrupt-slot fallback, unsupported schema, malformed records and populations, and bounded capacity. Corruption never triggers whole-NVS erase.

Migration tests cover absent bootstrap, changed legacy plaintext, malformed/unavailable legacy data, commit/readback ordering, plaintext deletion only after proof, interruption at each stage, retry idempotence, and truthful migration states. The compiled bootstrap console case remains explicitly deferred; no credential was guessed or silently changed.

## 4. Verifier and Provisioning

The verifier is PBKDF2-HMAC-SHA-256 with a 16-byte random salt, 32-byte result, bounded 10,000 through 500,000 iterations, and provisional default 25,000. Comparison uses `mbedtls_ct_memcmp()`. Work is serialized through a dedicated 6,144-byte worker task and yields cooperatively every 256 rounds.

Final ESP32-S3 benchmark:

| Iterations | Elapsed | Free heap before/after |
|---:|---:|---:|
| 10,000 | 825 ms | 123,980 / 123,996 |
| 25,000 | 2,054 ms | 123,996 / 124,004 |
| 50,000 | 4,108 ms | 124,004 / 124,004 |
| 100,000 | 8,215 ms | 124,004 / 123,996 |

Worker stack high-water margin was 4,764 bytes. The final run produced no watchdog, panic, reset, heap, or stack fault.

Known-answer tests cover the standard one-round SHA-256 vector and a 513-round vector that also proves two cooperative pauses. Create/verify, wrong-password, unique-salt, malformed-record, and iteration-bound tests passed.

`tools/provision_phase4d2.py --synthetic-test` generated encrypted development artifacts, decrypted them, and verified readback using synthetic values only. An initial invocation without the ESP-IDF environment failed closed; rerunning under ESP-IDF v5.5.3 passed. No real credential appeared in arguments, logs, tracked files, or repository changes.

## 5. GPIO0 and Recovery Evidence

Source and schematic review identified KEY1/BOOT as active-low GPIO0 with pull-up; RESET is KEY2. The runtime detector requires a post-boot released state, 100 ms debounce, a continuous 10-second hold, and debounced release. Pure tests passed for startup-low rejection, short press, bounce, qualifying hold/release, one-shot behavior, persistence-before-post ordering, and failure propagation.

Physical observations:

- A confirmed short BOOT press below one second caused no recovery transition.
- Two early attempts mistakenly held RESET. USB disconnected while held and returned after release; these attempts exercised no firmware recovery logic and were not counted as KEY1 tests.
- A 12-second BOOT hold was detected after release and the persistent marker survived reboot.
- Recovery booted directly into `SECURITY_RECOVERY_AP_ONLY`.
- A repeated valid BOOT hold originally exposed an idempotence defect and entered `NETWORK_FAULT`.
- After correction, the repeated hold logged `Physical security recovery already active`, remained AP-only, and produced no fault.
- Diagnostic-only `K` cleanup was accepted only while stationary and safe, cleared the marker, and rebooted to `AP_DISCOVERABLE`.

Throughout recovery and cleanup the controller remained `UNLOCKED` with configured, trajectory, applied, and generated speed zero; generated steps zero; driver disabled; E-stop clear; fault zero; and authority `NONE/NONE`. No command-router dispatch occurred. Commissioned configuration remained LKG slot 0 generation 2. Customer identity, STA configuration, security records, calibration, users, and machine-client records were not erased or rewritten.

The unchanged WPA2 profile reconnected to `Argus-Service-6EC2D0`; `192.168.4.1` replied to both pings. Unauthenticated `/` and `/api/status` requests reached the portal and returned bounded HTTP 401 challenges.

## 6. Failure and Correction Chronology

1. A strict build orchestration timeout left an earlier Ninja child active; a second build collided while linking. A final single-process full-clean, no-ccache build replaced this evidence.
2. The first implementation boot produced `LoadProhibited` in `argus_security_store` writer task. A pointer-sized queue had received the first bytes of a stack request instead of a pointer to it. Commit `4f9a2c3` enqueued the actual request pointer and added a pointer-width guard.
3. The first 100,000-round KDF benchmark triggered TWDT because the monolithic priority-3 PBKDF2 call starved the idle task. Commit `c279112` implemented the equivalent cooperative HMAC-SHA-256 loop with a one-tick pause every 256 rounds.
4. The first post-correction suite found that the original one-round expected vector was malformed beginning at byte 22. The standard vector and a second independently generated 513-round vector now pass.
5. One otherwise clean 660/660 suite run failed isolation when autonomous STA retry reported `NO_AP_FOUND` and advanced authority generation. This was preserved as external production churn; the isolation check was not weakened.
6. The first correct BOOT hold revealed that a repeated request while persistent recovery was already active attempted Wi-Fi teardown again and entered `NETWORK_FAULT`. Commit `c279112` made active recovery idempotent and avoided redundant marker writes.

## 7. Final Validation

Host validation:

- Phase 4B.5 controls JavaScript: PASS
- Phase 4B.6 portal JavaScript: PASS
- Synthetic encrypted provisioning/readback: PASS
- `git diff --check`: PASS
- Production MQTT router dispatch: exactly one site in `argus_mqtt_runtime.c`
- Legacy MQTT command topics in production: none
- New direct motion/state path: none
- Potential secret assignments in the final diff: zero

Final controller validation used genuine Windows ConPTY-backed `idf.py monitor`:

| Invocation | Distinct | Internal passes | Executed | Passed | Failed | Isolation |
|---|---:|---:|---:|---:|---:|---|
| 1 | 220 | 3 | 660 | 660 | 0 | PASS |
| 2 | 220 | 3 | 660 | 660 | 0 | PASS |
| 3 | 220 | 3 | 660 | 660 | 0 | PASS |

Total final executions were 1,980/1,980. Authority generation, network mode, broker state, machine state, and task count were unchanged in every accepted invocation. There was no panic, unexpected reset, watchdog, brownout, assertion, stack-canary failure, heap corruption, task leak, or production-state contamination.

Final ESP-IDF v5.5.3 build:

- Command: `idf.py fullclean`, `idf.py --no-ccache reconfigure`, `ninja -C build -j 1 all`
- Ccache: disabled
- Compiler warnings: 0
- Compiler errors: 0
- Binary: `0x1146d0` (1,132,240 bytes)
- OTA headroom: `0x1eb930` (2,013,488 bytes, 64%)
- Static D/IRAM: 182,071 bytes used, 159,689 bytes free
- Phase 4D.1 delta: +24,704 binary bytes; +1,488 static D/IRAM bytes

## 8. Acceptance Boundary

Phase 4D.2 security storage, credential-verifier/provisioning foundation, migration behavior, and physically local network recovery are accepted. The NVS XTS key remains physically extractable because no eFuse, HMAC-derived key, flash encryption, or secure boot was enabled. No irreversible device-security action occurred.

Browser sessions, `/login`, user administration, role enforcement middleware, machine enrollment, MQTT authentication, credential rotation UI, recovery authorization UI, HTTPS, TLS, certificates, hostile-network acceptance, and penetration testing remain Phase 4D.3 or later work. No powered motion, pump, process, or mechanical acceptance is implied.
