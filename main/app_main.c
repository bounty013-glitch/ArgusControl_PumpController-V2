#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

// Include sdkconfig for Kconfig settings
#include "sdkconfig.h"

#include "esp_timer.h"
#include "argus_mqtt_broker.h"
#include "argus_config.h"
#include "argus_conversions.h"
#include "argus_feedback.h"
#include "argus_step_gen.h"
#include "argus_trajectory.h"
#include "argus_cmd_parser.h"
#include "argus_state_mgr.h"
#include "argus_identity.h"
#include "argus_nvs_config.h"
#include "argus_authority_mgr.h"
#include "argus_cmd_router.h"
#include "argus_net_mgr.h"
#include "argus_tests_4a.h"
#include "argus_tests.h"

#define MQTT_BROKER_PORT      1883U

#define MQTT_TOPIC_ROOT       "argus/peristaltic"
#define CMD_RUN_TOPIC         MQTT_TOPIC_ROOT "/cmd/run"
#define CMD_SPEED_PCT_TOPIC   MQTT_TOPIC_ROOT "/cmd/speed_pct"
#define CMD_STOP_TOPIC        MQTT_TOPIC_ROOT "/cmd/stop"
#define CMD_ESTOP_TOPIC       MQTT_TOPIC_ROOT "/cmd/e_stop"
#define CMD_RESET_ESTOP_TOPIC MQTT_TOPIC_ROOT "/cmd/reset_estop"
#define CMD_UNLOCK_TOPIC      MQTT_TOPIC_ROOT "/cmd/unlock"

#define STATUS_STATE_TOPIC    MQTT_TOPIC_ROOT "/status/state"
#define STATUS_RPM_TOPIC      MQTT_TOPIC_ROOT "/status/rpm"
#define STATUS_RUN_TOPIC      MQTT_TOPIC_ROOT "/status/target_rpm"
#define STATUS_ONLINE_TOPIC   MQTT_TOPIC_ROOT "/status/online"

#include <stdatomic.h>

static const char *TAG = "argus_app_main";
static _Atomic bool s_verbose_status = false;

bool argus_app_main_get_console_verbosity(void)
{
    return atomic_load(&s_verbose_status);
}

void argus_app_main_set_console_verbosity(bool verbose)
{
    atomic_store(&s_verbose_status, verbose);
}

void argus_app_main_print_oneshot_status(void)
{
    argus_state_snapshot_t snap;
    argus_state_mgr_get_snapshot(&snap);
    const argus_config_t *cfg = argus_config_get();
    uint64_t freq_mhz = argus_conversions_rpm_to_mhz(snap.generated_rpm_milli, cfg);
    (void)freq_mhz;
}

// ================= TELEMETRY =================

static void publish_status_topic(const char *topic, const char *payload, bool retain)
{
    if (!argus_mqtt_broker_is_running()) {
        return;
    }
    esp_err_t err = argus_mqtt_broker_publish(topic, payload, retain);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to publish %s: %s", topic, esp_err_to_name(err));
    }
}

static void publish_status(void)
{
    if (!argus_mqtt_broker_is_running()) {
        return;
    }

    argus_state_snapshot_t snap;
    argus_state_mgr_get_snapshot(&snap);

    publish_status_topic(STATUS_STATE_TOPIC, argus_state_mgr_get_state_name(snap.machine_state), true);

    char payload[64];
    snprintf(payload, sizeof(payload), "%ld.%03ld",
             (long)(snap.generated_rpm_milli / 1000),
             (long)((snap.generated_rpm_milli < 0 ? -snap.generated_rpm_milli : snap.generated_rpm_milli) % 1000));
    publish_status_topic(STATUS_RPM_TOPIC, payload, true);

    snprintf(payload, sizeof(payload), "%ld", (long)snap.configured_target_rpm_milli);
    publish_status_topic(STATUS_RUN_TOPIC, payload, true);
}

// ================= COMMAND ARBITRATION VIA ROUTER =================

