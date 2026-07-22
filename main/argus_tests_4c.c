#include "argus_tests_4c.h"

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
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 11U, &heartbeat, 100U) == ESP_OK);
    CHECK(core.link == ARGUS_MQTT_LINK_ONLINE && core.lease_connection_id == 11U);
    heartbeat.counter = 2U;
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 12U, &heartbeat, 200U) ==
          ESP_ERR_INVALID_STATE);
    CHECK(core.lease_connection_id == 11U);
    heartbeat.counter = 1U;
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 11U, &heartbeat, 200U) ==
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
    CHECK(argus_mqtt_session_accept_heartbeat(&isolated.core, 1U, &heartbeat, 100U) == ESP_OK);
    CHECK(!argus_mqtt_session_tick(&isolated.core, 6099U));
    CHECK(argus_mqtt_session_tick(&isolated.core, 6100U));
    CHECK(isolated.core.link == ARGUS_MQTT_LINK_STALE);
    CHECK(isolated.core.lease_connection_id == 0U);
    CHECK(isolated.core.heartbeat_counter == 1U);
    CHECK(argus_mqtt_session_accept_heartbeat(
              &isolated.core, 1U, &heartbeat, 6200U) == ESP_ERR_INVALID_STATE);
    heartbeat.counter = 2U;
    CHECK(argus_mqtt_session_accept_heartbeat(
              &isolated.core, 1U, &heartbeat, 6200U) == ESP_OK);
    CHECK(isolated.motion_sentinel == 0x12345678U);
    CHECK(isolated.state_sentinel == 0x87654321U);
    return ESP_OK;
}

esp_err_t test_4c_disconnect_releases_matching_lease(void)
{
    argus_mqtt_session_core_t core;
    argus_mqtt_session_core_init(&core, SESSION);
    argus_mqtt_heartbeat_t heartbeat = {.counter = 1U};
    strlcpy(heartbeat.session, SESSION, sizeof(heartbeat.session));
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 7U, &heartbeat, 0U) == ESP_OK);
    CHECK(!argus_mqtt_session_disconnect(&core, 8U));
    CHECK(core.link == ARGUS_MQTT_LINK_ONLINE);
    CHECK(argus_mqtt_session_disconnect(&core, 7U));
    CHECK(core.link == ARGUS_MQTT_LINK_OFFLINE && core.lease_connection_id == 0U);

    argus_mqtt_session_core_init(&core, SESSION);
    CHECK(argus_mqtt_session_accept_heartbeat(&core, 9U, &heartbeat, 100U) == ESP_OK);
    CHECK(argus_mqtt_session_tick(&core, 6100U));
    CHECK(core.link == ARGUS_MQTT_LINK_STALE && core.lease_connection_id == 0U);
    CHECK(core.heartbeat_connection_id == 9U);
    CHECK(argus_mqtt_session_disconnect(&core, 9U));
    CHECK(core.link == ARGUS_MQTT_LINK_OFFLINE);
    CHECK(core.heartbeat_connection_id == 0U && core.heartbeat_counter == 0U);
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
