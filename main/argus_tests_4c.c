#include "argus_tests_4c.h"
#include "argus_nvs_config.h"
#include "argus_security_store.h"

#include <string.h>

#include "argus_mqtt_contract.h"

#define CHECK(condition) do { if (!(condition)) return ESP_FAIL; } while (0)

static const char *SESSION = "0123456789abcdef";

static argus_mqtt_command_t command(uint32_t sequence, const char *id,
                                    argus_mqtt_action_t action)
{
    argus_mqtt_command_t value = {
        .sequence = sequence,
        .action = action,
        .forward = true,
    };
    strlcpy(value.session, SESSION, sizeof(value.session));
    strlcpy(value.command_id, id, sizeof(value.command_id));
    return value;
}

esp_err_t test_4c_topic_root_and_canonical_topics(void)
{
    argus_mqtt_topics_t topics;
    CHECK(argus_mqtt_topics_build(&topics, "paladin", "pump_001") == ESP_OK);
    CHECK(strcmp(topics.root, "argus/paladin/pump_001") == 0);
    CHECK(strcmp(topics.command_set_target,
                 "argus/paladin/pump_001/command/pump1/set_target_rpm_milli") == 0);
    CHECK(strcmp(topics.command_e_stop,
                 "argus/paladin/pump_001/command/pump1/e_stop") == 0);
    CHECK(strcmp(topics.heartbeat,
                 "argus/paladin/pump_001/status/supervisor/heartbeat") == 0);
    CHECK(strcmp(topics.command_result,
                 "argus/paladin/pump_001/event/pump1/command_result") == 0);
    CHECK(strncmp(topics.root, "argus/peristaltic", 17U) != 0);
    return ESP_OK;
}

esp_err_t test_4c_topic_component_rejections(void)
{
    argus_mqtt_topics_t topics;
    CHECK(argus_mqtt_topics_build(NULL, "paladin", "pump_001") == ESP_ERR_INVALID_ARG);
    CHECK(argus_mqtt_topics_build(&topics, "", "pump_001") == ESP_ERR_INVALID_ARG);
    CHECK(argus_mqtt_topics_build(&topics, "bad/name", "pump_001") == ESP_ERR_INVALID_ARG);
    CHECK(argus_mqtt_topics_build(&topics, "bad+", "pump_001") == ESP_ERR_INVALID_ARG);
    CHECK(argus_mqtt_topics_build(&topics, "paladin", "bad#") == ESP_ERR_INVALID_ARG);
    CHECK(argus_mqtt_topics_build(&topics, "paladin", "bad name") == ESP_ERR_INVALID_ARG);
    CHECK(argus_mqtt_topics_build(
              &topics, "123456789012345678901234567890123", "pump_001") ==
          ESP_ERR_INVALID_ARG);
    return ESP_OK;
}

esp_err_t test_4c_topic_ownership_policy(void)
{
    argus_mqtt_topics_t topics;
    CHECK(argus_mqtt_topics_build(&topics, "paladin", "pump_001") == ESP_OK);
    argus_mqtt_broker_message_t message = {.qos = 1U};
    strlcpy(message.topic, topics.command_start, sizeof(message.topic));
    CHECK(argus_mqtt_topics_external_publish_allowed(&topics, &message));
    strlcpy(message.topic, topics.heartbeat, sizeof(message.topic));
    CHECK(argus_mqtt_topics_external_publish_allowed(&topics, &message));
    message.retain = true;
    CHECK(!argus_mqtt_topics_external_publish_allowed(&topics, &message));
    message.retain = false;
    strlcpy(message.topic, topics.state_mode, sizeof(message.topic));
    CHECK(!argus_mqtt_topics_external_publish_allowed(&topics, &message));
    strlcpy(message.topic, topics.metadata_device_name, sizeof(message.topic));
    CHECK(!argus_mqtt_topics_external_publish_allowed(&topics, &message));
    strlcpy(message.topic, "argus/peristaltic/cmd/run", sizeof(message.topic));
    CHECK(!argus_mqtt_topics_external_publish_allowed(&topics, &message));
    strlcpy(message.topic, "argus/paladin/pump_001/command/pump1/+", sizeof(message.topic));
    CHECK(!argus_mqtt_topics_external_publish_allowed(&topics, &message));
    return ESP_OK;
}

esp_err_t test_4c_command_decoder_all_actions(void)
{
    static const argus_mqtt_action_t actions[] = {
        ARGUS_MQTT_ACTION_START, ARGUS_MQTT_ACTION_STOP,
        ARGUS_MQTT_ACTION_UNLOCK, ARGUS_MQTT_ACTION_E_STOP,
        ARGUS_MQTT_ACTION_RESET_E_STOP, ARGUS_MQTT_ACTION_RECOVER,
    };
    const char *body =
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,"
        "\"command_id\":\"cmd-1\",\"value\":true}";
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        argus_mqtt_command_t out;
        CHECK(argus_mqtt_decode_command(body, strlen(body), actions[i],
                                        200000, &out) == ARGUS_MQTT_DECODE_OK);
        CHECK(out.action == actions[i] && out.sequence == 1U);
    }
    const char *target =
        "{\"value\":8000,\"command_id\":\"target.1\",\"sequence\":2,"
        "\"session\":\"0123456789abcdef\"}";
    argus_mqtt_command_t out;
    CHECK(argus_mqtt_decode_command(target, strlen(target),
                                    ARGUS_MQTT_ACTION_SET_TARGET,
                                    200000, &out) == ARGUS_MQTT_DECODE_OK);
    CHECK(out.target_rpm_milli == 8000 && out.forward);
    return ESP_OK;
}