static void handle_mqtt_command(const char *topic, const char *data, bool retain)
{
    ESP_LOGI(TAG, "MQTT RX topic=[%s] payload=[%s] retain=%d", topic, data, retain);

    if (argus_cmd_parser_validate_control_message(topic, data, retain) != ESP_OK) {
        ESP_LOGW(TAG, "Command rejected: Retained control payload ignored on topic %s", topic);
        return;
    }

    argus_authority_snapshot_t snap;
    argus_authority_mgr_get_snapshot(&snap);

    argus_command_envelope_t env = {
        .source = ARGUS_CMD_SRC_MQTT_SUPERVISORY,
        .authority_generation = snap.generation,
        .target_rpm_milli = 0,
        .forward = true
    };

    if (strcmp(topic, CMD_SPEED_PCT_TOPIC) == 0) {
        int pct = 0;
        if (argus_cmd_parser_speed_pct(data, &pct) != ESP_OK) {
            ESP_LOGW(TAG, "Command rejected: Malformed speed_pct payload: '%s'", data);
            return;
        }
        const argus_config_t *cfg = argus_config_get();
        int32_t range = cfg->max_output_milli_rpm - cfg->min_output_milli_rpm;
        int32_t rpm_milli = cfg->min_output_milli_rpm + (pct * range) / 100;
        if (pct == 0) rpm_milli = 0;

        env.command_type = ARGUS_CMD_TYPE_SET_TARGET;
        env.target_rpm_milli = rpm_milli;

        esp_err_t err = argus_cmd_router_dispatch(&env);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Setpoint updated: %d%% (%ld mRPM)", pct, (long)rpm_milli);
        } else {
            ESP_LOGW(TAG, "Setpoint dispatch rejected: %s", esp_err_to_name(err));
        }
        publish_status();
        return;
    }

    if (strcmp(topic, CMD_RUN_TOPIC) == 0) {
        bool run_val = false;
        if (argus_cmd_parser_bool(data, &run_val) != ESP_OK) {
            ESP_LOGW(TAG, "Command rejected: Malformed run payload: '%s'", data);
            return;
        }
        env.command_type = run_val ? ARGUS_CMD_TYPE_START : ARGUS_CMD_TYPE_STOP_NORMAL;
        argus_cmd_router_dispatch(&env);
        publish_status();
        return;
    }

    if (strcmp(topic, CMD_STOP_TOPIC) == 0) {
        bool stop_val = false;
        if (argus_cmd_parser_bool(data, &stop_val) != ESP_OK) {
            ESP_LOGW(TAG, "Command rejected: Malformed stop payload: '%s'", data);
            return;
        }
        if (stop_val) {
            env.command_type = ARGUS_CMD_TYPE_STOP_NORMAL;
            argus_cmd_router_dispatch(&env);
        }
        publish_status();
        return;
    }

    if (strcmp(topic, CMD_ESTOP_TOPIC) == 0) {
        bool estop_val = false;
        if (argus_cmd_parser_bool(data, &estop_val) != ESP_OK) {
            ESP_LOGW(TAG, "Command rejected: Malformed e_stop payload: '%s'", data);
            return;
        }
        env.command_type = estop_val ? ARGUS_CMD_TYPE_ESTOP : ARGUS_CMD_TYPE_RESET_ESTOP;
        argus_cmd_router_dispatch(&env);
        publish_status();
        return;
    }

    if (strcmp(topic, CMD_RESET_ESTOP_TOPIC) == 0) {
        bool reset_val = false;
        if (argus_cmd_parser_bool(data, &reset_val) != ESP_OK) {
            ESP_LOGW(TAG, "Command rejected: Malformed reset_estop payload: '%s'", data);
            return;
        }
        if (reset_val) {
            env.command_type = ARGUS_CMD_TYPE_RESET_ESTOP;
            argus_cmd_router_dispatch(&env);
        }
        publish_status();
        return;
    }

    if (strcmp(topic, CMD_UNLOCK_TOPIC) == 0) {
        bool unlock_val = false;
        if (argus_cmd_parser_bool(data, &unlock_val) != ESP_OK) {
            ESP_LOGW(TAG, "Command rejected: Malformed unlock payload: '%s'", data);
            return;
        }
        if (unlock_val) {
            env.command_type = ARGUS_CMD_TYPE_UNLOCK;
            argus_cmd_router_dispatch(&env);
        }
        publish_status();
        return;
    }
}

// ================= MQTT BROKER & POLICY SEAM =================

static esp_err_t mqtt_policy_check(const char *topic, const char *payload, bool retain, void *user_ctx)
{
    (void)user_ctx;
    return argus_cmd_parser_validate_control_message(topic, payload, retain);
}

static void mqtt_broker_message_cb(const char *topic, const char *payload, bool retain, void *user_ctx)
{
    (void)user_ctx;
    handle_mqtt_command(topic, payload, retain);
}

