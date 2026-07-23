# Phase 4D.2 - Security Storage, Credential Foundation, and Local Recovery

**Status:** COMPLETE AND ACCEPTED - July 22, 2026

**Branch:** `phase4d2-security-storage-and-recovery`

**Accepted baseline:** `b454fbc4718411320c88b54d810c8555431248b1`

**Accepted baseline tag:** `v2-phase4d.1-security-contract-accepted`

**Firmware identity:** `v2-phase4d.2-dev`

## 1. Scope

Phase 4D.2 implements storage, verifier, migration, provisioning, and physically local network-recovery foundations. It preserves Phase 4C motion, MQTT, authority, and fail-operational behavior. It does not implement browser sessions, `/login`, user administration, role-shaped pages, general authorization middleware, machine enrollment, MQTT authentication, AP-password administration, HTTPS, TLS, or certificates.

## 2. Identity-First Baseline

The clean synchronized Phase 4D.1 merge and annotated tag were verified before branching. Commit `e523243` (`chore: establish Phase 4D.2 identity`) changed only the project version, fallback identity, startup labels, and pure-suite labels. The stale final Phase 4C suite banner was reconciled in the same identity-only commit.

## 3. Client Admin Peer Management

The stable capability identifier is `manage_client_admins` (`ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS` in firmware metadata).

- Ordinary Client Admin administration is downward-only.
- All Client Admins may manage permitted Supervisor, Operator, and Viewer accounts beneath their ceiling.
- Client Admin peer creation, promotion, modification, disablement, or deletion requires `manage_client_admins`.
- Argus Personnel possess the capability and may explicitly assign or revoke it for an individual Client Admin.
- The capability is protected and non-delegable by Client Admins; it cannot be copied into a custom role or inherited merely because its holder possesses it.
- It never reaches, modifies, impersonates, demotes, deletes, or delegates Argus Personnel access.

## 4. Security Module Ownership

| Module | Ownership |
|---|---|
| `argus_security_store` | Encrypted partition initialization, versioned schemas, built-in permission metadata, AP-secret domains, console verifier, security epoch, migration state, recovery marker, atomic dual-slot commits, sanitized status |
| `argus_password_verifier` | PBKDF2-HMAC-SHA-256 records, random salts, bounded cost/input policy, constant-time verification, and dedicated KDF worker task |
| `argus_local_recovery` | GPIO0/KEY1 input policy, debounce/hold/release detector, persistent recovery request, network-manager handoff, and diagnostic-only safe cleanup seam |
| `argus_net_mgr` | Application of the explicit AP-only security-recovery network state without command dispatch or machine-state mutation |

No production security path calls the command router, state manager mutation APIs, trajectory, step generator, or motion GPIOs. Diagnostic-only recovery cleanup reads a state snapshot to prove the controller is stationary before clearing the recovery marker.

## 5. Schemas and Limits

All persisted records use explicit format and schema versions, fixed bounds, generation numbers, payload lengths, CRC32, and a final valid marker. Current logical maxima are:

| Record class | Maximum |
|---|---:|
| Built-in plus custom roles | 16 |
| Human accounts | 16 |
| Machine clients | 16 |
| AP secret domains | 2: factory and active |
| Console verifier | 1 |
| Browser sessions | 8 RAM-only; not implemented in Phase 4D.2 |

Identifiers are bounded to 36 characters, login names to 32, display names to 48, AP secrets to 63 bytes, verifier salts to 16 bytes, and verifier outputs to 32 bytes. Missing, unprovisioned, corrupt, unsupported-version, and unavailable states remain distinct. Malformed counts, enums, flags, lengths, identifiers, role ceilings, credential records, and recovery states fail closed.

## 6. Partition and Capacity Plan

The accepted 16 MiB layout ends at `0x660000`. Phase 4D.2 appends aligned partitions without moving NVS, OTA data, either 3 MiB OTA slot, or the coredump:

