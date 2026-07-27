#include "argus_tests_4c.h"
#include "argus_nvs_config.h"
#include "argus_security_store.h"

#include <string.h>

#include "argus_mqtt_contract.h"
#include "argus_mqtt_runtime.h"
#include "argus_mqtt_security.h"
#include "argus_state_mgr.h"

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
        "\"authority_epoch\":0,\"command_id\":\"cmd-1\",\"value\":true}";
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        argus_mqtt_command_t out;
        CHECK(argus_mqtt_decode_command(body, strlen(body), actions[i],
                                        200000, &out) == ARGUS_MQTT_DECODE_OK);
        CHECK(out.action == actions[i] && out.sequence == 1U);
    }
    const char *target =
        "{\"value\":8000,\"authority_epoch\":0,\"command_id\":\"target.1\",\"sequence\":2,"
        "\"session\":\"0123456789abcdef\"}";
    argus_mqtt_command_t out;
    CHECK(argus_mqtt_decode_command(target, strlen(target),
                                    ARGUS_MQTT_ACTION_SET_TARGET,
                                    200000, &out) == ARGUS_MQTT_DECODE_OK);
    CHECK(out.target_rpm_milli == 8000 && out.forward);
    return ESP_OK;
}

esp_err_t test_4c_command_requires_authority_epoch(void)
{
    // A2.9: epochless commands are invalid, so no client can construct one.
    const char *epochless =
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,"
        "\"command_id\":\"x\",\"value\":true}";
    argus_mqtt_command_t out;
    CHECK(argus_mqtt_decode_command(epochless, strlen(epochless),
              ARGUS_MQTT_ACTION_START, 200000, &out) ==
          ARGUS_MQTT_DECODE_MISSING_FIELD);

    const char *with_epoch =
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,"
        "\"authority_epoch\":7,\"command_id\":\"x\",\"value\":true}";
    CHECK(argus_mqtt_decode_command(with_epoch, strlen(with_epoch),
              ARGUS_MQTT_ACTION_START, 200000, &out) == ARGUS_MQTT_DECODE_OK);
    CHECK(out.authority_epoch == 7U);

    // A duplicate epoch field is rejected like any other duplicate.
    const char *dup =
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"authority_epoch\":1,"
        "\"authority_epoch\":2,\"command_id\":\"x\",\"value\":true}";
    CHECK(argus_mqtt_decode_command(dup, strlen(dup),
              ARGUS_MQTT_ACTION_START, 200000, &out) ==
          ARGUS_MQTT_DECODE_DUPLICATE_FIELD);
    return ESP_OK;
}

esp_err_t test_4c_epoch_advances_across_loss_and_regain(void)
{
    // The exact sequence A2.9 requires. This is the case the identity gate
    // cannot catch on its own, so it is proved at the core level: after A
    // loses and regains authority, the epoch it holds is NOT the epoch its
    // delayed command was predicated on.
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);

    // 1. Principal A owns epoch N.
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-core",
              ARGUS_MACHINE_CLIENT_ARGUS_COMMAND,
              ARGUS_AUTHORITY_PROFILE_ARGUSCORE_PREFERRED, false, 100U)
          == ARGUS_MQTT_AUTHORITY_GRANTED);
    uint32_t delayed_command_epoch = core.authority_epoch;

    // 2-3. Authority leaves A (voluntary release is an ownership change).
    CHECK(argus_mqtt_session_release_authority(&core, "m-core"));
    CHECK(core.authority_epoch != delayed_command_epoch);

    // 4. A reacquires under a new epoch.
    CHECK(argus_mqtt_session_request_authority(&core, 12U, "m-core",
              ARGUS_MACHINE_CLIENT_ARGUS_COMMAND,
              ARGUS_AUTHORITY_PROFILE_ARGUSCORE_PREFERRED, false, 200U)
          == ARGUS_MQTT_AUTHORITY_GRANTED);

    // 5-6. A is the owner again, and identity alone would admit the delayed
    // command - but the epoch it carries no longer matches, so admission
    // rejects it.
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);
    CHECK(core.authority_epoch != delayed_command_epoch);
    return ESP_OK;
}

esp_err_t test_4c_command_decoder_strict_structure(void)
{
    static const char *const invalid[] = {
        "", "[]", "{}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":true",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":true} trailing",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":true,\"extra\":1}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"sequence\":2,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":true}",
        "{\"session\":{\"nested\":1},\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":true}",
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
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":false}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":1}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":\"true\"}",
    };
    for (size_t i = 0; i < sizeof(invalid_start) / sizeof(invalid_start[0]); ++i) {
        argus_mqtt_command_t out;
        CHECK(argus_mqtt_decode_command(invalid_start[i], strlen(invalid_start[i]),
                                        ARGUS_MQTT_ACTION_START, 200000, &out) !=
              ARGUS_MQTT_DECODE_OK);
    }
    const char *too_high =
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":200001}";
    const char *negative =
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":-1}";
    const char *zero =
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":0}";
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
        "{\"session\":\"0123456789abcdeF\",\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":true}",
        "{\"session\":\"short\",\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":true}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":0,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":true}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":01,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":true}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":4294967296,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":true}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"\",\"value\":true}",
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"bad/id\",\"value\":true}",
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
        "{\"session\":\"0123456789abcdef\",\"sequence\":1,\"authority_epoch\":0,\"command_id\":\"x\",\"value\":true}";
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

// ===========================================================================
// Section 9 - seam-level tests
//
// Everything above this line tests one function at a time. The gap this
// section closes is the SEAM: an authenticated principal arriving at a topic,
// being classified, scope-checked, capability-checked, admitted by the broker,
// strictly decoded, session- and epoch-validated, and only then arbitrated.
// Each stage was individually correct and the composition had never been run.
//
// HOW FAITHFUL THIS IS - stated plainly, because a green suite that overstates
// its reach is the exact failure this whole pass exists to correct.
//
//   Faithful:  every gate below calls the REAL production function. Nothing is
//              reimplemented, stubbed, or approximated.
//   Mirrored:  the ORDER of the gates is transcribed from argus_mqtt_runtime.c
//              (broker_publish_authorize_cb -> broker_policy_cb ->
//              handle_message -> handle_authority_request). The production
//              handler is static and needs the broker, a FreeRTOS queue and a
//              mutex, so it is not the function being called here.
//   NOT here:  A2.6 duplicate/replay suppression lives in runtime file statics
//              and cannot be reached without refactoring argus_mqtt_runtime.
//              Reimplementing it in the test would prove only that the test
//              agrees with itself. It is left uncovered and reported, not
//              faked. Same for real socket disconnect and rebind, which the
//              Section 5 core tests already cover at their own level.
//
// So: if someone reorders the gates in argus_mqtt_runtime.c, these tests will
// NOT catch it. That is the residual risk and it is the argument for the
// runtime refactor, not something to paper over here.
// ===========================================================================

#define SEAM_CLIENT "paladin"
#define SEAM_UNIT   "pump_001"
#define SEAM_SCOPE  "argus/paladin/pump_001"

// Same short names the Section 5 tests above use. They are #undef'd at the end
// of that block, so they are redeclared here rather than widening the scope of
// the originals.
#define HMI_T   ARGUS_MACHINE_CLIENT_HMI
#define CORE_T  ARGUS_MACHINE_CLIENT_ARGUS_COMMAND
#define SVC_T   ARGUS_MACHINE_CLIENT_SERVICE_TOOL
#define P_ALONE ARGUS_AUTHORITY_PROFILE_STANDALONE_HMI
#define P_CORE  ARGUS_AUTHORITY_PROFILE_ARGUSCORE_PREFERRED

// ONE topic table, shared by every Section 9 test below.
//
// argus_mqtt_topics_t is a little over 7 KB. It cannot be an automatic in
// these tests - the diagnostic task runs the suite on a 12 KB stack that was
// already down to a few kilobytes of headroom, and these frames carry session
// cores and broker messages on top of it. It equally must not be a
// function-static: that is ~7 KB of .bss per test, and thirteen copies starved
// the Wi-Fi driver of internal DRAM at init (esf_buf_setup_static alloc fail
// -> ESP_ERR_NO_MEM). File scope costs one copy and is correct on both counts.
//
// Tests above this point declare their own local `topics` and shadow this one,
// so they are unaffected. The suite is single-threaded and every Section 9
// test rebuilds the table on entry, so no state carries between tests.
static argus_mqtt_topics_t topics;

static esp_err_t seam_topics_build(void)
{
    return argus_mqtt_topics_build(&topics, SEAM_CLIENT, SEAM_UNIT);
}

// Where a message stopped. Asserting the exact stage is much stronger than
// asserting "refused": it catches a refusal that happens for a new and
// incidental reason after a fixture changes.
typedef enum {
    SEAM_STAGE_PUBLISH_AUTHZ = 0,  // broker_publish_authorize_cb refused
    SEAM_STAGE_NOT_AUTHORITY,      // classified as something else entirely
    SEAM_STAGE_POLICY,             // broker_policy_cb cleared policy_admitted
    SEAM_STAGE_CAPABILITY,         // principal lacks REQUEST_AUTHORITY
    SEAM_STAGE_RETAINED,           // defence in depth inside the handler
    SEAM_STAGE_QOS,                // qos 1 required
    SEAM_STAGE_DECODE,             // strict decode refused it
    SEAM_STAGE_SESSION,            // named a different controller session
    SEAM_STAGE_STALE_EPOCH,        // release naming an epoch that is not current
    SEAM_STAGE_ARBITRATED,         // reached the pure core
} seam_stage_t;

typedef struct {
    seam_stage_t stage;
    argus_mqtt_decode_result_t decode;
    argus_mqtt_authority_result_t arbitration;
    argus_mqtt_authority_request_t request;
    bool released;
} seam_outcome_t;