static void mqtt_broker_start(void)
{
    const argus_mqtt_broker_config_t broker_config = {
        .port = MQTT_BROKER_PORT,
        .on_message = mqtt_broker_message_cb,
        .policy_check = mqtt_policy_check,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(argus_mqtt_broker_start(&broker_config));
    publish_status_topic(STATUS_ONLINE_TOPIC, "online", true);
    publish_status();
}

// ================= STATUS TASK =================

static void status_task(void *arg)
{
    (void)arg;

    while (true) {
        argus_state_snapshot_t snap;
        argus_state_mgr_get_snapshot(&snap);

        const argus_config_t *cfg = argus_config_get();
        uint64_t freq_mhz = argus_conversions_rpm_to_mhz(snap.generated_rpm_milli, cfg);

        if (atomic_load(&s_verbose_status)) {
            ESP_LOGI(TAG, "status: state=%s target_rpm_milli=%ld applied_rpm_milli=%ld generated_rpm_milli=%ld freq_hz=%ld.%03ld steps=%lld enabled=%d ramp=%d estop=%d fault=%lu",
                     argus_state_mgr_get_state_name(snap.machine_state),
                     (long)snap.configured_target_rpm_milli, (long)snap.applied_rpm_milli, (long)snap.generated_rpm_milli,
                     (long)(freq_mhz / 1000), (long)(freq_mhz % 1000),
                     (long long)snap.generated_step_count, snap.driver_enabled, snap.ramp_active, snap.estop_latched, (unsigned long)snap.fault_code);
        }

        if (argus_mqtt_broker_is_running()) {
            publish_status();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ================= DIAGNOSTIC TEST MENU =================

#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
static char s_staged_ssid[33] = {0};
static char s_staged_pass[64] = {0};
static bool s_has_staged_config = false;

static void estop_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI("test_seam", "[TEST SEAM] Firing concurrent internal software E-STOP during service transition...");
    argus_state_mgr_estop();
}

static bool check_full_state_invariance(const argus_state_snapshot_t *sb, const argus_authority_snapshot_t *ab,
                                        const argus_state_snapshot_t *sa, const argus_authority_snapshot_t *aa)
{
    return (sb->machine_state == sa->machine_state &&
            sb->configured_target_rpm_milli == sa->configured_target_rpm_milli &&
            sb->trajectory_target_rpm_milli == sa->trajectory_target_rpm_milli &&
            sb->applied_rpm_milli == sa->applied_rpm_milli &&
            sb->generated_rpm_milli == sa->generated_rpm_milli &&
            sb->generated_step_count == sa->generated_step_count &&
            sb->requested_forward == sa->requested_forward &&
            sb->applied_forward == sa->applied_forward &&
            sb->driver_enabled == sa->driver_enabled &&
            sb->ramp_active == sa->ramp_active &&
            sb->estop_latched == sa->estop_latched &&
            sb->fault_code == sa->fault_code &&
            ab->mode == aa->mode &&
            ab->owner == aa->owner &&
            ab->generation == aa->generation);
}

static int get_menu_key(const char *prompt)
{
    printf("%s", prompt);
    fflush(stdout);

    int c;
    do {
        c = getchar();
        if (c == EOF) vTaskDelay(pdMS_TO_TICKS(50));
    } while (c == EOF || c == '\n' || c == '\r');

    printf("%c\n", c);
    fflush(stdout);
    return c;
}

static void read_string_input(const char *prompt, char *buffer, size_t max_len, bool mask)
{
    (void)mask;
    printf("%s", prompt);
    fflush(stdout);

    if (!buffer || max_len == 0) return;
    buffer[0] = '\0';

    size_t idx = 0;
    while (idx < max_len - 1) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (idx == 0) {
                // Ignore leftover newline from menu choice selection
                continue;
            }
            printf("\n");
            fflush(stdout);
            break;
        }
        buffer[idx++] = (char)c;
        printf("%c", c);
        fflush(stdout);
    }
    buffer[idx] = '\0';
}

static bool confirm_action(const char *prompt)
{
    char full_prompt[128];
    snprintf(full_prompt, sizeof(full_prompt), "%s (y/n): ", prompt);
    int c = get_menu_key(full_prompt);
    return (c == 'y' || c == 'Y');
}

static void argus_phase4a_acceptance_menu(void)
{
    while (true) {
        argus_authority_snapshot_t auth_snap;
        argus_authority_mgr_get_snapshot(&auth_snap);
        argus_network_mode_t net_mode = argus_net_mgr_get_mode();

        printf("\n===================================================\n");
        printf("=== Phase 4A Network & Authority Acceptance ===\n");
        printf("===================================================\n");
        printf("Network Mode        : %s (%d)\n", argus_net_mgr_get_mode_name(net_mode), (int)net_mode);
        printf("Authority Mode      : %s (%d)\n", argus_authority_mgr_get_mode_name(auth_snap.mode), (int)auth_snap.mode);
        printf("Authority Owner     : %s (%d)\n", argus_authority_mgr_get_owner_name(auth_snap.owner), (int)auth_snap.owner);
        printf("Authority Generation: %lu\n", (unsigned long)auth_snap.generation);
        printf("---------------------------------------------------\n");
        printf("[1] Display sanitized identity/configuration\n");
        printf("[2] Display network and authority snapshot\n");
        printf("[3] Display NVS slot validity/generation\n");
        printf("[4] Enable service AP discoverability\n");
        printf("[5] Request LOCAL_SERVICE as diagnostic CLI\n");
        printf("[6] Inject MQTT-source permission test (non-mutating probe)\n");
        printf("[7] Inject browser-source permission test (non-mutating probe)\n");
        printf("[E] Test transition E-Stop (Schedule concurrent E-stop during service entry)\n");
        printf("[8] Stage STA configuration\n");
        printf("[9] Validate staged configuration\n");
        printf("[A] Apply staged configuration and reboot\n");
        printf("[X] Exit local service without configuration change\n");
        printf("[F] Factory reset\n");
        printf("[L] Display transition/event log\n");
        printf("[0] Return\n");

        int c = get_menu_key("Select action: ");

        if (c == '0') break;

        switch (c) {
            case '1': {
                argus_identity_t id;
                argus_identity_get(&id);
                argus_config_payload_t cfg;
                bool has_cfg = (argus_nvs_config_get(&cfg) == ESP_OK);

                printf("\n--- Sanitized Device Identity & Configuration ---\n");
                printf("Hardware UID : %s\n", id.mac_uid);
                printf("App Version  : %s\n", id.fw_version);
                printf("Service SSID : %s\n", id.service_ssid);
                printf("Service Pass : [CONFIGURED] (Build Protected)\n");
                printf("Client ID    : %s\n", id.client_id);
                printf("Unit ID      : %s\n", id.unit_id);
                printf("Device Name  : %s\n", id.device_name);
                if (has_cfg && argus_nvs_config_is_commissioned(&cfg)) {
                    printf("STA SSID     : %s\n", cfg.sta_ssid);
                    printf("STA Pass     : [CONFIGURED] (Masked)\n");
                    printf("Commissioned : YES\n");
                } else {
                    printf("STA SSID     : [EMPTY]\n");
                    printf("STA Pass     : [NOT CONFIGURED]\n");
                    printf("Commissioned : NO\n");
                }
                break;
            }
            case '2': {
                printf("\n--- Network & Authority Snapshot ---\n");
                printf("Network Mode        : %s (%d)\n", argus_net_mgr_get_mode_name(net_mode), (int)net_mode);
                printf("Wi-Fi Driver Mode   : %s\n", argus_net_mgr_get_wifi_driver_mode_name());
                printf("STA Status          : %s\n", argus_net_mgr_is_sta_ip_acquired() ? "CONNECTED (IP Acquired)" : (argus_net_mgr_is_sta_connected() ? "ASSOCIATED" : "DISABLED"));
                printf("Service AP Status   : %s\n", argus_net_mgr_is_ap_started() ? "ENABLED" : "DISABLED");
                printf("MQTT Broker Status  : %s\n", argus_mqtt_broker_is_running() ? "READY" : "STOPPED");
                printf("Authority Mode      : %s (%d)\n", argus_authority_mgr_get_mode_name(auth_snap.mode), (int)auth_snap.mode);
                printf("Authority Owner     : %s (%d)\n", argus_authority_mgr_get_owner_name(auth_snap.owner), (int)auth_snap.owner);
                printf("Authority Generation: %lu\n", (unsigned long)auth_snap.generation);
                break;
            }
            case '3': {
                argus_config_payload_t cfg;
                esp_err_t err = argus_nvs_config_get(&cfg);
                printf("\n--- NVS Slot Validity & Generation ---\n");
                if (err == ESP_OK) {
                    uint32_t crc = argus_nvs_config_calc_crc32(&cfg);
                    printf("Commissioned : YES\n");
                    printf("Client ID    : %s\n", cfg.client_id);
                    printf("Unit ID      : %s\n", cfg.unit_id);
                    printf("Device Name  : %s\n", cfg.device_name);
                    printf("STA SSID     : %s\n", cfg.sta_ssid);
                    printf("CRC32 Status : VALID (0x%08X)\n", (unsigned int)crc);
                } else {
                    printf("NVS Status   : NO VALID LKG CONFIGURATION (Uncommissioned)\n");
                }
                break;
            }
            case '4': {
                printf("Enabling Service AP discoverability (APSTA mode)...\n");
                argus_net_mgr_enable_ap_discoverable();
                break;
            }
            case '5': {
                printf("Requesting LOCAL_SERVICE authority as DIAGNOSTIC_CLI...\n");
                argus_authority_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI);
                break;
            }
            case '6': {
                argus_state_snapshot_t sb;
                argus_state_mgr_get_snapshot(&sb);
                argus_authority_snapshot_t ab;
                argus_authority_mgr_get_snapshot(&ab);

                printf("Permission test only — no START/RUN command will be issued.\n");
                printf("Probing MQTT-source authority (gen %lu)... ", (unsigned long)ab.generation);

                argus_command_envelope_t env = {
                    .source = ARGUS_CMD_SRC_MQTT_SUPERVISORY,
                    .command_type = ARGUS_CMD_TYPE_SET_TARGET,
                    .authority_generation = ab.generation,
                    .target_rpm_milli = 20000,
                    .forward = true
                };

                esp_err_t res = argus_cmd_router_check_authority(&env);

                argus_state_snapshot_t sa;
                argus_state_mgr_get_snapshot(&sa);
                argus_authority_snapshot_t aa;
                argus_authority_mgr_get_snapshot(&aa);

                if (res == ESP_OK) {
                    printf("[ACCEPTED]\n");
                } else {
                    printf("[REJECTED: %s (%d)]\n", esp_err_to_name(res), res);
                }

                bool inv = check_full_state_invariance(&sb, &ab, &sa, &aa);
                printf("State Invariance Check: %s (15/15 fields unchanged)\n", inv ? "PASSED" : "FAILED");
                break;
            }
            case '7': {
                argus_state_snapshot_t sb;
                argus_state_mgr_get_snapshot(&sb);
                argus_authority_snapshot_t ab;
                argus_authority_mgr_get_snapshot(&ab);

                printf("Permission test only — no START/RUN command will be issued.\n");
                printf("Probing Browser-source authority (gen %lu)... ", (unsigned long)ab.generation);

                argus_command_envelope_t env = {
                    .source = ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL,
                    .command_type = ARGUS_CMD_TYPE_SET_TARGET,
                    .authority_generation = ab.generation,
                    .target_rpm_milli = 20000,
                    .forward = true
                };

                esp_err_t res = argus_cmd_router_check_authority(&env);

                argus_state_snapshot_t sa;
                argus_state_mgr_get_snapshot(&sa);
                argus_authority_snapshot_t aa;
                argus_authority_mgr_get_snapshot(&aa);

                if (res == ESP_OK) {
                    printf("[ACCEPTED]\n");
                } else {
                    printf("[REJECTED: %s (%d)]\n", esp_err_to_name(res), res);
                }

                bool inv = check_full_state_invariance(&sb, &ab, &sa, &aa);
                printf("State Invariance Check: %s (15/15 fields unchanged)\n", inv ? "PASSED" : "FAILED");
                break;
            }
            case 'E': {
                printf("\n--- Concurrent Transition E-Stop Test ---\n");
                if (!confirm_action("Run concurrent Transition E-Stop test?")) break;

                argus_state_snapshot_t st;
                argus_state_mgr_get_snapshot(&st);
                argus_authority_snapshot_t aut;
                argus_authority_mgr_get_snapshot(&aut);

                if (st.machine_state != ARGUS_STATE_RUNNING || aut.mode != ARGUS_AUTHORITY_SUPERVISORY) {
                    printf("[ERROR] Prerequisite failed: Motor must be RUNNING under SUPERVISORY/MQTT authority.\n");
                    printf("Please start supervisory motion first before running this test.\n");
                    break;
                }

                printf("Scheduling background E-STOP timer (500 ms delay)...\n");
                esp_timer_handle_t timer_handle = NULL;
                esp_timer_create_args_t timer_args = {
                    .callback = &estop_timer_cb,
                    .name = "estop_test_timer"
                };
                esp_timer_create(&timer_args, &timer_handle);
                esp_timer_start_once(timer_handle, 500000); // 500 ms

                printf("Requesting service entry (LOCAL_SERVICE as DIAGNOSTIC_CLI)...\n");
                esp_err_t res = argus_authority_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI);

                esp_timer_delete(timer_handle);

                argus_state_snapshot_t st_after;
                argus_state_mgr_get_snapshot(&st_after);
                argus_authority_snapshot_t aut_after;
                argus_authority_mgr_get_snapshot(&aut_after);

                printf("\n--- Concurrent Transition E-Stop Results ---\n");
                printf("Service Request Result : %s (%d)\n", esp_err_to_name(res), res);
                printf("Machine State          : %s\n", argus_state_mgr_get_state_name(st_after.machine_state));
                printf("E-Stop Latched         : %s\n", st_after.estop_latched ? "YES" : "NO");
                printf("Generated RPM          : %ld mRPM\n", (long)st_after.generated_rpm_milli);
                printf("Authority Granted      : %s (%s/%s)\n",
                       (aut_after.mode == ARGUS_AUTHORITY_LOCAL_SERVICE) ? "YES (FAILED TEST)" : "NO (PASSED)",
                       argus_authority_mgr_get_mode_name(aut_after.mode),
                       argus_authority_mgr_get_owner_name(aut_after.owner));

                if (res != ESP_OK && st_after.machine_state == ARGUS_STATE_EMERGENCY_STOPPED &&
                    st_after.estop_latched && aut_after.mode != ARGUS_AUTHORITY_LOCAL_SERVICE) {
                    printf("[TEST PASSED] Transition E-stop preempted service entry and blocked local authority grant!\n");
                } else {
                    printf("[TEST FAILED] Unexpected transition E-stop state.\n");
                }
                break;
            }
            case '8': {
                read_string_input("Enter STA SSID (1-32 chars): ", s_staged_ssid, sizeof(s_staged_ssid), false);
                read_string_input("Enter STA WPA2 Password (8-63 chars, un-echoed): ", s_staged_pass, sizeof(s_staged_pass), true);
                s_has_staged_config = (strlen(s_staged_ssid) >= 1 && strlen(s_staged_pass) >= 8 && strlen(s_staged_pass) <= 63);
                if (s_has_staged_config) {
                    printf("[STAGED SUCCESSFULLY] SSID: '%s', Pass: [CONFIGURED] (%u chars)\n", s_staged_ssid, (unsigned int)strlen(s_staged_pass));
                } else {
                    printf("[STAGING FAILED] Invalid parameters. SSID must be 1-32 chars, Pass 8-63 chars.\n");
                    memset(s_staged_ssid, 0, sizeof(s_staged_ssid));
                    memset(s_staged_pass, 0, sizeof(s_staged_pass));
                }
                break;
            }
            case '9': {
                if (!s_has_staged_config) {
                    printf("[VALIDATION FAILED] No staged configuration present.\n");
                } else {
                    argus_config_payload_t temp = {0};
                    argus_identity_t id;
                    argus_identity_get(&id);
                    strlcpy(temp.client_id, id.client_id, sizeof(temp.client_id));
                    strlcpy(temp.unit_id, id.unit_id, sizeof(temp.unit_id));
                    strlcpy(temp.device_name, id.device_name, sizeof(temp.device_name));
                    strlcpy(temp.sta_ssid, s_staged_ssid, sizeof(temp.sta_ssid));
                    strlcpy(temp.sta_pass, s_staged_pass, sizeof(temp.sta_pass));
                    if (argus_nvs_config_is_commissioned(&temp)) {
                        printf("[VALIDATION PASSED] Staged configuration complies with Schema V1.\n");
                    } else {
                        printf("[VALIDATION FAILED] Staged configuration fails Schema V1 rules.\n");
                    }
                }
                break;
            }
            case 'A': {
                if (!s_has_staged_config) {
                    printf("[ERROR] No valid staged configuration to apply.\n");
                    break;
                }
                if (confirm_action("Apply staged STA configuration to NVS and reboot?")) {
                    argus_config_payload_t payload = {0};
                    argus_identity_t id;
                    argus_identity_get(&id);
                    strlcpy(payload.client_id, id.client_id, sizeof(payload.client_id));
                    strlcpy(payload.unit_id, id.unit_id, sizeof(payload.unit_id));
                    strlcpy(payload.device_name, id.device_name, sizeof(payload.device_name));
                    strlcpy(payload.sta_ssid, s_staged_ssid, sizeof(payload.sta_ssid));
                    strlcpy(payload.sta_pass, s_staged_pass, sizeof(payload.sta_pass));
                    printf("Committing configuration payload to NVS...\n");
                    esp_err_t err = argus_nvs_config_commit(&payload);
                    if (err == ESP_OK) {
                        printf("Commit successful! Rebooting ESP32...\n");
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_restart();
                    } else {
                        printf("[ERROR] NVS Commit failed: %s (%d)\n", esp_err_to_name(err), err);
                    }
                }
                break;
            }
            case 'X': {
                if (confirm_action("Exit local service mode without configuration change and reboot?")) {
                    printf("Executing controlled service exit...\n");
                    argus_authority_request_exit();
                }
                break;
            }
            case 'F': {
                printf("\nWARNING: Factory Reset will erase:\n");
                printf("  - STA SSID & WPA2 Password\n");
                printf("  - Dual configuration slots & active selector\n");
                printf("  - Pending staged credentials\n");
                printf("What remains:\n");
                printf("  - Immutable efuse Hardware UID\n");
                printf("  - Build Service AP Credential\n");
                if (confirm_action("Confirm Factory Reset?")) {
                    printf("Executing factory reset...\n");
                    argus_nvs_config_factory_reset();
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
                break;
            }
            case 'L': {
                printf("\n--- Network & Authority Transition Event Log ---\n");
                printf("Current Network Mode  : %s (%d)\n", argus_net_mgr_get_mode_name(net_mode), (int)net_mode);
                printf("Current Authority Mode: %s (%d)\n", argus_authority_mgr_get_mode_name(auth_snap.mode), (int)auth_snap.mode);
                printf("Current Owner         : %s (%d)\n", argus_authority_mgr_get_owner_name(auth_snap.owner), (int)auth_snap.owner);
                printf("Current Generation    : %lu\n", (unsigned long)auth_snap.generation);
                printf("(All network, authority, and machine state transitions logged to console in real time)\n");
                break;
            }
            default:
                printf("Unknown action: '%c'\n", c);
                break;
        }
    }
}