| Partition | Offset | Size | Purpose |
|---|---:|---:|---|
| `sec_keys` | `0x660000` | `0x1000` | Software-generated NVS XTS key material |
| `sec_store` | `0x661000` | `0x40000` | Versioned encrypted security records |
| `sec_audit` | `0x6a1000` | `0x40000` | Reserved bounded audit storage for later implementation |

The new layout ends at `0x6e1000`, leaving `0x91f000` (9,564,160 bytes) unused. All additions are 4 KiB aligned. The 3 MiB OTA slots remain at `0x20000` and `0x320000`.

The security partition reserves 256 KiB. Compiled role, human, machine, manifest, and slot records are 57, 238, 402, 562, and 582 bytes. The conservative dual-generation raw-record calculation is 22,744 bytes before NVS page, wear, and future-schema reserves.

## 7. NVS Encryption Decision

ESP-IDF v5.5.3 provides `nvs_flash_generate_keys()`, `nvs_flash_read_security_cfg()`, and `nvs_flash_secure_init_partition()`. Phase 4D.2 uses those APIs explicitly for `sec_store`; it does not enable encryption implicitly for the legacy default NVS partition.

On a blank `sec_keys` partition, the controller generates an XTS key pair using ESP-IDF randomness, stores it in `sec_keys`, initializes `sec_store` with that configuration, and zeroizes the RAM copy after initialization. Existing keys are read and reused. Corrupt or unavailable keys fail closed. The security partition is never automatically erased for version, capacity, or corruption errors.

This arrangement is non-destructive and burns no eFuse, but the key partition is readable through physical flash access because flash encryption and HMAC/eFuse key derivation remain disabled. It protects against casual plaintext NVS inspection; it does not establish physical-extraction resistance. The logical schemas remain compatible with later HMAC/eFuse-backed NVS keys, flash encryption, and secure boot under a controlled manufacturing process.

## 8. Password Verifier

The verifier format uses PBKDF2-HMAC-SHA-256, a unique 16-byte random salt, a fixed 32-byte verifier, explicit format/algorithm identifiers, and an explicit uint32 iteration count. Accepted passwords are 1 through 64 bytes for the primitive; product-level password policy remains a later authorization/UI concern. Empty, oversized, malformed, unsupported-algorithm, and out-of-range-cost inputs fail closed.

The accepted cost range is 10,000 through 500,000 iterations, with a provisional 25,000 default. Final ESP32-S3 measurements were 825 ms, 2,054 ms, 4,108 ms, and 8,215 ms at 10k, 25k, 50k, and 100k. Production derivation and verification execute only on a dedicated bounded worker task, yield one tick every 256 rounds to preserve watchdog health, and use `mbedtls_ct_memcmp()` for result comparison. The worker holds no NVS, HTTP, MQTT, authority, state, or motion lock during KDF work.

## 9. Provisioning Flow

The store exposes one atomic provisioning transaction with separate factory AP, active AP, and console-verifier fields. It refuses missing/malformed values and refuses overwrite of an already provisioned production domain unless a separately explicit future rotation path is used.

For the current migration image, the build-provisioned AP credential may initialize both AP domains once without changing its value. A changed legacy portal password may be converted to a verifier and removed only after encrypted-store readback succeeds. A compiled bootstrap console value is not silently converted into a new authoritative verifier; that case remains staged for explicit non-echoing factory or operator provisioning.

The repository provides a local provisioning helper that accepts secrets only through non-echoing interactive input or an ignored restricted file, validates them, emits only deterministic non-secret status, and cleans temporary plaintext material. Synthetic values alone are used in automated tests. Real values are never required in ChatGPT or command-line arguments.

## 10. Legacy Migration State Machine

1. Inspect only presence and marker metadata for the legacy portal record.
2. Distinguish absent bootstrap, changed plaintext, malformed, and unavailable states without logging values.
3. For a valid changed record, derive the console verifier on the KDF worker.
4. Commit and read back the new encrypted security record atomically.
5. Mark migration complete only after validation.
6. Switch Basic-Auth compatibility verification to the verifier seam.
7. Delete legacy plaintext only after the authoritative record is proven.
8. On interruption, repeat safely; a proven new record wins, otherwise the untouched legacy record remains.