esp_err_t test_4c_command_decoder_strict_structure(void)
{
    static const char *const invalid[] = {
        "", "[]", "{}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"command_id\":\"x\",\"value\":true",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"command_id\":\"x\",\"value\":true} trailing",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"command_id\":\"x\",\"value\":true,\"extra\":1}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"sequence\":2,\"command_id\":\"x\",\"value\":true}",
        "{\"session\":{\"nested\":1},\"sequence\":1,\"command_id\":\"x\",\"value\":true}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        argus_mqtt_command_t out;
        CHECK(argus_mqtt_decode_command(invalid[i], strlen(invalid[i]),
                                        ARGUS_MQTT_ACTION_START, 200000, &out) !=
              ARGUS_MQTT_DECODE_OK);
    }
    return ESP_OK;
}

esp_err_t test_4c_command_decoder_value_contract(void)
{
    static const char *const invalid_start[] = {
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"command_id\":\"x\",\"value\":false}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"command_id\":\"x\",\"value\":1}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"command_id\":\"x\",\"value\":\"true\"}",
    };
    for (size_t i = 0; i < sizeof(invalid_start) / sizeof(invalid_start[0]); ++i) {
        argus_mqtt_command_t out;
        CHECK(argus_mqtt_decode_command(invalid_start[i], strlen(invalid_start[i]),
                                        ARGUS_MQTT_ACTION_START, 200000, &out) !=
              ARGUS_MQTT_DECODE_OK);
    }
    const char *too_high =
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"command_id\":\"x\",\"value\":200001}";
    const char *negative =
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"command_id\":\"x\",\"value\":-1}";
    const char *zero =
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"command_id\":\"x\",\"value\":0}";
    argus_mqtt_command_t out;
    CHECK(argus_mqtt_decode_command(too_high, strlen(too_high),
                                    ARGUS_MQTT_ACTION_SET_TARGET, 200000, &out) !=
          ARGUS_MQTT_DECODE_OK);
    CHECK(argus_mqtt_decode_command(negative, strlen(negative),
                                    ARGUS_MQTT_ACTION_SET_TARGET, 200000, &out) !=
          ARGUS_MQTT_DECODE_OK);
    CHECK(argus_mqtt_decode_command(zero, strlen(zero),
                                    ARGUS_MQTT_ACTION_SET_TARGET, 200000, &out) ==
          ARGUS_MQTT_DECODE_OK);
    CHECK(out.target_rpm_milli == 0);
    return ESP_OK;
}

esp_err_t test_4c_command_decoder_identity_fields(void)
{
    static const char *const invalid[] = {
        "{\"session\":\"0123456789abcdeF\",\"sequence\":1,\"command_id\":\"x\",\"value\":true}",
        "{\"session\":\"short\",\"sequence\":1,\"command_id\":\"x\",\"value\":true}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":0,\"command_id\":\"x\",\"value\":true}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":01,\"command_id\":\"x\",\"value\":true}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":4294967296,\"command_id\":\"x\",\"value\":true}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"command_id\":\"\",\"value\":true}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"command_id\":\"bad/id\",\"value\":true}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        argus_mqtt_command_t out;
        CHECK(argus_mqtt_decode_command(invalid[i], strlen(invalid[i]),
                                        ARGUS_MQTT_ACTION_START, 200000, &out) !=
              ARGUS_MQTT_DECODE_OK);
    }
    return ESP_OK;
}

esp_err_t test_4c_command_decoder_length_and_nul(void)
{
    char oversized[ARGUS_MQTT_BROKER_PAYLOAD_CAP];
    memset(oversized, ' ', sizeof(oversized));
    argus_mqtt_command_t out;
    CHECK(argus_mqtt_decode_command(oversized, sizeof(oversized),
                                    ARGUS_MQTT_ACTION_START, 200000, &out) ==
          ARGUS_MQTT_DECODE_TOO_LARGE);
    const char body[] =
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"command_id\":\"x\",\"value\":true}";
    char embedded[sizeof(body)];
    memcpy(embedded, body, sizeof(body));
    embedded[10] = '\0';
    CHECK(argus_mqtt_decode_command(embedded, sizeof(body) - 1U,
                                    ARGUS_MQTT_ACTION_START, 200000, &out) ==
          ARGUS_MQTT_DECODE_MALFORMED);
    return ESP_OK;
}

esp_err_t test_4c_heartbeat_decoder_contract(void)
{
    const char *body = " { \"counter\" : 42, \"session\" : \"0123456789abcdef\" } ";
    argus_mqtt_heartbeat_t out;
    CHECK(argus_mqtt_decode_heartbeat(body, strlen(body), &out) ==
          ARGUS_MQTT_DECODE_OK);
    CHECK(out.counter == 42U && strcmp(out.session, SESSION) == 0);
    return ESP_OK;
}

esp_err_t test_4c_heartbeat_decoder_rejections(void)
{
    static const char *const invalid[] = {
        "", "{}", "[]",
        "{\"session\":\"0123456789abcdef\",\"counter\":0}",
        "{\"session\":\"0123456789abcdef\",\"counter\":1,\"counter\":2}",
        "{\"session\":\"0123456789abcdef\",\"counter\":1,\"extra\":1}",
        "{\"session\":\"0123456789abcdef\",\"counter\":true}",
        "{\"session\":\"0123456789abcdef\",\"counter\":{\"nested\":1}}",
        "{\"session\":\"0123456789abcdef\",\"counter\":1} trailing",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        argus_mqtt_heartbeat_t out;
        CHECK(argus_mqtt_decode_heartbeat(invalid[i], strlen(invalid[i]), &out) !=
              ARGUS_MQTT_DECODE_OK);
    }
    return ESP_OK;
}