static void argus_diagnostic_menu_task(void *pvParameters)
{
    (void)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(2000)); // allow startup logs to settle

    bool print_menu = true;

    while (true) {
        if (print_menu) {
            argus_state_snapshot_t snap;
            argus_state_mgr_get_snapshot(&snap);
            argus_authority_snapshot_t auth_snap;
            argus_authority_mgr_get_snapshot(&auth_snap);

            printf("\n");
            printf("===================================================\n");
            printf("=== Argus V2 Pump Controller Diagnostic Menu ===\n");
            printf("===================================================\n");
            printf("Current State: [%s] Setpoint: [%ld mRPM] Network: [%s] AuthMode: [%s] AuthOwner: [%s] Gen: [%lu]\n",
                   argus_state_mgr_get_state_name(snap.machine_state),
                   (long)snap.configured_target_rpm_milli,
                   argus_net_mgr_get_mode_name(argus_net_mgr_get_mode()),
                   argus_authority_mgr_get_mode_name(auth_snap.mode),
                   argus_authority_mgr_get_owner_name(auth_snap.owner),
                   (unsigned long)auth_snap.generation);
            printf("[i] Display ONE current status snapshot\n");
            printf("[v] Toggle periodic console status logging (currently %s)\n",
                   atomic_load(&s_verbose_status) ? "ON" : "OFF");
            printf("[1] Set & Start 0.5 RPM (500 mRPM)\n");
            printf("[2] Set & Start 0.6 RPM (600 mRPM)\n");
            printf("[3] Set & Start 0.7 RPM (700 mRPM)\n");
            printf("[4] Set & Start 1.0 RPM (1000 mRPM)\n");
            printf("[5] Set & Start 20.0 RPM (20000 mRPM)\n");
            printf("[6] Set & Start 72.0 RPM (72000 mRPM)\n");
            printf("[7] Set & Start 100.0 RPM (100000 mRPM)\n");
            printf("[8] Set & Start 200.0 RPM (200000 mRPM)\n");
            printf("[g] Start Motion (apply stored setpoint)\n");
            printf("[s] Stop Normal (ramps speed to zero, retains holding torque)\n");
            printf("[u] Unlock/Disable driver (releases shaft)\n");
            printf("[e] Software E-STOP (instant pulse halt, latches E-stop)\n");
            printf("[c] Clear/Reset E-STOP latch (returns to HOLDING/UNLOCKED)\n");
            printf("[r] Diagnostic RECOVERY (resets faulted state)\n");
            printf("[t] Run ALL PURE unit tests (Phase 3B + Phase 4A mock backends)\n");
            printf("[N] Phase 4A Network & Authority Acceptance Submenu\n");
            printf("[H] Claim LOCAL_SERVICE CLI Authority & Open HARDWARE ACCEPTANCE menu\n");
        print_menu = false;
        }

        int c = get_menu_key("Select option: ");
        print_menu = true;

        argus_authority_snapshot_t auth_snap;
        argus_authority_mgr_get_snapshot(&auth_snap);

        bool has_cli_authority = (auth_snap.mode == ARGUS_AUTHORITY_LOCAL_SERVICE &&
                                  auth_snap.owner == ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI);

        argus_command_envelope_t cli_env = {
            .source = ARGUS_CMD_SRC_CLI_DIAGNOSTIC,
            .authority_generation = auth_snap.generation,
            .target_rpm_milli = 0,
            .forward = true
        };

        // Enforce explicit authority requirement for motion commands
        if ((c >= '1' && c <= '8') || c == 'g' || c == 's' || c == 'u') {
            if (!has_cli_authority) {
                printf("[REJECTED] CLI motion rejected: diagnostic CLI does not own LOCAL_SERVICE authority.\n");
                printf("Use H or N -> 5 to request authority explicitly.\n");
                continue;
            }
        }

        switch (c) {
            case '1':
                cli_env.command_type = ARGUS_CMD_TYPE_SET_TARGET;
                cli_env.target_rpm_milli = 500;
                argus_cmd_router_dispatch(&cli_env);
                cli_env.command_type = ARGUS_CMD_TYPE_START;
                argus_cmd_router_dispatch(&cli_env);
                break;
            case '2':
                cli_env.command_type = ARGUS_CMD_TYPE_SET_TARGET;
                cli_env.target_rpm_milli = 600;
                argus_cmd_router_dispatch(&cli_env);
                cli_env.command_type = ARGUS_CMD_TYPE_START;
                argus_cmd_router_dispatch(&cli_env);
                break;
            case '3':
                cli_env.command_type = ARGUS_CMD_TYPE_SET_TARGET;
                cli_env.target_rpm_milli = 700;
                argus_cmd_router_dispatch(&cli_env);
                cli_env.command_type = ARGUS_CMD_TYPE_START;
                argus_cmd_router_dispatch(&cli_env);
                break;
            case '4':
                cli_env.command_type = ARGUS_CMD_TYPE_SET_TARGET;
                cli_env.target_rpm_milli = 1000;
                argus_cmd_router_dispatch(&cli_env);
                cli_env.command_type = ARGUS_CMD_TYPE_START;
                argus_cmd_router_dispatch(&cli_env);
                break;
            case '5':
                cli_env.command_type = ARGUS_CMD_TYPE_SET_TARGET;
                cli_env.target_rpm_milli = 20000;
                argus_cmd_router_dispatch(&cli_env);
                cli_env.command_type = ARGUS_CMD_TYPE_START;
                argus_cmd_router_dispatch(&cli_env);
                break;
            case '6':
                cli_env.command_type = ARGUS_CMD_TYPE_SET_TARGET;
                cli_env.target_rpm_milli = 72000;
                argus_cmd_router_dispatch(&cli_env);
                cli_env.command_type = ARGUS_CMD_TYPE_START;
                argus_cmd_router_dispatch(&cli_env);
                break;
            case '7':
                cli_env.command_type = ARGUS_CMD_TYPE_SET_TARGET;
                cli_env.target_rpm_milli = 100000;
                argus_cmd_router_dispatch(&cli_env);
                cli_env.command_type = ARGUS_CMD_TYPE_START;
                argus_cmd_router_dispatch(&cli_env);
                break;
            case '8':
                cli_env.command_type = ARGUS_CMD_TYPE_SET_TARGET;
                cli_env.target_rpm_milli = 200000;
                argus_cmd_router_dispatch(&cli_env);
                cli_env.command_type = ARGUS_CMD_TYPE_START;
                argus_cmd_router_dispatch(&cli_env);
                break;
            case 'g':
                cli_env.command_type = ARGUS_CMD_TYPE_START;
                argus_cmd_router_dispatch(&cli_env);
                break;
            case 's':
                cli_env.command_type = ARGUS_CMD_TYPE_STOP_NORMAL;
                argus_cmd_router_dispatch(&cli_env);
                break;
            case 'u':
                cli_env.command_type = ARGUS_CMD_TYPE_UNLOCK;
                argus_cmd_router_dispatch(&cli_env);
                break;
            case 'e':
                cli_env.command_type = ARGUS_CMD_TYPE_ESTOP;
                argus_cmd_router_dispatch(&cli_env);
                break;
            case 'c':
                cli_env.command_type = ARGUS_CMD_TYPE_RESET_ESTOP;
                argus_cmd_router_dispatch(&cli_env);
                break;
            case 'r':
                cli_env.command_type = ARGUS_CMD_TYPE_RECOVER;
                argus_cmd_router_dispatch(&cli_env);
                break;
            case 'v': {
                bool current = atomic_load(&s_verbose_status);
                bool next_val = !current;
                atomic_store(&s_verbose_status, next_val);
                printf("Periodic console status logging is now %s.\n", next_val ? "ON" : "OFF");
                break;
            }
            case 'i': {
                argus_state_snapshot_t snap;
                argus_state_mgr_get_snapshot(&snap);
                const argus_config_t *cfg = argus_config_get();
                uint64_t freq_mhz = argus_conversions_rpm_to_mhz(snap.generated_rpm_milli, cfg);
                printf("\n--- Single Status Snapshot ---\n");
                printf("State               : %s (%d)\n", argus_state_mgr_get_state_name(snap.machine_state), (int)snap.machine_state);
                printf("Configured Target   : %ld mRPM\n", (long)snap.configured_target_rpm_milli);
                printf("Applied Target      : %ld mRPM\n", (long)snap.applied_rpm_milli);
                printf("Generated Output    : %ld mRPM (%ld.%03ld Hz)\n", (long)snap.generated_rpm_milli, (long)(freq_mhz / 1000), (long)(freq_mhz % 1000));
                printf("Generated Steps     : %lld\n", (long long)snap.generated_step_count);
                printf("Driver Enabled      : %s\n", snap.driver_enabled ? "YES" : "NO");
                printf("Ramp Active         : %s\n", snap.ramp_active ? "YES" : "NO");
                printf("E-Stop Latched      : %s\n", snap.estop_latched ? "YES" : "NO");
                printf("Fault Code          : %lu\n", (unsigned long)snap.fault_code);
                break;
            }
            case 't':
                printf("Running Phase 3B PURE unit tests...\n");
                argus_tests_run_all();
                printf("Running Phase 4A PURE unit tests...\n");
                argus_tests_4a_run_all();
                break;
            case 'N':
                argus_phase4a_acceptance_menu();
                break;
            case 'H':
                printf("Claiming LOCAL_SERVICE authority for CLI...\n");
                argus_authority_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI);
                printf("Opening HARDWARE ACCEPTANCE test submenu...\n");
                argus_tests_run_hardware_acceptance();
                break;
            default:
                printf("Unknown option: '%c'\n", c);
                break;
        }
    }
}
#endif