// A byte image of the session core. Compared with memcmp rather than field by
// field so that a field added to argus_mqtt_session_core_t in future is
// covered by every one of these tests the day it is added, without anyone
// remembering to come back here.
typedef struct {
    uint8_t bytes[sizeof(argus_mqtt_session_core_t)];
} seam_image_t;

static seam_image_t seam_capture(const argus_mqtt_session_core_t *core)
{
    seam_image_t image;
    memcpy(image.bytes, core, sizeof(image.bytes));
    return image;
}

static bool seam_unchanged(const seam_image_t *image,
                           const argus_mqtt_session_core_t *core)
{
    return memcmp(image->bytes, core, sizeof(image->bytes)) == 0;
}

// Stands in for a controller that has already accepted a command and is
// running it. Nothing an authority event does may disturb any of this - the
// fail-operational rule, asserted rather than assumed.
static void seam_load_operational_state(argus_mqtt_session_core_t *core)
{
    core->has_sequence = true;
    core->last_sequence = 42U;
    core->cached_action = ARGUS_MQTT_ACTION_SET_TARGET;
    strlcpy(core->cached_command_id, "cmd-72rpm",
            sizeof(core->cached_command_id));
    strlcpy(core->cached_payload, "{\"target_rpm_milli\":72000}",
            sizeof(core->cached_payload));
    strlcpy(core->cached_result, "ACCEPTED", sizeof(core->cached_result));
}

static esp_err_t seam_assert_operational_state(
    const argus_mqtt_session_core_t *core)
{
    CHECK(core->has_sequence);
    CHECK(core->last_sequence == 42U);
    CHECK(core->cached_action == ARGUS_MQTT_ACTION_SET_TARGET);
    CHECK(strcmp(core->cached_command_id, "cmd-72rpm") == 0);
    CHECK(strcmp(core->cached_payload, "{\"target_rpm_milli\":72000}") == 0);
    CHECK(strcmp(core->cached_result, "ACCEPTED") == 0);
    return ESP_OK;
}

static argus_machine_principal_t seam_principal(
    const char *identifier, uint8_t client_type,
    argus_permission_set_t permissions, const char *topic_scope)
{
    argus_machine_principal_t principal = {0};
    strlcpy(principal.identifier, identifier, sizeof(principal.identifier));
    strlcpy(principal.topic_scope, topic_scope, sizeof(principal.topic_scope));
    principal.client_type = client_type;
    principal.permissions = permissions;
    return principal;
}

static argus_mqtt_broker_message_t seam_message(
    const char *topic, const char *payload,
    const argus_machine_principal_t *principal)
{
    argus_mqtt_broker_message_t message = {0};
    strlcpy(message.topic, topic, sizeof(message.topic));
    size_t length = strlen(payload);
    if (length < sizeof(message.payload)) {
        memcpy(message.payload, payload, length + 1U);
        message.payload_len = length;
    }
    message.qos = 1U;
    message.retain = false;
    message.policy_admitted = true;   // the broker's starting position
    message.connection_id = 11U;
    message.principal = *principal;
    return message;
}

// The seam itself. Gate order transcribed from argus_mqtt_runtime.c; every
// call below is the real production function.
// Correction order §7. This used to reimplement handle_authority_request's
// gate sequence by hand - correct in what it modelled, but a rename or
// reorder of the real gates could drift from it silently. It now transcribes
// only the two gates that genuinely live OUTSIDE argus_mqtt_authority_admit()
// by design (broker publish admission and the REQUEST_AUTHORITY capability
// check - both documented as the caller's responsibility in
// argus_mqtt_runtime.h), calling the real production functions for each, and
// then calls argus_mqtt_authority_admit() ITSELF - the exact function
// handle_authority_request calls in production - for everything from
// decoding onward. A rename or reorder inside admit() is now caught here
// automatically, because this test runs the same code, not a description of
// it.
static seam_outcome_t seam_run(const argus_mqtt_topics_t *topics,
                               argus_mqtt_session_core_t *core,
                               const argus_mqtt_broker_message_t *message,
                               uint8_t profile, bool window_open,
                               bool machine_running, uint64_t now_ms)
{
    seam_outcome_t out = {
        .stage = SEAM_STAGE_ARBITRATED,
        .decode = ARGUS_MQTT_DECODE_OK,
        .arbitration = ARGUS_MQTT_AUTHORITY_DENIED_INVALID,
        .released = false,
    };

    // broker_publish_authorize_cb. A refusal here means the message is never
    // handed to the application at all - the handler does not run.
    if (!argus_mqtt_security_publish_allowed(topics, &message->principal,
                                             message->topic)) {
        out.stage = SEAM_STAGE_PUBLISH_AUTHZ;
        return out;
    }

    // handle_message's classification, which decides release vs request and
    // which action a non-authority topic would have been.
    argus_mqtt_action_t action =
        argus_mqtt_topics_classify(topics, message->topic);
    bool release;
    if (action == ARGUS_MQTT_ACTION_REQUEST_AUTHORITY) {
        release = false;
    } else if (action == ARGUS_MQTT_ACTION_RELEASE_AUTHORITY) {
        release = true;
    } else {
        out.stage = SEAM_STAGE_NOT_AUTHORITY;
        return out;
    }

    // handle_authority_request's own two pre-admit checks: policy_admitted
    // (broker_policy_cb's outcome, recomputed here the same way the broker
    // computes it) and the REQUEST_AUTHORITY capability gate. Both are
    // documented in argus_mqtt_runtime.h as the caller's job, not admit()'s.
    bool admitted =
        message->policy_admitted &&
        argus_mqtt_topics_external_publish_allowed(topics, message);
    if (!admitted) {
        out.stage = SEAM_STAGE_POLICY;
        return out;
    }
    if ((message->principal.permissions &
         ARGUS_PERMISSION_REQUEST_AUTHORITY) == 0U) {
        out.stage = SEAM_STAGE_CAPABILITY;
        return out;
    }

    // Everything from here on is the REAL production function - not a
    // transcription of it.
    argus_mqtt_authority_outcome_t real = argus_mqtt_authority_admit(
        core, message, release, profile, window_open, machine_running, now_ms);

    switch (real.stage) {
    case ARGUS_MQTT_AUTHORITY_ADMIT_RETAINED_REFUSED:
        out.stage = SEAM_STAGE_RETAINED;
        return out;
    case ARGUS_MQTT_AUTHORITY_ADMIT_QOS_REFUSED:
        out.stage = SEAM_STAGE_QOS;
        return out;
    case ARGUS_MQTT_AUTHORITY_ADMIT_DECODE_REJECTED:
        out.stage = SEAM_STAGE_DECODE;
        out.decode = real.decode;
        return out;
    case ARGUS_MQTT_AUTHORITY_ADMIT_SESSION_MISMATCH:
        out.stage = SEAM_STAGE_SESSION;
        return out;
    case ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_STALE_EPOCH:
        out.stage = SEAM_STAGE_STALE_EPOCH;
        return out;
    case ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_ACCEPTED:
        out.stage = SEAM_STAGE_ARBITRATED;
        out.released = true;
        return out;
    case ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_NOT_OWNER:
        out.stage = SEAM_STAGE_ARBITRATED;
        out.released = false;
        return out;
    case ARGUS_MQTT_AUTHORITY_ADMIT_REQUEST_EVALUATED:
        out.stage = SEAM_STAGE_ARBITRATED;
        out.arbitration = real.request_result;
        return out;
    case ARGUS_MQTT_AUTHORITY_ADMIT_DUPLICATE_REPLAY:
    case ARGUS_MQTT_AUTHORITY_ADMIT_DUPLICATE_CONFLICT:
    default:
        // The pre-existing seam_outcome_t shape has no slot for these two -
        // duplicate handling is exercised directly against
        // argus_mqtt_authority_admit() in its own tests below, which can see
        // the cached JSON and the CONFLICT/REPLAY distinction that this
        // narrower struct was never built to carry.
        out.stage = SEAM_STAGE_ARBITRATED;
        return out;
    }
}

#define SEAM_REQ_OK \
    "{\"schema\":1,\"request_id\":\"req-1\"," \
    "\"session\":\"0123456789abcdef\",\"authority_epoch\":0}"
// A second, distinct request. Needed anywhere a test sends two logically
// SEPARATE requests in sequence: with the real A2.6 duplicate cache now live
// (argus_mqtt_authority_admit() calls into it - see the comment on
// seam_run()), reusing SEAM_REQ_OK's exact bytes for both would make the
// second call a byte-identical replay of the first's cached result rather
// than a fresh request, which is not what those tests are exercising.
#define SEAM_REQ_OK_2 \
    "{\"schema\":1,\"request_id\":\"req-2\"," \
    "\"session\":\"0123456789abcdef\",\"authority_epoch\":0}"
#define SEAM_REQ_OK_3 \
    "{\"schema\":1,\"request_id\":\"req-3\"," \
    "\"session\":\"0123456789abcdef\",\"authority_epoch\":0}"

static const argus_permission_set_t SEAM_PANEL_PERMS =
    ARGUS_PERMISSION_VIEW_STATUS | ARGUS_PERMISSION_REQUEST_AUTHORITY |
    ARGUS_PERMISSION_MOTION;

// --- entry: classification and required permission --------------------------

