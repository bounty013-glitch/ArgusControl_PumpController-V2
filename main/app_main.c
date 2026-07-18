#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static const char *TAG = "argus_app_main";

// ================= TELEMETRY =================

static void publish_status_topic(const char *topic, const char *payload, bool retain)
{
    esp_err_t err = argus_mqtt_broker_publish(topic, payload, retain);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to publish %s: %s", topic, esp_err_to_name(err));
    }
}

static void publish_status(void)
{
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

        ESP_LOGI(TAG, "status: state=%s target_rpm_milli=%ld applied_rpm_milli=%ld generated_rpm_milli=%ld freq_hz=%ld.%03ld steps=%lld enabled=%d ramp=%d estop=%d fault=%lu",
                 argus_state_mgr_get_state_name(snap.machine_state),
                 (long)snap.configured_target_rpm_milli, (long)snap.applied_rpm_milli, (long)snap.generated_rpm_milli,
                 (long)(freq_mhz / 1000), (long)(freq_mhz % 1000),
                 (long long)snap.generated_step_count, snap.driver_enabled, snap.ramp_active, snap.estop_latched, (unsigned long)snap.fault_code);

        publish_status();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ================= DIAGNOSTIC TEST MENU =================

#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
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
            printf("Current State: [%s] Setpoint: [%ld mRPM] AuthMode: [%d] AuthOwner: [%d]\n",
                   argus_state_mgr_get_state_name(snap.machine_state),
                   (long)snap.configured_target_rpm_milli,
                   (int)auth_snap.mode, (int)auth_snap.owner);
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
            printf("[H] Claim LOCAL_SERVICE CLI Authority & Open HARDWARE ACCEPTANCE menu\n");
            printf("Select option: ");
            fflush(stdout);
            print_menu = false;
        }

        int c;
        do {
            c = getchar();
            if (c == EOF) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        } while (c == EOF || c == '\n' || c == '\r');

        printf("%c\n", c);
        print_menu = true;

        argus_authority_snapshot_t auth_snap;
        argus_authority_mgr_get_snapshot(&auth_snap);

        if (c != 't') {
            if (auth_snap.mode != ARGUS_AUTHORITY_LOCAL_SERVICE || auth_snap.owner != ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI) {
                printf("[CLI] Claiming LOCAL_SERVICE authority for Diagnostic CLI...\n");
                argus_authority_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI);
                argus_authority_mgr_get_snapshot(&auth_snap);
            }
        }

        argus_command_envelope_t cli_env = {
            .source = ARGUS_CMD_SRC_CLI_DIAGNOSTIC,
            .authority_generation = auth_snap.generation,
            .target_rpm_milli = 0,
            .forward = true
        };

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
            case 't':
                printf("Running Phase 3B PURE unit tests...\n");
                argus_tests_run_all();
                printf("Running Phase 4A PURE unit tests...\n");
                argus_tests_4a_run_all();
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

    // 10. Initialize Dedicated Network Manager (Wi-Fi AP/STA)
    esp_err_t net_err = argus_net_mgr_init();
    if (net_err == ESP_OK) {
        mqtt_broker_start();
    } else {
        ESP_LOGE(TAG, "Network manager initialization failed: %s", esp_err_to_name(net_err));
    }

    // Launch background tasks
    xTaskCreate(status_task, "status_task", 4096, NULL, 5, NULL);
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    xTaskCreate(argus_diagnostic_menu_task, "diagnostic_task", 4096, NULL, 5, NULL);
#endif

    ESP_LOGI(TAG, "V2 Pump Controller Phase 4A startup completed successfully.");
}