esp_err_t test_4c_serial_number_arithmetic(void)
{
    CHECK(argus_mqtt_session_is_newer(2U, 1U));
    CHECK(!argus_mqtt_session_is_newer(1U, 1U));
    CHECK(!argus_mqtt_session_is_newer(1U, 2U));
    CHECK(argus_mqtt_session_is_newer(1U, UINT32_MAX));
    CHECK(!argus_mqtt_session_is_newer(0x80000001U, 1U));
    return ESP_OK;
}

esp_err_t test_4c_heartbeat_lease_binding(void)
{
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_mqtt_heartbeat_t heartbeat = {.counter = 1U};
    strlcpy(heartbeat.session, SESSION, sizeof(heartbeat.session));
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 11U, NULL, 0U, &heartbeat, 100U) == ESP_OK);
    CHECK(core.link == ARGUS_MQTT_LINK_ONLINE && core.lease_connection_id == 11U);
    heartbeat.counter = 2U;
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 12U, NULL, 0U, &heartbeat, 200U) ==
          ESP_ERR_INVALID_STATE);
    CHECK(core.lease_connection_id == 11U);
    heartbeat.counter = 1U;
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 11U, NULL, 0U, &heartbeat, 200U) ==
          ESP_ERR_INVALID_STATE);
    return ESP_OK;
}

esp_err_t test_4c_heartbeat_expiry_is_observability_only(void)
{
    struct {
        uint32_t motion_sentinel;
        argus_mqtt_session_core_t core;
        uint32_t state_sentinel;
    } isolated = {.motion_sentinel = 0x12345678U, .state_sentinel = 0x87654321U};
    argus_mqtt_session_core_init(&isolated.core, SESSION);
    argus_mqtt_heartbeat_t heartbeat = {.counter = 1U};
    strlcpy(heartbeat.session, SESSION, sizeof(heartbeat.session));
    CHECK(argus_mqtt_session_accept_heartbeat(&isolated.core, 1U, NULL, 0U, &heartbeat, 100U) == ESP_OK);
    CHECK(!argus_mqtt_session_tick(&isolated.core, 6099U));
    CHECK(argus_mqtt_session_tick(&isolated.core, 6100U));
    CHECK(isolated.core.link == ARGUS_MQTT_LINK_STALE);
    CHECK(isolated.core.lease_connection_id == 0U);
    CHECK(isolated.core.heartbeat_counter == 1U);
    CHECK(argus_mqtt_session_accept_heartbeat(
              &isolated.core, 1U, NULL, 0U, &heartbeat, 6200U) == ESP_ERR_INVALID_STATE);
    heartbeat.counter = 2U;
    CHECK(argus_mqtt_session_accept_heartbeat(
              &isolated.core, 1U, NULL, 0U, &heartbeat, 6200U) == ESP_OK);
    CHECK(isolated.motion_sentinel == 0x12345678U);
    CHECK(isolated.state_sentinel == 0x87654321U);
    return ESP_OK;
}

// ---- Explicit authority acquisition (Phase 4C Amendment A1) -------------
//
// Initial ownership is a property of how the installation was COMMISSIONED,
// never of who connected first. Transfer is asymmetric: ArgusCore may request
// authority from the panel; the panel may not take it from a healthy Core
// lease. The controller adjudicates both.

#define HMI_T   ARGUS_MACHINE_CLIENT_HMI
#define CORE_T  ARGUS_MACHINE_CLIENT_ARGUS_COMMAND
#define SVC_T   ARGUS_MACHINE_CLIENT_SERVICE_TOOL
#define P_ALONE ARGUS_AUTHORITY_PROFILE_STANDALONE_HMI
#define P_CORE  ARGUS_AUTHORITY_PROFILE_ARGUSCORE_PREFERRED

esp_err_t test_4c_authority_standalone_profile(void)
{
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(core.authority_epoch == 0U);  // 0 means never owned

    // The panel is the authority in a standalone installation.
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-panel", HMI_T,
              P_ALONE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    CHECK(strcmp(core.lease_machine_id, "m-panel") == 0);
    CHECK(core.authority_epoch == 1U);
    CHECK(core.link == ARGUS_MQTT_LINK_ONLINE);

    // An ArgusCore that turns up on the network cannot take control of a unit
    // commissioned standalone - that requires recommissioning, not a request.
    CHECK(argus_mqtt_session_request_authority(&core, 12U, "m-core", CORE_T,
              P_ALONE, false, 200U) == ARGUS_MQTT_AUTHORITY_DENIED_PROFILE);
    CHECK(strcmp(core.lease_machine_id, "m-panel") == 0);
    CHECK(core.authority_epoch == 1U);  // a refused request changes nothing
    return ESP_OK;
}