esp_err_t test_4c_seam_authority_topic_permission_map(void)
{
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();

    CHECK(argus_mqtt_topics_classify(&topics, topics.command_request_authority) ==
          ARGUS_MQTT_ACTION_REQUEST_AUTHORITY);
    CHECK(argus_mqtt_topics_classify(&topics, topics.command_release_authority) ==
          ARGUS_MQTT_ACTION_RELEASE_AUTHORITY);

    // Section 2's correction: these used to fall through to "no permission
    // required", which publish_allowed reads as "refuse", so the acquisition
    // protocol was unreachable. Fail-closed, but inert.
    CHECK(argus_mqtt_security_required_permission(
              &topics, topics.command_request_authority) ==
          ARGUS_PERMISSION_REQUEST_AUTHORITY);
    CHECK(argus_mqtt_security_required_permission(
              &topics, topics.command_release_authority) ==
          ARGUS_PERMISSION_REQUEST_AUTHORITY);
    CHECK(argus_mqtt_security_required_permission(&topics, topics.heartbeat) ==
          ARGUS_PERMISSION_REQUEST_AUTHORITY);

    CHECK(argus_mqtt_security_required_permission(&topics, topics.command_e_stop) ==
          ARGUS_PERMISSION_SOFTWARE_ESTOP);
    CHECK(argus_mqtt_security_required_permission(
              &topics, topics.command_reset_e_stop) ==
          ARGUS_PERMISSION_RESET_SOFTWARE_ESTOP);
    CHECK(argus_mqtt_security_required_permission(&topics, topics.command_start) ==
          ARGUS_PERMISSION_MOTION);
    CHECK(argus_mqtt_security_required_permission(
              &topics, topics.command_set_target) == ARGUS_PERMISSION_MOTION);

    // Controller-owned topics are not externally publishable at all.
    CHECK(argus_mqtt_security_required_permission(&topics, topics.state_mode) == 0U);
    CHECK(argus_mqtt_security_required_permission(
              &topics, topics.status_control_owner) == 0U);
    CHECK(argus_mqtt_security_required_permission(&topics, "argus/other/x") == 0U);
    return ESP_OK;
}

esp_err_t test_4c_seam_publish_admission_requires_topic_scope(void)
{
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();

    argus_machine_principal_t in_scope = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    CHECK(argus_mqtt_security_publish_allowed(
        &topics, &in_scope, topics.command_request_authority));

    // Right capability, wrong unit. Topic scope is what keeps two pumps on one
    // broker from commanding each other.
    argus_machine_principal_t other_unit = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, "argus/paladin/pump_002");
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &other_unit, topics.command_request_authority));

    // A prefix that is not a path boundary must not match.
    argus_machine_principal_t prefix = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, "argus/paladin/pump_00");
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &prefix, topics.command_request_authority));

    argus_machine_principal_t empty = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, "");
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &empty, topics.command_request_authority));
    return ESP_OK;
}

esp_err_t test_4c_seam_publish_admission_requires_capability(void)
{
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();

    argus_machine_principal_t viewer = seam_principal(
        "m-view", HMI_T, ARGUS_PERMISSION_VIEW_STATUS, SEAM_SCOPE);
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &viewer, topics.command_request_authority));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &viewer, topics.command_release_authority));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &viewer, topics.command_start));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &viewer, topics.command_e_stop));

    // A viewer may still read: subscription is a separate gate.
    CHECK(argus_mqtt_security_subscription_allowed(
        &topics, &viewer, "argus/paladin/pump_001/state/#"));
    return ESP_OK;
}

esp_err_t test_4c_seam_authority_and_motion_are_separate_capabilities(void)
{
    // Holding authority and being allowed to move the pump are independently
    // granted. Neither implies the other, in either direction.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();

    argus_machine_principal_t authority_only = seam_principal(
        "m-sup", CORE_T, ARGUS_PERMISSION_REQUEST_AUTHORITY, SEAM_SCOPE);
    CHECK(argus_mqtt_security_publish_allowed(
        &topics, &authority_only, topics.command_request_authority));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &authority_only, topics.command_start));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &authority_only, topics.command_set_target));

    argus_machine_principal_t motion_only = seam_principal(
        "m-mot", HMI_T, ARGUS_PERMISSION_MOTION, SEAM_SCOPE);
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &motion_only, topics.command_request_authority));
    CHECK(argus_mqtt_security_publish_allowed(
        &topics, &motion_only, topics.command_start));

    // E-stop is separate again: motion does not imply it.
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &motion_only, topics.command_e_stop));
    return ESP_OK;
}

esp_err_t test_4c_seam_controller_owned_topics_reject_external_publish(void)
{
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();

    // The most privileged machine principal the system can express still may
    // not publish controller-owned state. Ownership of a topic is structural,
    // not a matter of permission.
    argus_machine_principal_t root = seam_principal(
        "m-root", CORE_T, ARGUS_PERMISSION_DEFINED_MASK, "*");
    CHECK(!argus_mqtt_security_publish_allowed(&topics, &root, topics.state_mode));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &root, topics.status_control_owner));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &root, topics.status_authority_epoch));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &root, topics.status_core_lease_status));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &root, topics.telemetry_applied));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &root, topics.command_result));
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &root, topics.metadata_device_name));
    return ESP_OK;
}

// --- broker admission --------------------------------------------------------

esp_err_t test_4c_seam_retained_request_refused_before_arbitration(void)
{
    // A2.1. A retained request would let the broker replay an acquisition
    // after a session change. It is refused twice over: the broker policy
    // clears policy_admitted, and the handler refuses retained messages again
    // on its own account. This test pins WHICH gate fires first, so that
    // removing either one is visible rather than silently tolerated.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    seam_load_operational_state(&core);

    argus_machine_principal_t panel = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    argus_mqtt_broker_message_t message = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &panel);
    message.retain = true;

    seam_image_t before = seam_capture(&core);
    seam_outcome_t out = seam_run(&topics, &core, &message, P_ALONE, false, false, 100U);
    CHECK(out.stage == SEAM_STAGE_POLICY);
    CHECK(seam_unchanged(&before, &core));
    CHECK(core.authority_epoch == 0U);
    CHECK(core.lease_machine_id[0] == '\0');
    CHECK(seam_assert_operational_state(&core) == ESP_OK);

    // The handler's own retained check is the second line: reached only if the
    // broker policy ever admitted a retained message.
    message.policy_admitted = true;
    argus_mqtt_broker_message_t forced = message;
    forced.retain = false;
    CHECK(argus_mqtt_topics_external_publish_allowed(&topics, &forced));
    message.retain = true;
    CHECK(!argus_mqtt_topics_external_publish_allowed(&topics, &message));
    return ESP_OK;
}

esp_err_t test_4c_seam_malformed_transport_refused_at_broker(void)
{
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    seam_load_operational_state(&core);
    argus_machine_principal_t panel = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);

    // An embedded NUL - the classic way to make a decoder see a shorter
    // payload than the broker forwarded.
    argus_mqtt_broker_message_t nul_message = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &panel);
    nul_message.payload[8] = '\0';
    seam_image_t before = seam_capture(&core);
    seam_outcome_t out =
        seam_run(&topics, &core, &nul_message, P_ALONE, false, false, 100U);
    CHECK(out.stage == SEAM_STAGE_POLICY);
    CHECK(seam_unchanged(&before, &core));

    // A payload that fills the buffer leaves no room for termination.
    argus_mqtt_broker_message_t big_message = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &panel);
    big_message.payload_len = sizeof(big_message.payload);
    out = seam_run(&topics, &core, &big_message, P_ALONE, false, false, 100U);
    CHECK(out.stage == SEAM_STAGE_POLICY);
    CHECK(seam_unchanged(&before, &core));

    // A wildcard in a publish topic is not a topic; it classifies as nothing
    // and is refused before scope is even consulted.
    argus_mqtt_broker_message_t wildcard = seam_message(
        "argus/paladin/pump_001/command/core/+", SEAM_REQ_OK, &panel);
    out = seam_run(&topics, &core, &wildcard, P_ALONE, false, false, 100U);
    CHECK(out.stage == SEAM_STAGE_PUBLISH_AUTHZ);
    CHECK(seam_unchanged(&before, &core));
    CHECK(seam_assert_operational_state(&core) == ESP_OK);
    return ESP_OK;
}

esp_err_t test_4c_seam_unauthorized_principal_never_reaches_arbitration(void)
{
    // The strongest statement this seam can make: a principal without the
    // capability is stopped at the broker, so the arbitration core is never
    // called at all - not called and refused, but never called.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    seam_load_operational_state(&core);

    argus_machine_principal_t viewer = seam_principal(
        "m-view", HMI_T, ARGUS_PERMISSION_VIEW_STATUS, SEAM_SCOPE);
    argus_mqtt_broker_message_t message = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &viewer);

    seam_image_t before = seam_capture(&core);
    seam_outcome_t out = seam_run(&topics, &core, &message, P_ALONE, false, false, 100U);
    CHECK(out.stage == SEAM_STAGE_PUBLISH_AUTHZ);
    CHECK(seam_unchanged(&before, &core));
    CHECK(core.authority_epoch == 0U);

    // Same for a scoped-out principal that does hold the capability.
    argus_machine_principal_t foreign = seam_principal(
        "m-other", HMI_T, SEAM_PANEL_PERMS, "argus/paladin/pump_002");
    argus_mqtt_broker_message_t foreign_message = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &foreign);
    out = seam_run(&topics, &core, &foreign_message, P_ALONE, false, false, 100U);
    CHECK(out.stage == SEAM_STAGE_PUBLISH_AUTHZ);
    CHECK(seam_unchanged(&before, &core));

    // And the in-handler capability gate, which is what protects the core if
    // the broker gate is ever loosened: reached only when scope admits.
    argus_machine_principal_t scoped_no_cap = seam_principal(
        "m-view2", HMI_T,
        ARGUS_PERMISSION_VIEW_STATUS | ARGUS_PERMISSION_MOTION, SEAM_SCOPE);
    CHECK(!argus_mqtt_security_publish_allowed(
        &topics, &scoped_no_cap, topics.command_request_authority));
    CHECK(seam_assert_operational_state(&core) == ESP_OK);
    return ESP_OK;
}