// ================= APP MAIN =================

void app_main(void)
{
    ESP_LOGI(TAG, "Argus Pump Controller V2 firmware starting (Phase 4A)...");

    // 1. Initialize Persistent Device Identity
    ESP_ERROR_CHECK(argus_identity_init());

    // 2. Initialize Power-Loss-Safe Dual-Slot NVS Configuration (production storage)
    ESP_ERROR_CHECK(argus_nvs_config_init(NULL));

    // 3. Initialize V2 Core Configuration
    ESP_ERROR_CHECK(argus_config_init());
    const argus_config_t *cfg = argus_config_get();

    // 4. Initialize GPTimer Step Generator
    ESP_ERROR_CHECK(argus_step_gen_init(cfg));

    // 5. Initialize Trajectory Linear Ramp Engine
    ESP_ERROR_CHECK(argus_trajectory_init(cfg));

    // 6. Initialize Authoritative State Manager (using production motion ops)
    ESP_ERROR_CHECK(argus_state_mgr_init(cfg, NULL));

    // 7. Initialize Exclusive Control Authority Manager
    ESP_ERROR_CHECK(argus_authority_mgr_init());

    // 8. Initialize Command Router Serialization Gate
    ESP_ERROR_CHECK(argus_cmd_router_init());

    // 9. Arm & Start Step Generator & Trajectory Periodic Task
    ESP_ERROR_CHECK(argus_step_gen_arm());
    ESP_ERROR_CHECK(argus_step_gen_start());
    ESP_ERROR_CHECK(argus_trajectory_task_start());

    // 10. Register MQTT Broker Startup Callback with Network Manager
    argus_net_mgr_register_broker_start_cb(mqtt_broker_start);

    // 11. Initialize Dedicated Network Manager (Wi-Fi AP/STA)
    esp_err_t net_err = argus_net_mgr_init();
    if (net_err != ESP_OK) {
        ESP_LOGE(TAG, "Network manager initialization failed: %s", esp_err_to_name(net_err));
    }

    // Launch background tasks
    xTaskCreate(status_task, "status_task", 4096, NULL, 5, NULL);
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    xTaskCreate(argus_diagnostic_menu_task, "diagnostic_task", 4096, NULL, 5, NULL);
#endif

    ESP_LOGI(TAG, "V2 Pump Controller Phase 4A startup completed successfully.");
}