esp_err_t test_4c_authority_startup_window_blocks_early_hmi(void)
{
    // The whole point of the bounded window: a panel that boots faster than
    // ArgusCore must not win control by arriving first.
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);

    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-panel", HMI_T,
              P_CORE, true, 100U) == ARGUS_MQTT_AUTHORITY_DENIED_PROFILE);
    CHECK(core.lease_machine_id[0] == '\0');
    CHECK(core.authority_epoch == 0U);

    // ArgusCore may acquire at any point during its own window.
    CHECK(argus_mqtt_session_request_authority(&core, 12U, "m-core", CORE_T,
              P_CORE, true, 200U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    CHECK(core.lease_client_type == CORE_T);
    CHECK(core.authority_epoch == 1U);
    return ESP_OK;
}

esp_err_t test_4c_authority_hmi_is_fallback_after_window(void)
{
    // If ArgusCore never acquires, the pump must not stay unusable.
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-panel", HMI_T,
              P_CORE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    CHECK(core.lease_client_type == HMI_T);
    CHECK(core.authority_epoch == 1U);
    return ESP_OK;
}

esp_err_t test_4c_authority_transfer_is_asymmetric(void)
{
    // ArgusCore may request a transfer from the panel...
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-panel", HMI_T,
              P_CORE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    uint32_t epoch_after_panel = core.authority_epoch;

    CHECK(argus_mqtt_session_request_authority(&core, 12U, "m-core", CORE_T,
              P_CORE, false, 200U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);
    CHECK(core.lease_client_type == CORE_T);
    // Ownership changed hands, so the epoch MUST advance - that is what
    // invalidates the panel's in-flight commands immediately.
    CHECK(core.authority_epoch != epoch_after_panel);
    uint32_t epoch_after_core = core.authority_epoch;

    // ...but the panel may NOT take authority from a healthy Core lease.
    CHECK(argus_mqtt_session_request_authority(&core, 13U, "m-panel", HMI_T,
              P_CORE, false, 300U) == ARGUS_MQTT_AUTHORITY_DENIED_HELD);
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);
    CHECK(core.lease_client_type == CORE_T);
    CHECK(core.authority_epoch == epoch_after_core);
    CHECK(core.lease_connection_id == 12U);
    return ESP_OK;
}

esp_err_t test_4c_authority_reacquire_by_self_keeps_epoch(void)
{
    // Re-requesting authority you already hold must not advance the epoch, or
    // an owner would invalidate its own in-flight commands by asking again.
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-core", CORE_T,
              P_CORE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    uint32_t epoch = core.authority_epoch;

    CHECK(argus_mqtt_session_request_authority(&core, 12U, "m-core", CORE_T,
              P_CORE, false, 500U) == ARGUS_MQTT_AUTHORITY_ALREADY_HELD);
    CHECK(core.authority_epoch == epoch);
    CHECK(core.lease_connection_id == 12U);   // rebound to the current socket
    CHECK(core.last_heartbeat_ms == 500U);    // liveness refreshed
    return ESP_OK;
}

esp_err_t test_4c_authority_release_rules(void)
{
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-core", CORE_T,
              P_CORE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    uint32_t epoch = core.authority_epoch;

    // Releasing someone else's authority is not a thing any client may do.
    CHECK(!argus_mqtt_session_release_authority(&core, "m-panel"));
    CHECK(!argus_mqtt_session_release_authority(&core, NULL));
    CHECK(!argus_mqtt_session_release_authority(&core, ""));
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);
    CHECK(core.authority_epoch == epoch);

    CHECK(argus_mqtt_session_release_authority(&core, "m-core"));
    CHECK(core.lease_machine_id[0] == '\0');
    CHECK(core.lease_connection_id == 0U);
    CHECK(core.link == ARGUS_MQTT_LINK_OFFLINE);
    CHECK(core.authority_epoch != epoch);  // release is an ownership change
    return ESP_OK;
}

esp_err_t test_4c_authority_epoch_never_reads_unowned(void)
{
    // 0 is reserved for "never owned". A wrapping counter must skip it, or a
    // live epoch would be indistinguishable from having never had an owner.
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    core.authority_epoch = 0xFFFFFFFFU;
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-core", CORE_T,
              P_CORE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    CHECK(core.authority_epoch == 1U);
    return ESP_OK;
}

esp_err_t test_4c_authority_rejects_malformed_requests(void)
{
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(argus_mqtt_session_request_authority(&core, 0U, "m-core", CORE_T,
              P_CORE, false, 100U) == ARGUS_MQTT_AUTHORITY_DENIED_INVALID);
    CHECK(argus_mqtt_session_request_authority(&core, 11U, NULL, CORE_T,
              P_CORE, false, 100U) == ARGUS_MQTT_AUTHORITY_DENIED_INVALID);
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "", CORE_T,
              P_CORE, false, 100U) == ARGUS_MQTT_AUTHORITY_DENIED_INVALID);
    // An out-of-range profile must not be coerced to a default - a corrupt
    // record would then get to decide who commands the pump.
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-core", CORE_T,
              0xEE, false, 100U) == ARGUS_MQTT_AUTHORITY_DENIED_INVALID);
    // Every rejection leaves the core completely untouched.
    CHECK(core.lease_machine_id[0] == '\0');
    CHECK(core.authority_epoch == 0U);
    CHECK(core.link != ARGUS_MQTT_LINK_ONLINE);
    return ESP_OK;
}