esp_err_t test_4c_seam_qos_zero_request_refused(void)
{
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    seam_load_operational_state(&core);
    argus_machine_principal_t panel = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);

    argus_mqtt_broker_message_t message = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &panel);
    message.qos = 0U;

    seam_image_t before = seam_capture(&core);
    seam_outcome_t out = seam_run(&topics, &core, &message, P_ALONE, false, false, 100U);
    CHECK(out.stage == SEAM_STAGE_QOS);
    CHECK(seam_unchanged(&before, &core));
    CHECK(core.authority_epoch == 0U);
    CHECK(seam_assert_operational_state(&core) == ESP_OK);
    return ESP_OK;
}

// --- Section 3 rejection matrix, with negative tests -------------------------
//
// Until now this matrix was validated by inspection and compilation. Each
// refusal path below is asserted by its specific decode result, so a change
// that turns one refusal into a different refusal is visible.

esp_err_t test_4c_seam_decode_rejects_structure(void)
{
    argus_mqtt_authority_request_t out;
    static const char *const malformed[] = {
        "",                       // nothing at all
        "   ",                    // whitespace only
        "[]",                     // not an object
        "\"schema\":1",           // bare members
        "{",                      // unterminated
        "{\"schema\"}",           // no colon
        "{\"schema\":1",          // no closing brace
        "{\"schema\":1}}",        // trailing brace
        "{\"schema\":1} extra",   // trailing garbage
        "{schema:1}",             // unquoted key
        "{\"schema\":1 \"request_id\":\"r\"}",   // missing comma
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        CHECK(argus_mqtt_decode_authority_request(
                  malformed[i], strlen(malformed[i]), false, &out) ==
              ARGUS_MQTT_DECODE_MALFORMED);
    }
    return ESP_OK;
}

esp_err_t test_4c_seam_decode_trailing_comma_now_rejected(void)
{
    // Correction order §6. This decoder used to accept a trailing comma
    // before the closing brace - not strict JSON, even though every field
    // was still bounded, typed and validated once reached. The independent
    // review called this out and the contract's own strictness language
    // ("trailing content is rejected") gives no exception for it. Fixed for
    // all three decoders that share this parsing shape (heartbeat, command,
    // authority request); this test pins the authority-request case, which
    // is the one the correction order named.
    argus_mqtt_authority_request_t out;

    const char *trailing_comma =
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1,}";
    CHECK(argus_mqtt_decode_authority_request(
              trailing_comma, strlen(trailing_comma), false, &out) ==
          ARGUS_MQTT_DECODE_MALFORMED);

    // A comma after EVERY field, not just the last, is equally a trailing
    // comma the moment it precedes the close.
    const char *trailing_comma_single_field = "{\"schema\":1,}";
    CHECK(argus_mqtt_decode_authority_request(
              trailing_comma_single_field, strlen(trailing_comma_single_field),
              false, &out) == ARGUS_MQTT_DECODE_MALFORMED);

    // A release with a trailing comma is malformed for the same structural
    // reason, before the release-specific epoch rule is ever reached.
    const char *trailing_comma_release =
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1,}";
    CHECK(argus_mqtt_decode_authority_request(
              trailing_comma_release, strlen(trailing_comma_release),
              true, &out) == ARGUS_MQTT_DECODE_MALFORMED);

    // Trailing whitespace AFTER the closing brace is a different thing from
    // a trailing comma INSIDE the object, and remains explicitly permitted -
    // MQTT payloads are not always trimmed by every publisher, and nothing
    // about that whitespace could ever be misread as content.
    const char *trailing_space =
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1}  ";
    CHECK(argus_mqtt_decode_authority_request(
              trailing_space, strlen(trailing_space), false, &out) ==
          ARGUS_MQTT_DECODE_OK);
    CHECK(out.authority_epoch == 1U);

    // A comma is legal BETWEEN two real fields - only a comma immediately
    // followed by the close is the defect.
    const char *interior_comma =
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1}";
    CHECK(argus_mqtt_decode_authority_request(
              interior_comma, strlen(interior_comma), false, &out) ==
          ARGUS_MQTT_DECODE_OK);
    return ESP_OK;
}

esp_err_t test_4c_seam_decode_schema_and_request_id_reasons(void)
{
    // Correction order §6: schema and request_id failures get their own
    // A2.5 wire reasons rather than falling into the generic invalid_value
    // every other field's failure uses.
    argus_mqtt_authority_request_t out;

    static const char *const bad_schema[] = {
        "{\"schema\":0,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1}",
        "{\"schema\":2,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1}",
        "{\"schema\":true,\"request_id\":\"r\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":1}",
    };
    for (size_t i = 0; i < sizeof(bad_schema) / sizeof(bad_schema[0]); ++i) {
        CHECK(argus_mqtt_decode_authority_request(
                  bad_schema[i], strlen(bad_schema[i]), false, &out) ==
              ARGUS_MQTT_DECODE_SCHEMA_UNSUPPORTED);
    }

    static const char *const bad_request_id[] = {
        "{\"schema\":1,\"request_id\":\"\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"bad/id\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"bad id\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":7,\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1}",
    };
    for (size_t i = 0; i < sizeof(bad_request_id) / sizeof(bad_request_id[0]); ++i) {
        CHECK(argus_mqtt_decode_authority_request(
                  bad_request_id[i], strlen(bad_request_id[i]), false, &out) ==
              ARGUS_MQTT_DECODE_INVALID_REQUEST_ID);
    }

    // session and authority_epoch still fall through to the generic reason -
    // only schema and request_id were named in the correction order.
    const char *bad_session =
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"nothex\","
        "\"authority_epoch\":1}";
    CHECK(argus_mqtt_decode_authority_request(
              bad_session, strlen(bad_session), false, &out) ==
          ARGUS_MQTT_DECODE_INVALID_VALUE);
    return ESP_OK;
}

esp_err_t test_4c_seam_decode_intent_enum(void)
{
    // A2.2: intent is optional, advisory, and never affects admission - but
    // an unsupported value is refused rather than silently accepted and
    // ignored.
    argus_mqtt_authority_request_t out;
    static const char *const supported[] = {
        "OPERATOR_INTENT", "SUPERVISORY_START", "SERVICE", "FALLBACK",
    };
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); ++i) {
        char body[160];
        snprintf(body, sizeof(body),
            "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
            "\"authority_epoch\":1,\"intent\":\"%s\"}", supported[i]);
        CHECK(argus_mqtt_decode_authority_request(
                  body, strlen(body), false, &out) == ARGUS_MQTT_DECODE_OK);
        CHECK(out.has_intent && strcmp(out.intent, supported[i]) == 0);
    }

    static const char *const unsupported[] = {
        "operator_intent",   // case matters
        "OPERATOR",
        "URGENT",
        "",
    };
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        char body[160];
        snprintf(body, sizeof(body),
            "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
            "\"authority_epoch\":1,\"intent\":\"%s\"}", unsupported[i]);
        CHECK(argus_mqtt_decode_authority_request(
                  body, strlen(body), false, &out) == ARGUS_MQTT_DECODE_INVALID_VALUE);
    }

    // Absent entirely: still valid, still optional.
    const char *no_intent =
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1}";
    CHECK(argus_mqtt_decode_authority_request(
              no_intent, strlen(no_intent), false, &out) == ARGUS_MQTT_DECODE_OK);
    CHECK(!out.has_intent);
    return ESP_OK;
}

esp_err_t test_4c_seam_decode_rejects_unknown_and_duplicate_fields(void)
{
    argus_mqtt_authority_request_t out;

    // An unknown field is refused rather than ignored. Silently dropping it
    // would let a future contract version appear to succeed against an old
    // controller that did not honour it.
    static const char *const unknown[] = {
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1,\"force\":true}",
        "{\"priority\":9,\"schema\":1,\"request_id\":\"r\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":1}",
        "{\"schema\":1,\"Request_Id\":\"r\"}",   // keys are case sensitive
    };
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); ++i) {
        CHECK(argus_mqtt_decode_authority_request(
                  unknown[i], strlen(unknown[i]), false, &out) ==
              ARGUS_MQTT_DECODE_UNKNOWN_FIELD);
    }

    // A repeated field is refused rather than last-wins, so no request can be
    // read two ways by two readers.
    static const char *const duplicate[] = {
        "{\"schema\":1,\"schema\":1,\"request_id\":\"r\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"r\",\"request_id\":\"s\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1,\"authority_epoch\":2}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"session\":\"fedcba9876543210\",\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1,\"intent\":\"a\",\"intent\":\"b\"}",
    };
    for (size_t i = 0; i < sizeof(duplicate) / sizeof(duplicate[0]); ++i) {
        CHECK(argus_mqtt_decode_authority_request(
                  duplicate[i], strlen(duplicate[i]), false, &out) ==
              ARGUS_MQTT_DECODE_DUPLICATE_FIELD);
    }
    return ESP_OK;
}

esp_err_t test_4c_seam_decode_rejects_missing_fields(void)
{
    argus_mqtt_authority_request_t out;

    // Each required field removed individually, so a test that passes because
    // some other field was also missing is not possible.
    static const char *const missing[] = {
        "{}",
        "{\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1}",                                  // no schema
        "{\"schema\":1,\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1}",                                  // no request_id
        "{\"schema\":1,\"request_id\":\"r\",\"authority_epoch\":1}", // no session
        "{\"schema\":1,\"request_id\":\"r\","
        "\"session\":\"0123456789abcdef\"}",                       // no epoch
        "{\"intent\":\"take\"}",                                   // optional only
    };
    for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); ++i) {
        CHECK(argus_mqtt_decode_authority_request(
                  missing[i], strlen(missing[i]), false, &out) ==
              ARGUS_MQTT_DECODE_MISSING_FIELD);
    }

    // intent really is optional: the same body without it must decode.
    const char *complete =
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1}";
    CHECK(argus_mqtt_decode_authority_request(
              complete, strlen(complete), false, &out) == ARGUS_MQTT_DECODE_OK);
    CHECK(!out.has_intent);
    return ESP_OK;
}

