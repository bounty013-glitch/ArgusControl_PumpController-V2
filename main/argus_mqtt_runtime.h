#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "argus_mqtt_broker.h"
#include "argus_mqtt_contract.h"

esp_err_t argus_mqtt_runtime_init(void);
esp_err_t argus_mqtt_runtime_prepare_start(void);
void argus_mqtt_runtime_get_broker_config(uint16_t port,
                                          argus_mqtt_broker_config_t *out);
esp_err_t argus_mqtt_runtime_broker_started(void);
// Anchors the bounded ARGUSCORE acquisition window to the moment
// acquisition is operationally possible, rather than to MCU uptime.
// Idempotent-safe: does not reset the window if an owner already holds
// a live lease, so a broker restart cannot hand control to someone else.
void argus_mqtt_runtime_mark_authority_ready(void);
void argus_mqtt_runtime_tick(void);
esp_err_t argus_mqtt_runtime_get_session(char *out, size_t out_size);

// Correction order §7. What stage of admission an authority request/release
// reached. Named for the OUTCOME, not the gate, so a test asserts "this is
// what happened" rather than reproducing the gate's internal control flow.
typedef enum {
    ARGUS_MQTT_AUTHORITY_ADMIT_RETAINED_REFUSED = 0,
    ARGUS_MQTT_AUTHORITY_ADMIT_QOS_REFUSED,
    ARGUS_MQTT_AUTHORITY_ADMIT_DECODE_REJECTED,
    ARGUS_MQTT_AUTHORITY_ADMIT_SESSION_MISMATCH,
    ARGUS_MQTT_AUTHORITY_ADMIT_DUPLICATE_REPLAY,
    ARGUS_MQTT_AUTHORITY_ADMIT_DUPLICATE_CONFLICT,
    ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_STALE_EPOCH,
    ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_NOT_OWNER,
    ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_ACCEPTED,
    // Final-focused-pass item 1: an ACQUISITION request naming a nonzero
    // epoch that does not match the controller's current epoch. Symmetric
    // with ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_STALE_EPOCH, which already
    // existed for releases only - acquisitions had no equivalent check at
    // all before this pass.
    ARGUS_MQTT_AUTHORITY_ADMIT_REQUEST_STALE_EPOCH,
    ARGUS_MQTT_AUTHORITY_ADMIT_REQUEST_EVALUATED,
} argus_mqtt_authority_admit_stage_t;

typedef struct {
    argus_mqtt_authority_admit_stage_t stage;
    // Meaningful only when stage == DECODE_REJECTED.
    argus_mqtt_decode_result_t decode;
    // Meaningful only when stage == REQUEST_EVALUATED.
    argus_mqtt_authority_result_t request_result;
    bool published;                              // a result should be emitted
    char result_json[ARGUS_MQTT_BROKER_PAYLOAD_CAP]; // exact bytes, when published
    bool state_changed;    // caller should republish the retained authority snapshot
    bool link_online;      // meaningful only when state_changed
} argus_mqtt_authority_outcome_t;

// The production admission/handling path for command/core/request_authority
// and command/core/release_authority, made directly callable: it takes the
// session core and message as explicit parameters rather than reading
// s_runtime, so a test can call the REAL function against a synthetic core
// and message with no broker, no FreeRTOS queue, and no mutex - and both the
// runtime task and a test exercise the identical code, not a transcription
// of its gate order.
//
// Callers must have already applied broker-level publish admission (topic
// scope, capability) exactly as the broker and the REQUEST_AUTHORITY
// permission check do today - this function starts from "the message is a
// legitimate, capability-checked authority request/release", the same point
// handle_authority_request always started from. `core` is mutated in place
// for GRANTED/ALREADY_HELD/RELEASE_ACCEPTED outcomes and left byte-identical
// for every other stage; a caller sharing `core` across threads is
// responsible for its own exclusion, exactly as before.
argus_mqtt_authority_outcome_t argus_mqtt_authority_admit(
    argus_mqtt_session_core_t *core, const argus_mqtt_broker_message_t *message,
    bool release, uint8_t authority_profile, bool core_window_open,
    bool machine_running, uint64_t now_ms);