esp_err_t test_4c_authority_lease_expiry_ends_epoch(void)
{
    // Lease loss must invalidate the epoch so a late command from the expired
    // owner is rejected rather than applied - while leaving machine state,
    // motion and the accepted setpoint alone.
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-core", CORE_T,
              P_CORE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    uint32_t epoch = core.authority_epoch;

    core.has_sequence = true;
    core.last_sequence = 77U;
    strlcpy(core.cached_result, "ACCEPTED", sizeof(core.cached_result));

    CHECK(argus_mqtt_session_tick(&core, 100U + ARGUS_MQTT_HEARTBEAT_TIMEOUT_MS));
    CHECK(core.link == ARGUS_MQTT_LINK_STALE);
    CHECK(core.authority_epoch != epoch);
    CHECK(core.lease_machine_id[0] == '\0');
    // Fail-operational: the accepted-command record is untouched.
    CHECK(core.has_sequence && core.last_sequence == 77U);
    CHECK(strcmp(core.cached_result, "ACCEPTED") == 0);
    return ESP_OK;
}

#undef HMI_T
#undef CORE_T
#undef SVC_T
#undef P_ALONE
#undef P_CORE

// ---- Fail-operational regression tests (Phase 3 5.9) --------------------
//
// Shawn's binding decision ("Pump Operation - Authority Changes",
// 2026-07-26): loss or transfer of ordinary command authority must NOT
// cancel RUN intent, clear the accepted setpoint, or stop the pump. Losing
// the services host, the MQTT session, the network path or the supervisory
// PID must not by itself stop chemical injection. The real-world cost of
// getting this wrong is roughly $4,000/hour when a coiled tubing unit is in
// hole.
//
// The behaviour is ALREADY correct - argus_mqtt_runtime.c logs "supervisor
// heartbeat stale; motion state intentionally unchanged" and mutates
// nothing. These tests exist to pin it so a future change cannot silently
// couple a comms watchdog to a stop. They are deliberately written as
// exact-delta assertions rather than spot checks: any new field a future
// author clears on lease loss fails the test by construction, even though
// this test was written before that field existed.

static esp_err_t assert_only_lease_fields_changed(
    const argus_mqtt_session_core_t *before,
    const argus_mqtt_session_core_t *after,
    bool heartbeat_binding_may_change)
{
    // The controller's accepted-command record must survive comms loss. If
    // any of these change, an authority event has reached into operational
    // state, which is exactly what the decision forbids.
    CHECK(strcmp(before->session, after->session) == 0);
    CHECK(before->has_sequence == after->has_sequence);
    CHECK(before->last_sequence == after->last_sequence);
    CHECK(before->cached_action == after->cached_action);
    CHECK(strcmp(before->cached_command_id, after->cached_command_id) == 0);
    CHECK(strcmp(before->cached_payload, after->cached_payload) == 0);
    CHECK(strcmp(before->cached_result, after->cached_result) == 0);
    CHECK(before->last_heartbeat_ms == after->last_heartbeat_ms);
    if (!heartbeat_binding_may_change) {
        CHECK(before->heartbeat_connection_id == after->heartbeat_connection_id);
        CHECK(before->heartbeat_counter == after->heartbeat_counter);
    }
    return ESP_OK;
}

esp_err_t test_4c_fail_operational_lease_expiry_preserves_operation(void)
{
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_mqtt_heartbeat_t heartbeat = {.counter = 1U};
    strlcpy(heartbeat.session, SESSION, sizeof(heartbeat.session));
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 11U, "m-core", 5U,
                                              &heartbeat, 100U) == ESP_OK);

    // Stand in for a controller that has accepted a command and is running.
    // Losing the supervisor must not disturb any of it.
    core.has_sequence = true;
    core.last_sequence = 42U;
    core.cached_action = ARGUS_MQTT_ACTION_SET_TARGET;
    strlcpy(core.cached_command_id, "cmd-72rpm", sizeof(core.cached_command_id));
    strlcpy(core.cached_payload, "{\"target_rpm_milli\":72000}", sizeof(core.cached_payload));
    strlcpy(core.cached_result, "ACCEPTED", sizeof(core.cached_result));

    argus_mqtt_session_core_t before = core;
    CHECK(argus_mqtt_session_tick(&core, 6100U));

    // What lease expiry IS allowed to do, and all it is allowed to do.
    CHECK(core.link == ARGUS_MQTT_LINK_STALE);
    CHECK(core.lease_connection_id == 0U);
    CHECK(core.lease_machine_id[0] == '\0');
    CHECK(core.lease_client_type == 0U);
    CHECK(assert_only_lease_fields_changed(&before, &core, false) == ESP_OK);
    return ESP_OK;
}

esp_err_t test_4c_fail_operational_disconnect_preserves_operation(void)
{
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_mqtt_heartbeat_t heartbeat = {.counter = 1U};
    strlcpy(heartbeat.session, SESSION, sizeof(heartbeat.session));
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 11U, "m-core", 5U,
                                              &heartbeat, 100U) == ESP_OK);

    core.has_sequence = true;
    core.last_sequence = 42U;
    core.cached_action = ARGUS_MQTT_ACTION_SET_TARGET;
    strlcpy(core.cached_command_id, "cmd-72rpm", sizeof(core.cached_command_id));
    strlcpy(core.cached_payload, "{\"target_rpm_milli\":72000}", sizeof(core.cached_payload));
    strlcpy(core.cached_result, "ACCEPTED", sizeof(core.cached_result));

    argus_mqtt_session_core_t before = core;
    CHECK(argus_mqtt_session_disconnect(&core, 11U));

    // A hard disconnect drops both connection bindings but PRESERVES the lease
    // owner, and must not touch the accepted-command record. Authority moving
    // on a transport event is exactly what the correction removed.
    CHECK(core.link == ARGUS_MQTT_LINK_OFFLINE);
    CHECK(core.lease_connection_id == 0U);
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);
    CHECK(core.heartbeat_connection_id == 0U && core.heartbeat_counter == 0U);
    CHECK(assert_only_lease_fields_changed(&before, &core, true) == ESP_OK);
    return ESP_OK;
}