The build-provisioned AP value is copied once into distinct factory and active records without rotation. Existing STA credentials remain in the accepted legacy configuration schema during this bounded subphase; moving them requires coordinated configuration API migration. `WIFI_STORAGE_RAM` prevents ESP-IDF from silently recreating additional Wi-Fi NVS copies. Any prior driver-owned copies may remain and are documented rather than blindly erased.

## 11. GPIO0/KEY1 Recovery Policy

The Waveshare ESP32-S3-RS485-CAN schematic identifies KEY1 as active-low GPIO0 with a 10 kOhm pull-up to 3.3 V. KEY2 is reset. Repository inspection confirms GPIO0 is unused; GPIO3, GPIO4, and GPIO5 remain motion outputs.

The running application configures GPIO0 as input with pull-up after normal boot. The detector must first observe a debounced released/high state. It samples in a low-priority task, requires 100 ms stable debounce and a 10-second continuous low hold, and triggers exactly once only after a debounced release. Short presses, startup-low, and bounce do nothing. Operators must never hold KEY1 while applying power or resetting because GPIO0 is a ROM strapping pin.

## 12. Recovery State and Cleanup

A qualifying release transactionally persists `REQUESTED`, then posts one security-recovery event to the network lifecycle owner. The network manager enters explicit `SECURITY_RECOVERY_AP_ONLY`, uses the separately stored factory AP secret, disables STA/retry activity, and preserves authority and every machine value. A repeated request while recovery is active is idempotent and does not repeat Wi-Fi teardown. Recovery performs no router dispatch and does not invoke factory reset.

The marker survives reboot, so a boot in recovery starts directly in the same AP-only state. Later Phase 4D.3 authenticated Argus Personnel action will clear the marker. For Phase 4D.2 physical validation, a diagnostic-build-only cleanup requires a stationary, E-stop-clear, fault-clear controller, clears only the recovery marker, and performs a controlled reboot to reconstruct the prior network disposition from unchanged commissioned configuration.

## 13. Task and Lock Boundaries

- Security NVS writes have one worker/owner and publish the new in-RAM snapshot only after durable readback and selector commit.
- No security metadata mutex is held across NVS, network, HTTP, MQTT, authority, state, or motion calls.
- KDF work has a dedicated bounded worker and no cross-layer locks.
- GPIO sampling runs in a low-priority task, never an ISR or motion task.
- Recovery persistence completes before the network event is posted.
- The network manager remains the sole owner of Wi-Fi lifecycle mutation.

## 14. Phase 4D.3 Seams

Phase 4D.3 will consume verifier lookup, security epoch, user/role schemas, session revocation metadata, `manage_client_admins`, recovery-exit authorization, and migration status. It will replace temporary Basic-Auth compatibility with `/login`, bounded sessions, CSRF enforcement, and operation-boundary authorization.

## 15. Explicit Exclusions

No complete browser authentication, browser sessions, user/role CRUD API, machine enrollment, MQTT CONNECT authentication, AP-password UI, HTTPS, TLS, certificate infrastructure, eFuse burn, secure boot, irreversible flash-encryption mode, UART/JTAG restriction, motion command, pump/process test, or Phase 4D.3 implementation is authorized or accepted here.

## 16. Acceptance Evidence

ESP-IDF v5.5.3 produced a zero-warning, zero-error full-clean no-ccache build. The final image is 1,132,240 bytes with 2,013,488 bytes (64%) OTA headroom and 182,071 bytes static D/IRAM. Three genuine ConPTY controller invocations passed 660/660 each with complete production isolation. GPIO0 short-press, long-hold, persistence, idempotence, AP reachability, and controlled cleanup were physically validated with the motor absent. See `Phase 4D.2 Tests.md`.