esp_err_t test_4c_seam_decode_rejects_invalid_values(void)
{
    // Schema and request_id failures moved to their own dedicated reasons -
    // see test_4c_seam_decode_schema_and_request_id_reasons(). What remains
    // here is every value-shape failure that still falls through to the
    // generic invalid_value, per A2.5.
    argus_mqtt_authority_request_t out;
    static const char *const invalid[] = {
        // session must be exactly 16 lowercase hex digits
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789ABCDEF\","
        "\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcde\","
        "\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdefa\","
        "\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdeg\","
        "\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"\","
        "\"authority_epoch\":1}",
        // numbers are unsigned integers, nothing else
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":-1}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1.5}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":01}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":4294967296}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":\"1\"}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":null}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(argus_mqtt_decode_authority_request(
                  invalid[i], strlen(invalid[i]), false, &out) ==
              ARGUS_MQTT_DECODE_INVALID_VALUE);
    }
    return ESP_OK;
}

esp_err_t test_4c_seam_decode_bounds_and_arguments(void)
{
    argus_mqtt_authority_request_t out;
    const char *body =
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1}";

    CHECK(argus_mqtt_decode_authority_request(body, strlen(body), false, NULL) ==
          ARGUS_MQTT_DECODE_INVALID_ARGUMENT);
    CHECK(argus_mqtt_decode_authority_request(NULL, 4U, false, &out) ==
          ARGUS_MQTT_DECODE_INVALID_ARGUMENT);
    CHECK(argus_mqtt_decode_authority_request(body, 0U, false, &out) ==
          ARGUS_MQTT_DECODE_MALFORMED);

    // Past the contract's own ceiling.
    static char oversize[ARGUS_MQTT_AUTHORITY_PAYLOAD_MAX + 64U];
    memset(oversize, 'a', sizeof(oversize));
    CHECK(argus_mqtt_decode_authority_request(
              oversize, sizeof(oversize), false, &out) ==
          ARGUS_MQTT_DECODE_TOO_LARGE);

    // Inside the contract ceiling but past the transport buffer: still refused,
    // and refused as TOO_LARGE rather than being silently truncated.
    CHECK(ARGUS_MQTT_AUTHORITY_PAYLOAD_MAX > ARGUS_MQTT_BROKER_PAYLOAD_CAP);
    CHECK(argus_mqtt_decode_authority_request(
              oversize, ARGUS_MQTT_BROKER_PAYLOAD_CAP, false, &out) ==
          ARGUS_MQTT_DECODE_TOO_LARGE);

    // An embedded NUL is refused rather than treated as a terminator.
    char embedded[128];
    size_t length = strlen(body);
    memcpy(embedded, body, length);
    embedded[10] = '\0';
    CHECK(argus_mqtt_decode_authority_request(embedded, length, false, &out) ==
          ARGUS_MQTT_DECODE_MALFORMED);

    // An over-long request_id is refused at the string bound, not truncated.
    const char *long_id =
        "{\"schema\":1,"
        "\"request_id\":\"0123456789012345678901234567890123456789\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":1}";
    CHECK(argus_mqtt_decode_authority_request(
              long_id, strlen(long_id), false, &out) ==
          ARGUS_MQTT_DECODE_INVALID_VALUE);
    return ESP_OK;
}

esp_err_t test_4c_seam_release_requires_named_epoch(void)
{
    argus_mqtt_authority_request_t out;

    // A2.3. A request may carry epoch 0 meaning "no assumption"; a release may
    // not, because a delayed release from a previous owner would otherwise
    // release whatever epoch happened to be current when it landed.
    const char *epoch_zero =
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":0}";
    CHECK(argus_mqtt_decode_authority_request(
              epoch_zero, strlen(epoch_zero), false, &out) ==
          ARGUS_MQTT_DECODE_OK);
    CHECK(out.authority_epoch == 0U);
    CHECK(argus_mqtt_decode_authority_request(
              epoch_zero, strlen(epoch_zero), true, &out) ==
          ARGUS_MQTT_DECODE_INVALID_VALUE);

    const char *epoch_named =
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":7,\"intent\":\"handover\"}";
    CHECK(argus_mqtt_decode_authority_request(
              epoch_named, strlen(epoch_named), true, &out) ==
          ARGUS_MQTT_DECODE_OK);
    CHECK(out.authority_epoch == 7U);
    CHECK(out.has_intent && strcmp(out.intent, "handover") == 0);
    return ESP_OK;
}

esp_err_t test_4c_seam_rejection_matrix_never_mutates_arbitration(void)
{
    // The keystone. Every refusal path in the matrix, run through the whole
    // seam against a controller that holds a live lease and is running an
    // accepted command, asserting the session core is byte-identical after
    // each one. A refusal that quietly advanced the epoch, cleared the owner,
    // or disturbed the accepted setpoint would fail here regardless of which
    // path produced it.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);

    argus_machine_principal_t panel = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    argus_machine_principal_t challenger = seam_principal(
        "m-other", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);

    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-panel", HMI_T,
              P_ALONE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    seam_load_operational_state(&core);
    CHECK(core.authority_epoch == 1U);

    static const char *const rejected[] = {
        "",
        "   ",
        "[]",
        "{",
        "{}",
        "{\"schema\":1}",
        "{\"schema\":1,}",
        "{schema:1}",
        "{\"schema\":1} extra",
        "{\"schema\":2,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1,\"force\":true}",
        "{\"schema\":1,\"schema\":1,\"request_id\":\"r\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"bad/id\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789ABCDEF\","
        "\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"nothex\","
        "\"authority_epoch\":1}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":-1}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":4294967296}",
        "{\"schema\":1,\"request_id\":\"r\",\"session\":\"0123456789abcdef\","
        "\"authority_epoch\":\"1\"}",
    };

    seam_image_t before = seam_capture(&core);
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        // Both topics, and from both a stranger and the current holder: a
        // malformed request must be inert no matter who sends it.
        argus_mqtt_broker_message_t as_request = seam_message(
            topics.command_request_authority, rejected[i], &challenger);
        seam_outcome_t out =
            seam_run(&topics, &core, &as_request, P_ALONE, false, false, 200U);
        CHECK(out.stage == SEAM_STAGE_DECODE);
        CHECK(out.decode != ARGUS_MQTT_DECODE_OK);
        CHECK(seam_unchanged(&before, &core));

        argus_mqtt_broker_message_t as_release = seam_message(
            topics.command_release_authority, rejected[i], &panel);
        out = seam_run(&topics, &core, &as_release, P_ALONE, false, false, 200U);
        CHECK(out.stage == SEAM_STAGE_DECODE);
        CHECK(out.decode != ARGUS_MQTT_DECODE_OK);
        CHECK(seam_unchanged(&before, &core));
    }

    // A release carrying epoch 0 is a decode refusal, not a release.
    argus_mqtt_broker_message_t zero_release = seam_message(
        topics.command_release_authority, SEAM_REQ_OK, &panel);
    seam_outcome_t out =
        seam_run(&topics, &core, &zero_release, P_ALONE, false, false, 200U);
    CHECK(out.stage == SEAM_STAGE_DECODE);
    CHECK(out.decode == ARGUS_MQTT_DECODE_INVALID_VALUE);
    CHECK(seam_unchanged(&before, &core));

    CHECK(core.authority_epoch == 1U);
    CHECK(strcmp(core.lease_machine_id, "m-panel") == 0);
    CHECK(seam_assert_operational_state(&core) == ESP_OK);
    return ESP_OK;
}

// --- session and epoch validation --------------------------------------------

esp_err_t test_4c_seam_session_mismatch_rejected_before_arbitration(void)
{
    // A request naming a previous controller session must not arbitrate. This
    // is what stops a request composed before a controller restart from taking
    // effect after it.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    seam_load_operational_state(&core);

    argus_machine_principal_t panel = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    const char *stale_session =
        "{\"schema\":1,\"request_id\":\"req-1\","
        "\"session\":\"fedcba9876543210\",\"authority_epoch\":0}";
    argus_mqtt_broker_message_t message = seam_message(
        topics.command_request_authority, stale_session, &panel);

    seam_image_t before = seam_capture(&core);
    seam_outcome_t out = seam_run(&topics, &core, &message, P_ALONE, false, false, 100U);
    CHECK(out.stage == SEAM_STAGE_SESSION);
    CHECK(seam_unchanged(&before, &core));
    CHECK(core.authority_epoch == 0U);
    CHECK(core.lease_machine_id[0] == '\0');
    CHECK(seam_assert_operational_state(&core) == ESP_OK);
    return ESP_OK;
}