esp_err_t test_4c_fail_operational_epoch_survives_reconnect_blip(void)
{
    // Shawn's decision 8.1 (2026-07-26): a TCP reconnect INSIDE the lease
    // term preserves authority. A dropped packet is a comms event, and comms
    // events must not move authority. The lease timer keeps running through
    // the blip, so a genuinely dead supervisor still expires on schedule
    // rather than hiding inside a reconnect loop.
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_mqtt_heartbeat_t heartbeat = {.counter = 1U};
    strlcpy(heartbeat.session, SESSION, sizeof(heartbeat.session));
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 11U, "m-core", 5U,
                                              &heartbeat, 100U) == ESP_OK);

    // Same principal returns on a new socket, well inside the lease term.
    heartbeat.counter = 1U;
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 12U, "m-core", 5U,
                                              &heartbeat, 3000U) == ESP_OK);
    CHECK(core.link == ARGUS_MQTT_LINK_ONLINE);
    CHECK(core.lease_connection_id == 12U);
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);

    // The blip must not have restarted the expiry clock from the blip time:
    // the lease deadline follows the renewing heartbeat, so a supervisor
    // that goes truly silent after reconnecting still expires.
    CHECK(!argus_mqtt_session_tick(&core, 8999U));
    CHECK(argus_mqtt_session_tick(&core, 9000U));
    CHECK(core.link == ARGUS_MQTT_LINK_STALE);
    return ESP_OK;
}

esp_err_t test_4c_lease_follows_identity_across_reconnect(void)
{
    // The lease belongs to the authenticated principal. A supervisor that
    // drops and reconnects arrives with a new connection_id and must reclaim
    // its own lease immediately - previously it was refused until the 6s
    // heartbeat timeout expired, a window in which any other client could
    // take control of the pump.
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_mqtt_heartbeat_t heartbeat = {.counter = 1U};
    strlcpy(heartbeat.session, SESSION, sizeof(heartbeat.session));

    CHECK(argus_mqtt_session_accept_heartbeat(&core, 11U, "m-core", 5U,
                                              &heartbeat, 100U) == ESP_OK);
    CHECK(core.lease_connection_id == 11U);
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);
    CHECK(core.lease_client_type == 5U);

    // Same principal, new socket, counter restarted after reconnect.
    heartbeat.counter = 1U;
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 12U, "m-core", 5U,
                                              &heartbeat, 200U) == ESP_OK);
    CHECK(core.lease_connection_id == 12U);
    CHECK(core.link == ARGUS_MQTT_LINK_ONLINE);
    return ESP_OK;
}

esp_err_t test_4c_lease_rejects_other_identities(void)
{
    // Reconnect continuity must not become preemption. A DIFFERENT principal
    // is still refused while the lease is held and ONLINE - precedence-based
    // takeover is Phase 3 5.3 and is deliberately not implemented yet.
    // Every rejection must leave the recorded holder completely unchanged.
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_mqtt_heartbeat_t heartbeat = {.counter = 1U};
    strlcpy(heartbeat.session, SESSION, sizeof(heartbeat.session));
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 11U, "m-core", 5U,
                                              &heartbeat, 100U) == ESP_OK);

    heartbeat.counter = 2U;
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 12U, "m-panel", 1U,
                                              &heartbeat, 200U) == ESP_ERR_INVALID_STATE);
    CHECK(core.lease_connection_id == 11U);
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);
    CHECK(core.lease_client_type == 5U);

    // An unidentified claimant must not match a named holder either, or an
    // unauthenticated path would inherit someone else's lease.
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 13U, NULL, 0U,
                                              &heartbeat, 200U) == ESP_ERR_INVALID_STATE);
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 14U, "", 0U,
                                              &heartbeat, 200U) == ESP_ERR_INVALID_STATE);
    CHECK(core.lease_connection_id == 11U);
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);

    // And a named claimant must not inherit a lease that carries no identity.
    argus_mqtt_session_core_t legacy;
    argus_mqtt_session_core_init(&legacy, SESSION);
    heartbeat.counter = 1U;
    CHECK(argus_mqtt_session_accept_heartbeat(&legacy, 21U, NULL, 0U,
                                              &heartbeat, 100U) == ESP_OK);
    heartbeat.counter = 2U;
    CHECK(argus_mqtt_session_accept_heartbeat(&legacy, 22U, "m-panel", 1U,
                                              &heartbeat, 200U) == ESP_ERR_INVALID_STATE);
    CHECK(legacy.lease_connection_id == 21U);
    return ESP_OK;
}

