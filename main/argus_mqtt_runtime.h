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

// Clears the bounded A2.6 duplicate-request cache. Called by
// argus_mqtt_runtime_prepare_start() on every broker (re)start, and exposed
// so tests can start each case from a known-empty cache without needing a
// full runtime restart.
void argus_mqtt_runtime_reset_duplicate_cache(void);
