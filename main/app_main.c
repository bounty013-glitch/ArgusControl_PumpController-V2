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
#include "argus_tests.h"

// ================= APP CONFIG =================
#define UNIT_NAME       "ArgusMotorTest"
#define WIFI_HOST       "ArgusMotorTestNode"

#ifdef CONFIG_ARGUS_WIFI_SSID
#define WIFI_SSID       CONFIG_ARGUS_WIFI_SSID
#else
#define WIFI_SSID       ""
#endif

#ifdef CONFIG_ARGUS_WIFI_PASSWORD
#define WIFI_PASS       CONFIG_ARGUS_WIFI_PASSWORD
#else
#define WIFI_PASS       ""
#endif

#define WIFI_AP_SSID    "ArgusMotorTest"
#define WIFI_AP_PASS    ""                  // Default empty/non-secret AP password
#define WIFI_AP_CHANNEL 6U
#define WIFI_AP_MAX_CONN 4U

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

#define WIFI_STA_CONNECTED_BIT BIT0
static EventGroupHandle_t wifi_event_group;

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

// ================= COMMAND ARBITRATION =================

static void handle_mqtt_command(const char *topic, const char *data, bool retain)
{
    ESP_LOGI(TAG, "MQTT RX topic=[%s] payload=[%s] retain=%d", topic, data, retain);

    if (argus_cmd_parser_validate_control_message(topic, data, retain) != ESP_OK) {
        ESP_LOGW(TAG, "Command rejected: Retained control payload ignored on topic %s", topic);
        return;
    }

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

        esp_err_t err = argus_state_mgr_set_target(rpm_milli, true);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Setpoint updated: %d%% (%ld mRPM)", pct, (long)rpm_milli);
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
        if (run_val) {
            argus_state_mgr_start();
        } else {
            argus_state_mgr_stop_normal();
        }
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
            argus_state_mgr_stop_normal();
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
        if (estop_val) {
            argus_state_mgr_estop();
        } else {
            // e_stop=false maps to reset_estop
            argus_state_mgr_reset_estop();
        }
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
            argus_state_mgr_reset_estop();
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
            argus_state_mgr_unlock();
        } else {
            ESP_LOGI(TAG, "unlock=false payload parsed successfully: no-op.");
        }
        publish_status();
        return;
    }
}

// ================= WIFI =================

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (strlen(WIFI_SSID) > 0) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_STA_CONNECTED_BIT);
        if (strlen(WIFI_SSID) > 0) {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI("WIFI", "Station got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_STA_CONNECTED_BIT);
    }
}

static void wifi_start_access_point_and_station(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(wifi_event_group != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    esp_netif_ip_info_t ap_ip = {0};
    IP4_ADDR(&ap_ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
    
    esp_err_t dhcp_err = esp_netif_dhcps_stop(ap_netif);
    if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_ERROR_CHECK(dhcp_err);
    }
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ap_ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    wifi_config_t ap_config = {
        .ap = {
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    strlcpy((char *)ap_config.ap.ssid, WIFI_AP_SSID, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, WIFI_AP_PASS, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(WIFI_AP_SSID);

    if (strlen(WIFI_AP_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, WIFI_HOST));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI("WIFI", "Access Point and Station initialized.");
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

            printf("\n");
            printf("===================================================\n");
            printf("=== Argus V2 Pump Controller Diagnostic Menu ===\n");
            printf("===================================================\n");
            printf("Current State: [%s] Setpoint: [%ld mRPM] E-Stop: [%s]\n",
                   argus_state_mgr_get_state_name(snap.machine_state),
                   (long)snap.configured_target_rpm_milli,
                   snap.estop_latched ? "LATCHED" : "CLEAR");
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
            printf("[t] Run PURE unit tests (mock backend, 0 hardware touch)\n");
            printf("[H] Open HARDWARE ACCEPTANCE test submenu\n");
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

        switch (c) {
            case '1':
                argus_state_mgr_set_target(500, true);
                argus_state_mgr_start();
                break;
            case '2':
                argus_state_mgr_set_target(600, true);
                argus_state_mgr_start();
                break;
            case '3':
                argus_state_mgr_set_target(700, true);
                argus_state_mgr_start();
                break;
            case '4':
                argus_state_mgr_set_target(1000, true);
                argus_state_mgr_start();
                break;
            case '5':
                argus_state_mgr_set_target(20000, true);
                argus_state_mgr_start();
                break;
            case '6':
                argus_state_mgr_set_target(72000, true);
                argus_state_mgr_start();
                break;
            case '7':
                argus_state_mgr_set_target(100000, true);
                argus_state_mgr_start();
                break;
            case '8':
                argus_state_mgr_set_target(200000, true);
                argus_state_mgr_start();
                break;
            case 'g':
                printf("Issuing START command...\n");
                argus_state_mgr_start();
                break;
            case 's':
                printf("Issuing STOP_NORMAL command...\n");
                argus_state_mgr_stop_normal();
                break;
            case 'u':
                printf("Issuing UNLOCK command...\n");
                argus_state_mgr_unlock();
                break;
            case 'e':
                printf("Issuing E_STOP command...\n");
                argus_state_mgr_estop();
                break;
            case 'c':
                printf("Issuing RESET_ESTOP command...\n");
                argus_state_mgr_reset_estop();
                break;
            case 'r':
                printf("Issuing RECOVER command...\n");
                argus_state_mgr_recover();
                break;
            case 't':
                printf("Running PURE unit tests (mock operations, zero hardware touch)...\n");
                argus_tests_run_all();
                break;
            case 'H':
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
    ESP_LOGI(TAG, "Argus Pump Controller V2 firmware starting (Phase 3B)...");

    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize V2 Configuration
    ESP_ERROR_CHECK(argus_config_init());
    const argus_config_t *cfg = argus_config_get();

    // 3. Initialize V2 GPTimer Step Generator
    ESP_ERROR_CHECK(argus_step_gen_init(cfg));

    // 4. Initialize V2 Trajectory Linear Ramp Engine
    ESP_ERROR_CHECK(argus_trajectory_init(cfg));

    // 5. Initialize V2 Authoritative State Manager (using production motion ops)
    ESP_ERROR_CHECK(argus_state_mgr_init(cfg, NULL));

    // 6. Arm & Start Step Generator & Trajectory Periodic Task
    ESP_ERROR_CHECK(argus_step_gen_arm());
    ESP_ERROR_CHECK(argus_step_gen_start());
    ESP_ERROR_CHECK(argus_trajectory_task_start());

    // Start networking and broker under concurrent load
    wifi_start_access_point_and_station();
    mqtt_broker_start();

    // Launch background tasks
    xTaskCreate(status_task, "status_task", 4096, NULL, 5, NULL);
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
    xTaskCreate(argus_diagnostic_menu_task, "diagnostic_task", 4096, NULL, 5, NULL);
#endif

    ESP_LOGI(TAG, "V2 Pump Controller Phase 3B startup completed successfully.");
}