esp_err_t test_4c_lease_identity_cleared_on_release(void)
{
    // A released lease must not leave a stale holder behind, or the next
    // claimant could be matched against a principal that is no longer there.
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_mqtt_heartbeat_t heartbeat = {.counter = 1U};
    strlcpy(heartbeat.session, SESSION, sizeof(heartbeat.session));

    CHECK(argus_mqtt_session_accept_heartbeat(&core, 11U, "m-core", 5U,
                                              &heartbeat, 100U) == ESP_OK);
    CHECK(argus_mqtt_session_tick(&core, 6100U));
    CHECK(core.link == ARGUS_MQTT_LINK_STALE);
    CHECK(core.lease_machine_id[0] == '\0' && core.lease_client_type == 0U);

    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 11U, "m-core", 5U,
                                              &heartbeat, 100U) == ESP_OK);
    // Corrected expectation. A disconnect clears the CONNECTION BINDING and
    // marks transport offline; it deliberately leaves the owner in place so a
    // momentary drop cannot move authority. The deadline in
    // argus_mqtt_session_tick() is what eventually clears the owner, and that
    // is where the epoch advances.
    CHECK(argus_mqtt_session_disconnect(&core, 11U));
    CHECK(core.link == ARGUS_MQTT_LINK_OFFLINE);
    CHECK(core.lease_connection_id == 0U);
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);
    return ESP_OK;
}

esp_err_t test_4c_disconnect_releases_matching_lease(void)
{
    // RENAMED IN MEANING by the authority correction: a disconnect no longer
    // releases the lease. It invalidates the CONNECTION BINDING only. The
    // previous behaviour handed authority away on a momentary TCP drop, which
    // contradicts the accepted rule that a comms event must not move
    // authority.
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_mqtt_heartbeat_t heartbeat = {.counter = 1U};
    strlcpy(heartbeat.session, SESSION, sizeof(heartbeat.session));
    CHECK(argus_mqtt_session_request_authority(&core, 7U, "m-core",
              ARGUS_MACHINE_CLIENT_ARGUS_COMMAND,
              ARGUS_AUTHORITY_PROFILE_ARGUSCORE_PREFERRED, false, 0U)
          == ARGUS_MQTT_AUTHORITY_GRANTED);
    uint32_t epoch = core.authority_epoch;

    // An unrelated connection dropping changes nothing at all.
    CHECK(!argus_mqtt_session_disconnect(&core, 8U));
    CHECK(core.link == ARGUS_MQTT_LINK_ONLINE);
    CHECK(core.lease_connection_id == 7U);

    // The owner's connection dropping clears the binding and marks transport
    // offline, but the lease, the owner and the epoch all survive.
    CHECK(argus_mqtt_session_disconnect(&core, 7U));
    CHECK(core.link == ARGUS_MQTT_LINK_OFFLINE);
    CHECK(core.lease_connection_id == 0U);
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);
    CHECK(core.authority_epoch == epoch);
    return ESP_OK;
}

esp_err_t test_4c_disconnect_then_rebind_preserves_epoch(void)
{
    // The reconnect case that matters, exercised through the real disconnect
    // entry point rather than only the heartbeat path. The previous test
    // asserted epoch survival while never calling argus_mqtt_session_disconnect
    // at all, so it passed against a production path that did the opposite.
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_mqtt_heartbeat_t heartbeat = {.counter = 1U};
    strlcpy(heartbeat.session, SESSION, sizeof(heartbeat.session));
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-core",
              ARGUS_MACHINE_CLIENT_ARGUS_COMMAND,
              ARGUS_AUTHORITY_PROFILE_ARGUSCORE_PREFERRED, false, 100U)
          == ARGUS_MQTT_AUTHORITY_GRANTED);
    uint32_t epoch = core.authority_epoch;

    CHECK(argus_mqtt_session_disconnect(&core, 11U));

    // Same principal returns on a new socket, inside the lease term.
    heartbeat.counter = 1U;
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 12U, "m-core",
              ARGUS_MACHINE_CLIENT_ARGUS_COMMAND, &heartbeat, 3000U) == ESP_OK);
    CHECK(core.link == ARGUS_MQTT_LINK_ONLINE);
    CHECK(core.lease_connection_id == 12U);
    CHECK(core.authority_epoch == epoch);   // authority never moved

    // A different principal must not inherit the lease by arriving after the
    // drop, whether it heartbeats or asks.
    argus_mqtt_session_core_t other;
    argus_mqtt_session_core_init(&other, SESSION);
    CHECK(argus_mqtt_session_request_authority(&other, 21U, "m-core",
              ARGUS_MACHINE_CLIENT_ARGUS_COMMAND,
              ARGUS_AUTHORITY_PROFILE_ARGUSCORE_PREFERRED, false, 100U)
          == ARGUS_MQTT_AUTHORITY_GRANTED);
    CHECK(argus_mqtt_session_disconnect(&other, 21U));
    CHECK(argus_mqtt_session_request_authority(&other, 22U, "m-panel",
              ARGUS_MACHINE_CLIENT_HMI,
              ARGUS_AUTHORITY_PROFILE_ARGUSCORE_PREFERRED, false, 200U)
          == ARGUS_MQTT_AUTHORITY_DENIED_HELD);
    CHECK(strcmp(other.lease_machine_id, "m-core") == 0);
    return ESP_OK;
}