// Pure computation of the two A2.10 status values that depend on more than
// the session core alone (local_control_status also depends on the
// commissioned profile and whether the acquisition window is open). Exported
// so a test can verify the exact function the retained snapshot and the
// per-request result both call, rather than a description of what it does.
void authority_status_from_core(
    const argus_mqtt_session_core_t *core, uint8_t authority_profile,
    bool core_window_open, uint64_t now_ms,
    char *lease_status_out, size_t lease_status_cap,
    char *local_status_out, size_t local_status_cap);

// SIZE of the bounded A2.6 duplicate/replay cache, derived from an explicit
// protocol property rather than a traffic estimate (final-focused-pass item
// 4 requires this): the broker rejects simultaneously active duplicate MQTT
// client IDs (§5 of the contract), so at most one connection per enrolled
// machine can be issuing requests at once, and ARGUS_SECURITY_MAX_MACHINES
// bounds how many machines can ever be enrolled at all. At most
// ARGUS_SECURITY_MAX_MACHINES distinct request identities can therefore be
// in flight at any instant, and that - not a traffic estimate - is the
// bound. In the header so tests exercise the REAL bound rather than
// hardcoding a stale copy of it.
//
// An earlier cut of this pass used 2x the ceiling, reasoning that a machine
// might have a request and an immediate follow-up resident together. That
// was speculative padding, not a protocol property, and it cost real heap
// (see the entry-layout comment in argus_mqtt_runtime.c - the oversized
// table starved the 4D.4 directory tests' allocations on hardware). It is
// also unnecessary: a machine's follow-up request is only sent after its
// first was answered, and cache residency no longer carries any safety
// burden - the A2.2 epoch rules do (see A2.6). One slot per machine that
// can have a request in flight is the honest bound.
#define ARGUS_MQTT_AUTH_DUP_CACHE_SIZE ARGUS_SECURITY_MAX_MACHINES

// Clears the bounded A2.6 duplicate-request cache. Called by
// argus_mqtt_runtime_prepare_start() on every broker (re)start, and exposed
// so tests can start each case from a known-empty cache without needing a
// full runtime restart.
void argus_mqtt_runtime_reset_duplicate_cache(void);

// The number of DISTINCT (session, request_id) keys admitted since the last
// reset, whether or not still cache-resident. For diagnostics and tests
// proving eviction/rollover behavior - NOT a mechanism for recognizing a
// specific evicted key, which this cache cannot do (see the eviction
// comment in argus_mqtt_runtime.c).
uint32_t argus_mqtt_runtime_duplicate_admission_count(void);

// The current A2.10 authority_reason string, exactly as published on
// status/core/authority_reason. Exported so tests can assert the real
// transition set_authority_reason() records, rather than inferring it from
// side effects.
const char *argus_mqtt_runtime_get_authority_reason(void);

// Final-focused-pass item 2. The session-side admission decision for an
// OPERATIONAL command (session binding, holder identity + connection
// binding, link state, A2.9 epoch), extracted from handle_command() so a
// test can drive the exact production rule against a synthetic core -
// notably the disconnect -> rebind -> command sequence, where the defect
// this pass fixed (rebind leaving link OFFLINE) lived precisely at this
// gate. Everything that CONSUMES state (sequence check, dispatch, result
// commit) stays in handle_command; this function is pure and mutates
// nothing.
typedef enum {
    ARGUS_MQTT_COMMAND_ADMIT_OK = 0,
    ARGUS_MQTT_COMMAND_ADMIT_SESSION_MISMATCH,
    ARGUS_MQTT_COMMAND_ADMIT_NOT_HOLDER,
    ARGUS_MQTT_COMMAND_ADMIT_STALE_EPOCH,
} argus_mqtt_command_admission_t;

argus_mqtt_command_admission_t argus_mqtt_command_admission_check(
    const argus_mqtt_session_core_t *core, const char *principal_identifier,
    uint64_t connection_id, const argus_mqtt_command_t *command);

// Final-focused-pass item 2. Pure A2.10 authority_reason decision rules,
// extracted from the runtime wiring that invokes them so a test can assert
// the exact transition rule directly. NULL means "no change" - see each
// function's definition in argus_mqtt_runtime.c for the reasoning.
const char *argus_mqtt_authority_reason_for_request(
    argus_mqtt_authority_result_t result, bool was_online_before,
    uint8_t requester_client_type, uint8_t authority_profile);
const char *argus_mqtt_authority_reason_for_release(bool released);
const char *argus_mqtt_authority_reason_for_disconnect(void);
const char *argus_mqtt_authority_reason_for_expiry(void);