esp_err_t test_4c_seam_release_stale_epoch_preserves_owner(void)
{
    // A2.3 through the full seam: a release naming an epoch that is no longer
    // current is refused, so a release that was in flight across a transfer
    // cannot release the new owner's authority.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-panel", HMI_T,
              P_ALONE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    seam_load_operational_state(&core);
    CHECK(core.authority_epoch == 1U);

    argus_machine_principal_t panel = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    const char *wrong_epoch =
        "{\"schema\":1,\"request_id\":\"rel-1\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":2}";
    argus_mqtt_broker_message_t message = seam_message(
        topics.command_release_authority, wrong_epoch, &panel);

    seam_image_t before = seam_capture(&core);
    seam_outcome_t out = seam_run(&topics, &core, &message, P_ALONE, false, false, 200U);
    CHECK(out.stage == SEAM_STAGE_STALE_EPOCH);
    CHECK(!out.released);
    CHECK(seam_unchanged(&before, &core));
    CHECK(core.authority_epoch == 1U);
    CHECK(strcmp(core.lease_machine_id, "m-panel") == 0);

    // The matching epoch does release, and the accepted command survives it.
    const char *right_epoch =
        "{\"schema\":1,\"request_id\":\"rel-2\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":1}";
    argus_mqtt_broker_message_t good = seam_message(
        topics.command_release_authority, right_epoch, &panel);
    out = seam_run(&topics, &core, &good, P_ALONE, false, false, 300U);
    CHECK(out.stage == SEAM_STAGE_ARBITRATED);
    CHECK(out.released);
    CHECK(core.authority_epoch == 2U);          // ownership changed
    CHECK(core.lease_machine_id[0] == '\0');
    CHECK(core.link == ARGUS_MQTT_LINK_OFFLINE);
    CHECK(seam_assert_operational_state(&core) == ESP_OK);   // fail-operational
    return ESP_OK;
}

esp_err_t test_4c_seam_release_by_non_owner_refused(void)
{
    // Releasing someone else's authority is not a thing any client may do,
    // even one that is otherwise fully permitted and names the correct epoch.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-panel", HMI_T,
              P_ALONE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    seam_load_operational_state(&core);

    argus_machine_principal_t stranger = seam_principal(
        "m-other", SVC_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    const char *body =
        "{\"schema\":1,\"request_id\":\"rel-1\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":1}";
    argus_mqtt_broker_message_t message = seam_message(
        topics.command_release_authority, body, &stranger);

    seam_image_t before = seam_capture(&core);
    seam_outcome_t out = seam_run(&topics, &core, &message, P_ALONE, false, false, 200U);
    CHECK(out.stage == SEAM_STAGE_ARBITRATED);
    CHECK(!out.released);
    CHECK(seam_unchanged(&before, &core));
    CHECK(core.authority_epoch == 1U);
    CHECK(strcmp(core.lease_machine_id, "m-panel") == 0);
    CHECK(seam_assert_operational_state(&core) == ESP_OK);
    return ESP_OK;
}

// --- arbitration through the whole seam --------------------------------------

esp_err_t test_4c_seam_request_granted_end_to_end(void)
{
    // The one path that is supposed to work, exercised the whole way through:
    // scoped and permitted panel, well-formed request, standalone profile.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    seam_load_operational_state(&core);

    argus_machine_principal_t panel = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    argus_mqtt_broker_message_t message = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &panel);

    seam_outcome_t out = seam_run(&topics, &core, &message, P_ALONE, false, false, 100U);
    CHECK(out.stage == SEAM_STAGE_ARBITRATED);
    CHECK(out.decode == ARGUS_MQTT_DECODE_OK);
    CHECK(out.arbitration == ARGUS_MQTT_AUTHORITY_GRANTED);
    CHECK(core.authority_epoch == 1U);
    CHECK(strcmp(core.lease_machine_id, "m-panel") == 0);
    CHECK(core.lease_client_type == HMI_T);
    CHECK(core.lease_connection_id == 11U);
    CHECK(core.link == ARGUS_MQTT_LINK_ONLINE);
    // Acquiring authority must not disturb what the pump is already doing.
    CHECK(seam_assert_operational_state(&core) == ESP_OK);

    // Re-requesting from the same identity rebinds but does not advance the
    // epoch: nothing changed hands, and a bump would invalidate our own
    // in-flight commands. A distinct request_id: a real second acquisition
    // attempt is a new request, not a redelivery of the first, and reusing
    // SEAM_REQ_OK here would now be caught as an exact A2.6 replay of the
    // first call's cached GRANTED result instead of being freshly evaluated.
    argus_mqtt_broker_message_t again = seam_message(
        topics.command_request_authority, SEAM_REQ_OK_2, &panel);
    again.connection_id = 12U;
    out = seam_run(&topics, &core, &again, P_ALONE, false, false, 200U);
    CHECK(out.arbitration == ARGUS_MQTT_AUTHORITY_ALREADY_HELD);
    CHECK(core.authority_epoch == 1U);
    CHECK(core.lease_connection_id == 12U);
    CHECK(seam_assert_operational_state(&core) == ESP_OK);
    return ESP_OK;
}

esp_err_t test_4c_seam_core_preempts_panel_end_to_end(void)
{
    // ArgusCore may request a transfer from the panel and the controller
    // adjudicates it. Run through the whole seam rather than the core alone.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);

    argus_machine_principal_t panel = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    argus_machine_principal_t argus_core = seam_principal(
        "m-core", CORE_T, SEAM_PANEL_PERMS, SEAM_SCOPE);

    argus_mqtt_broker_message_t panel_request = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &panel);
    seam_outcome_t out =
        seam_run(&topics, &core, &panel_request, P_CORE, false, false, 100U);
    CHECK(out.arbitration == ARGUS_MQTT_AUTHORITY_GRANTED);
    CHECK(core.authority_epoch == 1U);
    seam_load_operational_state(&core);

    // A distinct request_id: this is ArgusCore's own separate request, not a
    // redelivery of the panel's.
    argus_mqtt_broker_message_t core_request = seam_message(
        topics.command_request_authority, SEAM_REQ_OK_2, &argus_core);
    core_request.connection_id = 12U;
    out = seam_run(&topics, &core, &core_request, P_CORE, false, false, 200U);
    CHECK(out.stage == SEAM_STAGE_ARBITRATED);
    CHECK(out.arbitration == ARGUS_MQTT_AUTHORITY_GRANTED);
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);
    CHECK(core.lease_client_type == CORE_T);
    // Ownership changed, so the epoch advances and the panel's in-flight
    // commands become invalid immediately - without chasing them.
    CHECK(core.authority_epoch == 2U);
    // But the pump keeps doing what it was told to do.
    CHECK(seam_assert_operational_state(&core) == ESP_OK);
    return ESP_OK;
}

esp_err_t test_4c_seam_panel_cannot_preempt_core_end_to_end(void)
{
    // The asymmetry, through the whole seam: the panel may NOT take authority
    // from a healthy ArgusCore lease. Same permissions, same scope, same
    // well-formed request - refused on the commissioned policy alone.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);

    argus_machine_principal_t argus_core = seam_principal(
        "m-core", CORE_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    argus_machine_principal_t panel = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);

    argus_mqtt_broker_message_t core_request = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &argus_core);
    seam_outcome_t out =
        seam_run(&topics, &core, &core_request, P_CORE, false, false, 100U);
    CHECK(out.arbitration == ARGUS_MQTT_AUTHORITY_GRANTED);
    seam_load_operational_state(&core);
    CHECK(core.authority_epoch == 1U);

    // Distinct request_id, same reason as above: a separate request, not a
    // redelivery of ArgusCore's.
    argus_mqtt_broker_message_t panel_request = seam_message(
        topics.command_request_authority, SEAM_REQ_OK_2, &panel);
    panel_request.connection_id = 12U;
    seam_image_t before = seam_capture(&core);
    out = seam_run(&topics, &core, &panel_request, P_CORE, false, false, 200U);
    CHECK(out.stage == SEAM_STAGE_ARBITRATED);
    CHECK(out.arbitration == ARGUS_MQTT_AUTHORITY_DENIED_HELD);
    CHECK(seam_unchanged(&before, &core));
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);
    CHECK(core.authority_epoch == 1U);
    CHECK(seam_assert_operational_state(&core) == ESP_OK);
    return ESP_OK;
}