esp_err_t test_4c_disconnect_without_renewal_still_expires(void)
{
    // The other half of the same change: because the lease now survives a
    // disconnect, expiry must be driven by the deadline rather than by the
    // link state - otherwise a dropped supervisor would hold authority
    // forever. Expiry is where the epoch advances.
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-core",
              ARGUS_MACHINE_CLIENT_ARGUS_COMMAND,
              ARGUS_AUTHORITY_PROFILE_ARGUSCORE_PREFERRED, false, 100U)
          == ARGUS_MQTT_AUTHORITY_GRANTED);
    uint32_t epoch = core.authority_epoch;
    core.has_sequence = true;
    core.last_sequence = 99U;
    strlcpy(core.cached_result, "ACCEPTED", sizeof(core.cached_result));

    CHECK(argus_mqtt_session_disconnect(&core, 11U));
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);   // still owned

    // Not yet due.
    CHECK(!argus_mqtt_session_tick(&core, 100U + ARGUS_MQTT_HEARTBEAT_TIMEOUT_MS - 1U));
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);

    // Due: lease expires, epoch advances, owner cleared.
    CHECK(argus_mqtt_session_tick(&core, 100U + ARGUS_MQTT_HEARTBEAT_TIMEOUT_MS));
    CHECK(core.lease_machine_id[0] == '\0');
    CHECK(core.authority_epoch != epoch);
    // Fail-operational: the accepted-command record is untouched throughout.
    CHECK(core.has_sequence && core.last_sequence == 99U);
    CHECK(strcmp(core.cached_result, "ACCEPTED") == 0);
    return ESP_OK;
}

esp_err_t test_4c_sequence_first_and_newer(void)
{
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_mqtt_command_t first = command(10U, "first", ARGUS_MQTT_ACTION_START);
    CHECK(argus_mqtt_session_check_sequence(&core, &first, "payload-a") ==
          ARGUS_MQTT_SEQUENCE_FIRST);
    argus_mqtt_session_commit_result(&core, &first, "payload-a", "result-a");
    argus_mqtt_command_t newer = command(11U, "newer", ARGUS_MQTT_ACTION_STOP);
    CHECK(argus_mqtt_session_check_sequence(&core, &newer, "payload-b") ==
          ARGUS_MQTT_SEQUENCE_NEWER);
    return ESP_OK;
}

esp_err_t test_4c_sequence_duplicate_and_conflict(void)
{
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_mqtt_command_t original = command(10U, "same", ARGUS_MQTT_ACTION_START);
    argus_mqtt_session_commit_result(&core, &original, "payload-a", "result-a");
    CHECK(argus_mqtt_session_check_sequence(&core, &original, "payload-a") ==
          ARGUS_MQTT_SEQUENCE_DUPLICATE);
    CHECK(strcmp(core.cached_result, "result-a") == 0);
    argus_mqtt_command_t changed_id = command(10U, "different", ARGUS_MQTT_ACTION_START);
    CHECK(argus_mqtt_session_check_sequence(&core, &changed_id, "payload-a") ==
          ARGUS_MQTT_SEQUENCE_CONFLICT);
    CHECK(argus_mqtt_session_check_sequence(&core, &original, "payload-b") ==
          ARGUS_MQTT_SEQUENCE_CONFLICT);
    return ESP_OK;
}

esp_err_t test_4c_sequence_stale_and_wrap(void)
{
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_mqtt_command_t high = command(UINT32_MAX, "high", ARGUS_MQTT_ACTION_STOP);
    argus_mqtt_session_commit_result(&core, &high, "high", "result");
    argus_mqtt_command_t wrapped = command(1U, "wrapped", ARGUS_MQTT_ACTION_STOP);
    CHECK(argus_mqtt_session_check_sequence(&core, &wrapped, "wrapped") ==
          ARGUS_MQTT_SEQUENCE_NEWER);
    argus_mqtt_command_t stale = command(UINT32_MAX - 1U, "stale", ARGUS_MQTT_ACTION_STOP);
    CHECK(argus_mqtt_session_check_sequence(&core, &stale, "stale") ==
          ARGUS_MQTT_SEQUENCE_STALE);
    return ESP_OK;
}

esp_err_t test_4c_session_restart_invalidates_prior_envelope(void)
{
    argus_mqtt_session_core_t before;
    argus_mqtt_session_core_t after;
    argus_mqtt_session_core_init(&before, "0123456789abcdef");
    argus_mqtt_session_core_init(&after, "fedcba9876543210");
    argus_mqtt_command_t prior = command(UINT32_MAX, "prior", ARGUS_MQTT_ACTION_START);
    CHECK(strcmp(prior.session, after.session) != 0);
    CHECK(!after.has_sequence && after.link == ARGUS_MQTT_LINK_OFFLINE);
    return ESP_OK;
}

esp_err_t test_4c_session_generation_contract(void)
{
    char first[ARGUS_MQTT_SESSION_HEX_LEN + 1U];
    char second[ARGUS_MQTT_SESSION_HEX_LEN + 1U];
    CHECK(argus_mqtt_session_format(0U, 0U, first, sizeof(first)) ==
          ESP_ERR_INVALID_ARG);
    CHECK(argus_mqtt_session_format(0x01234567U, 0x89abcdefU,
                                    first, sizeof(first)) == ESP_OK);
    CHECK(strcmp(first, "0123456789abcdef") == 0);
    CHECK(argus_mqtt_session_format(0x01234567U, 0x89abcdf0U,
                                    second, sizeof(second)) == ESP_OK);
    CHECK(strcmp(first, second) != 0);
    CHECK(strlen(first) == ARGUS_MQTT_SESSION_HEX_LEN);
    return ESP_OK;
}

esp_err_t test_4c_retained_capacity_covers_baseline(void)
{
    const size_t retained_metadata = 4U;
    const size_t retained_state = 7U;
    const size_t retained_status = 8U;
    const size_t retained_telemetry = 6U;
    CHECK(retained_metadata + retained_state + retained_status +
              retained_telemetry <= ARGUS_MQTT_BROKER_RETAINED_CAPACITY);
    CHECK(sizeof(argus_mqtt_topics_t) > 4096U);
    return ESP_OK;
}