esp_err_t test_4c_seam_denied_paths_preserve_operation(void)
{
    // Every arbitration refusal, through the seam, against a running pump.
    // A denial is a denial of ownership, never an interruption of operation.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();

    // DENIED_PROFILE - an ArgusCore against a unit commissioned standalone.
    argus_mqtt_session_core_t standalone;
    argus_mqtt_session_core_init(&standalone, SESSION);
    CHECK(argus_mqtt_session_request_authority(&standalone, 11U, "m-panel",
              HMI_T, P_ALONE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    seam_load_operational_state(&standalone);

    argus_machine_principal_t argus_core = seam_principal(
        "m-core", CORE_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    argus_mqtt_broker_message_t core_request = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &argus_core);
    core_request.connection_id = 12U;

    seam_image_t before = seam_capture(&standalone);
    seam_outcome_t out =
        seam_run(&topics, &standalone, &core_request, P_ALONE, false, false, 200U);
    CHECK(out.stage == SEAM_STAGE_ARBITRATED);
    CHECK(out.arbitration == ARGUS_MQTT_AUTHORITY_DENIED_PROFILE);
    CHECK(seam_unchanged(&before, &standalone));
    CHECK(seam_assert_operational_state(&standalone) == ESP_OK);

    // DENIED_PROFILE again - a panel during ArgusCore's bounded window. A
    // faster boot must never convert into control.
    argus_mqtt_session_core_t windowed;
    argus_mqtt_session_core_init(&windowed, SESSION);
    seam_load_operational_state(&windowed);
    argus_machine_principal_t panel = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    // Distinct request_id: each of these three is an independent scenario
    // against a different session core, and the A2.6 duplicate cache is
    // keyed on (session, request_id) alone - it has no notion of `core`.
    // Reusing SEAM_REQ_OK across scenarios within this one test would make
    // the second and third calls exact-payload replays of the first's
    // cached (denied) result rather than fresh evaluations.
    argus_mqtt_broker_message_t panel_request = seam_message(
        topics.command_request_authority, SEAM_REQ_OK_2, &panel);

    before = seam_capture(&windowed);
    out = seam_run(&topics, &windowed, &panel_request, P_CORE, true, false, 100U);
    CHECK(out.arbitration == ARGUS_MQTT_AUTHORITY_DENIED_PROFILE);
    CHECK(seam_unchanged(&before, &windowed));
    CHECK(windowed.authority_epoch == 0U);
    CHECK(seam_assert_operational_state(&windowed) == ESP_OK);

    // DENIED_HELD - a second panel against a healthy peer lease.
    argus_mqtt_session_core_t held;
    argus_mqtt_session_core_init(&held, SESSION);
    CHECK(argus_mqtt_session_request_authority(&held, 11U, "m-panel", HMI_T,
              P_ALONE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    seam_load_operational_state(&held);

    argus_machine_principal_t second_panel = seam_principal(
        "m-panel-2", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    argus_mqtt_broker_message_t second_request = seam_message(
        topics.command_request_authority, SEAM_REQ_OK_3, &second_panel);
    second_request.connection_id = 13U;

    before = seam_capture(&held);
    out = seam_run(&topics, &held, &second_request, P_ALONE, false, false, 200U);
    CHECK(out.arbitration == ARGUS_MQTT_AUTHORITY_DENIED_HELD);
    CHECK(seam_unchanged(&before, &held));
    CHECK(strcmp(held.lease_machine_id, "m-panel") == 0);
    CHECK(held.authority_epoch == 1U);
    CHECK(seam_assert_operational_state(&held) == ESP_OK);
    return ESP_OK;
}

// ===========================================================================
// Independent-review correction order - additional §9 seam coverage.
//
// Everything below exercises production surface added or corrected by that
// pass: the machine-state-aware transfer rule (A2.8), the bounded A2.6
// duplicate cache with real interleaving, the event/core/authority_result
// schema and topic, and the disconnected-but-unexpired truthful-ownership
// fix (A2.11). All of it goes through argus_mqtt_authority_admit() directly
// - the real production function - or through argus_mqtt_authority_status_snapshot's
// pure sibling, not a reimplementation.
// ===========================================================================

esp_err_t test_4c_seam_transfer_unsupported_running_end_to_end(void)
{
    // A2.8: ArgusCore may take over from the panel while the pump is NOT
    // running (already covered by test_4c_seam_core_preempts_panel_end_to_end).
    // While it IS running, the same request must be refused with zero
    // mutation, and the pump must keep running - live running transfer
    // awaits proven bumpless PID tracking and is deferred, not simulated.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-panel", HMI_T,
              P_CORE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    seam_load_operational_state(&core);
    CHECK(core.authority_epoch == 1U);

    argus_machine_principal_t argus_core = seam_principal(
        "m-core", CORE_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    argus_mqtt_broker_message_t core_request = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &argus_core);
    core_request.connection_id = 12U;

    seam_image_t before = seam_capture(&core);
    argus_mqtt_authority_outcome_t out = argus_mqtt_authority_admit(
        &core, &core_request, false, P_CORE, false, /*machine_running=*/true, 200U);
    CHECK(out.stage == ARGUS_MQTT_AUTHORITY_ADMIT_REQUEST_EVALUATED);
    CHECK(out.request_result == ARGUS_MQTT_AUTHORITY_TRANSFER_UNSUPPORTED_RUNNING);
    CHECK(seam_unchanged(&before, &core));
    CHECK(strcmp(core.lease_machine_id, "m-panel") == 0);
    CHECK(core.authority_epoch == 1U);
    CHECK(seam_assert_operational_state(&core) == ESP_OK);

    // The published result names the exact A2.5 reason string.
    CHECK(strstr(out.result_json, "\"reason\":\"transfer_unsupported_running\"") != NULL);
    CHECK(strstr(out.result_json, "\"outcome\":\"REJECTED\"") != NULL);

    // The instant the pump stops, the identical request (a fresh
    // request_id - see the SEAM_REQ_OK_2 comment) is admissible.
    seam_load_operational_state(&core);   // re-affirm cached state is untouched
    argus_mqtt_broker_message_t retry = seam_message(
        topics.command_request_authority, SEAM_REQ_OK_2, &argus_core);
    retry.connection_id = 12U;
    out = argus_mqtt_authority_admit(&core, &retry, false, P_CORE, false,
                                     /*machine_running=*/false, 300U);
    CHECK(out.request_result == ARGUS_MQTT_AUTHORITY_GRANTED);
    CHECK(strcmp(core.lease_machine_id, "m-core") == 0);
    CHECK(core.authority_epoch == 2U);
    return ESP_OK;
}

esp_err_t test_4c_seam_transfer_unsupported_running_only_gates_the_running_transfer_case(void)
{
    // The running check must not leak into cases A2.8 does not name: a
    // same-principal re-request while running (not a transfer at all), and
    // a same-or-higher-standing denial that was already going to be refused
    // for an unrelated reason (denied_held_by_other), must both be
    // unaffected by machine_running.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-panel", HMI_T,
              P_ALONE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    seam_load_operational_state(&core);

    // Same principal, running: not a transfer, must still rebind cleanly.
    argus_machine_principal_t panel = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    argus_mqtt_broker_message_t same_principal = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &panel);
    same_principal.connection_id = 12U;
    argus_mqtt_authority_outcome_t out = argus_mqtt_authority_admit(
        &core, &same_principal, false, P_ALONE, false, /*machine_running=*/true, 200U);
    CHECK(out.request_result == ARGUS_MQTT_AUTHORITY_ALREADY_HELD);
    CHECK(core.authority_epoch == 1U);

    // A second HMI/service principal (not ArgusCore) may never preempt at
    // all, running or not - may_preempt() only admits ARGUSCORE as
    // requester, so this stays DENIED_HELD rather than becoming
    // TRANSFER_UNSUPPORTED_RUNNING.
    argus_machine_principal_t other_panel = seam_principal(
        "m-panel-2", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    argus_mqtt_broker_message_t other_request = seam_message(
        topics.command_request_authority, SEAM_REQ_OK_2, &other_panel);
    other_request.connection_id = 13U;
    out = argus_mqtt_authority_admit(&core, &other_request, false, P_ALONE,
                                     false, /*machine_running=*/true, 300U);
    CHECK(out.request_result == ARGUS_MQTT_AUTHORITY_DENIED_HELD);
    CHECK(strcmp(core.lease_machine_id, "m-panel") == 0);
    return ESP_OK;
}

esp_err_t test_4c_seam_duplicate_cache_replays_across_interleaving(void)
{
    // Correction order §5. The single-slot cache this replaced could only
    // remember the ONE most recently seen (session, request_id): request A,
    // then request B, then a redelivery of A would have B silently evict A's
    // remembered entry, so the redelivery would be re-arbitrated instead of
    // replayed - exactly what A2.6 forbids. This proves the bounded
    // multi-entry cache preserves A2.6 across that interleaving.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);

    argus_machine_principal_t panel_a = seam_principal(
        "m-panel-a", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    argus_machine_principal_t panel_b = seam_principal(
        "m-panel-b", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);

    // Request A grants.
    argus_mqtt_broker_message_t req_a = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &panel_a);
    req_a.connection_id = 11U;
    argus_mqtt_authority_outcome_t out_a = argus_mqtt_authority_admit(
        &core, &req_a, false, P_ALONE, false, false, 100U);
    CHECK(out_a.stage == ARGUS_MQTT_AUTHORITY_ADMIT_REQUEST_EVALUATED);
    CHECK(out_a.request_result == ARGUS_MQTT_AUTHORITY_GRANTED);
    CHECK(strcmp(core.lease_machine_id, "m-panel-a") == 0);
    CHECK(core.authority_epoch == 1U);

    // Request B, a DIFFERENT request_id, interleaves - DENIED_HELD (panel_b
    // cannot preempt panel_a), which is itself now cached.
    argus_mqtt_broker_message_t req_b = seam_message(
        topics.command_request_authority, SEAM_REQ_OK_2, &panel_b);
    req_b.connection_id = 12U;
    argus_mqtt_authority_outcome_t out_b = argus_mqtt_authority_admit(
        &core, &req_b, false, P_ALONE, false, false, 150U);
    CHECK(out_b.request_result == ARGUS_MQTT_AUTHORITY_DENIED_HELD);

    // A QoS 1 redelivery of A - byte-identical payload, same principal,
    // arriving AFTER B. Must replay A's ORIGINAL cached GRANTED result
    // without re-arbitrating - critically, without this being misread as
    // "panel_a re-requesting while holding" (which would read ALREADY_HELD,
    // a DIFFERENT outcome) and without B's intervening request having
    // evicted A's entry.
    seam_image_t before = seam_capture(&core);
    argus_mqtt_broker_message_t req_a_redelivered = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &panel_a);
    req_a_redelivered.connection_id = 99U;   // even a different transport connection
    argus_mqtt_authority_outcome_t replay = argus_mqtt_authority_admit(
        &core, &req_a_redelivered, false, P_ALONE, false, false, 200U);
    CHECK(replay.stage == ARGUS_MQTT_AUTHORITY_ADMIT_DUPLICATE_REPLAY);
    CHECK(strcmp(replay.result_json, out_a.result_json) == 0);
    // No arbitration occurred: byte-identical to the core state after A was
    // granted and B was denied, connection_id 99 never bound to anything.
    CHECK(seam_unchanged(&before, &core));
    CHECK(core.lease_connection_id == 11U);

    // A redelivery of B likewise replays B's cached DENIED_HELD result, not
    // a fresh (and by now different, since A still holds) re-arbitration.
    argus_mqtt_broker_message_t req_b_redelivered = seam_message(
        topics.command_request_authority, SEAM_REQ_OK_2, &panel_b);
    argus_mqtt_authority_outcome_t replay_b = argus_mqtt_authority_admit(
        &core, &req_b_redelivered, false, P_ALONE, false, false, 250U);
    CHECK(replay_b.stage == ARGUS_MQTT_AUTHORITY_ADMIT_DUPLICATE_REPLAY);
    CHECK(strcmp(replay_b.result_json, out_b.result_json) == 0);
    return ESP_OK;
}

esp_err_t test_4c_seam_duplicate_cache_conflict_vs_replay(void)
{
    // Same (session, request_id), DIFFERENT payload bytes: A2.6's
    // duplicate_conflict, not a replay. A request_id is not reusable for a
    // different request.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_machine_principal_t panel = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);

    argus_mqtt_broker_message_t original = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &panel);
    argus_mqtt_authority_outcome_t out = argus_mqtt_authority_admit(
        &core, &original, false, P_ALONE, false, false, 100U);
    CHECK(out.request_result == ARGUS_MQTT_AUTHORITY_GRANTED);

    const char *conflicting =
        "{\"schema\":1,\"request_id\":\"req-1\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":0,"
        "\"intent\":\"SERVICE\"}";
    argus_mqtt_broker_message_t conflict_message = seam_message(
        topics.command_request_authority, conflicting, &panel);
    seam_image_t before = seam_capture(&core);
    argus_mqtt_authority_outcome_t conflict = argus_mqtt_authority_admit(
        &core, &conflict_message, false, P_ALONE, false, false, 150U);
    CHECK(conflict.stage == ARGUS_MQTT_AUTHORITY_ADMIT_DUPLICATE_CONFLICT);
    CHECK(strstr(conflict.result_json, "\"reason\":\"duplicate_conflict\"") != NULL);
    CHECK(seam_unchanged(&before, &core));
    return ESP_OK;
}

esp_err_t test_4c_seam_duplicate_cache_is_bounded_and_evicts_fifo(void)
{
    // §5: memory is explicitly bounded. This proves the bound is real - a
    // (session, request_id) beyond the cache's capacity displaces the
    // OLDEST entry - and documents the accepted consequence: a redelivery
    // of that displaced request is no longer recognized and is
    // re-arbitrated rather than replayed. Re-arbitration is safe (full
    // admission runs again); it is simply not the A2.6-idempotent path.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_machine_principal_t panel = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-panel", HMI_T,
              P_ALONE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);

    // The cache holds 8. Fill it with 8 distinct, cache-populating requests
    // (releases naming the wrong epoch decode fine and populate the cache
    // without disturbing ownership, which keeps this test's setup simple).
    for (uint32_t i = 0; i < 8U; ++i) {
        char body[160];
        snprintf(body, sizeof(body),
            "{\"schema\":1,\"request_id\":\"fill-%lu\","
            "\"session\":\"0123456789abcdef\",\"authority_epoch\":99}",
            (unsigned long)i);
        argus_mqtt_broker_message_t filler = seam_message(
            topics.command_release_authority, body, &panel);
        argus_mqtt_authority_outcome_t filled = argus_mqtt_authority_admit(
            &core, &filler, true, P_ALONE, false, false, 200U + i);
        CHECK(filled.stage == ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_STALE_EPOCH);
    }

    // "fill-0" was the first entry stored and is now the oldest. A 9th
    // distinct key evicts it.
    const char *ninth =
        "{\"schema\":1,\"request_id\":\"fill-8\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":99}";
    argus_mqtt_broker_message_t ninth_message = seam_message(
        topics.command_release_authority, ninth, &panel);
    argus_mqtt_authority_outcome_t ninth_out = argus_mqtt_authority_admit(
        &core, &ninth_message, true, P_ALONE, false, false, 210U);
    CHECK(ninth_out.stage == ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_STALE_EPOCH);

    // A redelivery of "fill-0" no longer finds a cached entry - it is
    // re-evaluated (still STALE_EPOCH on its own merits, since epoch 99 is
    // still wrong; the point is that it is EVALUATED, not that a lookup ever
    // ran), rather than being recognized as a replay. Confirmed the only way
    // observable from outside the cache itself: still gets the correct
    // answer, still zero mutation.
    seam_image_t before = seam_capture(&core);
    const char *fill_zero_again =
        "{\"schema\":1,\"request_id\":\"fill-0\","
        "\"session\":\"0123456789abcdef\",\"authority_epoch\":99}";
    argus_mqtt_broker_message_t redelivered = seam_message(
        topics.command_release_authority, fill_zero_again, &panel);
    argus_mqtt_authority_outcome_t redelivered_out = argus_mqtt_authority_admit(
        &core, &redelivered, true, P_ALONE, false, false, 220U);
    CHECK(redelivered_out.stage == ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_STALE_EPOCH);
    CHECK(seam_unchanged(&before, &core));
    return ESP_OK;
}

esp_err_t test_4c_seam_authority_result_topic_and_schema(void)
{
    // §2: the result is published on event/core/authority_result, never on
    // event/pump1/command_result, and carries every field A2.4 requires -
    // including core_lease_status and local_control_status, which the
    // pre-correction result omitted entirely.
    CHECK(seam_topics_build() == ESP_OK);
    argus_mqtt_runtime_reset_duplicate_cache();
    CHECK(strcmp(topics.event_authority_result,
                 "argus/paladin/pump_001/event/core/authority_result") == 0);
    CHECK(strcmp(topics.event_authority_result, topics.command_result) != 0);

    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_machine_principal_t panel = seam_principal(
        "m-panel", HMI_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    argus_mqtt_broker_message_t message = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &panel);
    argus_mqtt_authority_outcome_t out = argus_mqtt_authority_admit(
        &core, &message, false, P_ALONE, false, false, 100U);
    CHECK(out.request_result == ARGUS_MQTT_AUTHORITY_GRANTED);

    CHECK(strstr(out.result_json, "\"schema\":1") != NULL);
    CHECK(strstr(out.result_json, "\"request_id\":\"req-1\"") != NULL);
    CHECK(strstr(out.result_json, "\"session\":\"0123456789abcdef\"") != NULL);
    CHECK(strstr(out.result_json, "\"outcome\":\"ACCEPTED\"") != NULL);
    CHECK(strstr(out.result_json, "\"reason\":\"granted\"") != NULL);
    CHECK(strstr(out.result_json, "\"control_owner\":\"LOCAL_HMI\"") != NULL);
    CHECK(strstr(out.result_json, "\"authority_epoch\":1") != NULL);
    CHECK(strstr(out.result_json, "\"core_lease_status\":\"ACTIVE\"") != NULL);
    CHECK(strstr(out.result_json, "\"local_control_status\":\"ACTIVE\"") != NULL);

    // A SERVICE_TOOL owner is reported distinctly - the pre-correction
    // publish_authority_result() folded it into the same default branch
    // ARGUSCORE and OTHER shared, which would have misreported it.
    argus_mqtt_session_core_t svc_core;
    argus_mqtt_session_core_init(&svc_core, SESSION);
    argus_machine_principal_t svc = seam_principal(
        "m-svc", SVC_T, SEAM_PANEL_PERMS, SEAM_SCOPE);
    argus_mqtt_broker_message_t svc_message = seam_message(
        topics.command_request_authority, SEAM_REQ_OK, &svc);
    argus_mqtt_authority_outcome_t svc_out = argus_mqtt_authority_admit(
        &svc_core, &svc_message, false, P_ALONE, false, false, 100U);
    CHECK(svc_out.request_result == ARGUS_MQTT_AUTHORITY_GRANTED);
    CHECK(strstr(svc_out.result_json, "\"control_owner\":\"SERVICE_TOOL\"") != NULL);
    return ESP_OK;
}

esp_err_t test_4c_seam_disconnected_lease_reports_truthful_ownership(void)
{
    // A2.11, the defect the correction order's §4 named directly: a
    // transport disconnect must not make published ownership say NONE (or
    // local_control_status say AVAILABLE) while the lease is still
    // unexpired. Exercises argus_mqtt_session_disconnect() (the real
    // production function) followed by the real
    // authority_status_from_core() computation, not a reimplementation of
    // either.
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(argus_mqtt_session_request_authority(&core, 11U, "m-panel", HMI_T,
              P_ALONE, false, 100U) == ARGUS_MQTT_AUTHORITY_GRANTED);
    CHECK(argus_mqtt_session_disconnect(&core, 11U));
    CHECK(core.link == ARGUS_MQTT_LINK_OFFLINE);
    CHECK(strcmp(core.lease_machine_id, "m-panel") == 0);   // lease survives

    char lease_status[16];
    char local_status[16];
    authority_status_from_core(&core, P_ALONE, false, 200U,
                               lease_status, sizeof(lease_status),
                               local_status, sizeof(local_status));
    // The fix: EXPIRING (owner known, transport down, deadline still
    // running), never NONE. Before the fix, `owned` required link ==
    // ONLINE, so this read NONE/AVAILABLE - a disconnected panel would have
    // been reported to every subscriber as if nobody held the pump, while
    // the pump was, in fact, still under a live lease.
    CHECK(strcmp(lease_status, "EXPIRING") == 0);
    CHECK(strcmp(local_status, "ACTIVE") == 0);

    // Once the deadline genuinely passes and tick() expires the lease, NONE
    // becomes the truthful answer - the distinction this fix exists to draw
    // is disconnected-but-unexpired versus actually-expired, not
    // connected-versus-not.
    CHECK(argus_mqtt_session_tick(&core, 100U + ARGUS_MQTT_HEARTBEAT_TIMEOUT_MS));
    authority_status_from_core(&core, P_ALONE, false,
                               100U + ARGUS_MQTT_HEARTBEAT_TIMEOUT_MS,
                               lease_status, sizeof(lease_status),
                               local_status, sizeof(local_status));
    CHECK(strcmp(lease_status, "EXPIRED") == 0);
    CHECK(strcmp(local_status, "AVAILABLE") == 0);
    return ESP_OK;
}
