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
#define CMD_UNLOCK_TOPIC      MQTT_TOPIC_ROOT "/cmd/unlock"

#define STATUS_RUN_TOPIC      MQTT_TOPIC_ROOT "/status/run"
#define STATUS_RPM_TOPIC      MQTT_TOPIC_ROOT "/status/rpm"
#define STATUS_LOCKED_TOPIC   MQTT_TOPIC_ROOT "/status/locked"
#define STATUS_ESTOP_TOPIC    MQTT_TOPIC_ROOT "/status/e_stop"
#define STATUS_ONLINE_TOPIC   MQTT_TOPIC_ROOT "/status/online"

static const char *TAG = "argus_app_main";

#define WIFI_STA_CONNECTED_BIT BIT0
#define WIFI_STA_CONNECT_WAIT_MS 10000

static EventGroupHandle_t wifi_event_group;

// ================= MOTOR HELPERS =================

static void publish_status_topic(const char *topic, const char *payload, bool retain)
{
    esp_err_t err = argus_mqtt_broker_publish(topic, payload, retain);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to publish %s: %s", topic, esp_err_to_name(err));
    }
}

static void publish_status(void)
{
    int32_t generated_rpm = argus_step_gen_get_generated_rpm_milli();
    int64_t steps = argus_step_gen_get_step_count();
    
    char payload[64];
    snprintf(payload, sizeof(payload), "%ld.%03ld",
             (long)(generated_rpm / 1000),
             (long)((generated_rpm < 0 ? -generated_rpm : generated_rpm) % 1000));
    publish_status_topic(STATUS_RPM_TOPIC, payload, true);

    snprintf(payload, sizeof(payload), "%lld", (long long)steps);
    publish_status_topic(STATUS_RUN_TOPIC, payload, true);
}

static void handle_speed_pct(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    const argus_config_t *cfg = argus_config_get();
    int32_t range = cfg->max_output_milli_rpm - cfg->min_output_milli_rpm;
    int32_t rpm_milli = cfg->min_output_milli_rpm + (pct * range) / 100;
    
    if (pct == 0) {
        rpm_milli = 0;
    }

    esp_err_t err = argus_trajectory_set_target_rpm_milli(rpm_milli, true);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Speed set via MQTT: %d%% = %ld.%03ld RPM output target",
                 pct,
                 (long)(rpm_milli / 1000),
                 (long)(rpm_milli % 1000));
        publish_status();
    } else {
        ESP_LOGE(TAG, "Failed to set speed: %s", esp_err_to_name(err));
    }
}

static bool payload_is_true(const char *data)
{
    if (!data) {
        return false;
    }
    return strcmp(data, "true") == 0 ||
           strcmp(data, "1") == 0 ||
           strcmp(data, "on") == 0 ||
           strcmp(data, "ON") == 0;
}

static void handle_mqtt_command(const char *topic, const char *data)
{
    ESP_LOGI(TAG, "MQTT RX topic=[%s] payload=[%s]", topic, data);

    if (strcmp(topic, CMD_SPEED_PCT_TOPIC) == 0) {
        int pct = atoi(data);
        handle_speed_pct(pct);
        return;
    }

    if (strcmp(topic, CMD_RUN_TOPIC) == 0) {
        if (payload_is_true(data)) {
            esp_err_t err = argus_step_gen_start();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Step generator START commanded");
            } else {
                ESP_LOGE(TAG, "Failed to start: %s", esp_err_to_name(err));
            }
        } else {
            esp_err_t err = argus_trajectory_stop_normal();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Normal trajectory STOP commanded");
            } else {
                ESP_LOGE(TAG, "Failed to stop: %s", esp_err_to_name(err));
            }
        }
        publish_status();
        return;
    }

    if (strcmp(topic, CMD_STOP_TOPIC) == 0) {
        if (payload_is_true(data)) {
            argus_trajectory_stop_normal();
            publish_status();
        }
        return;
    }

    if (strcmp(topic, CMD_ESTOP_TOPIC) == 0) {
        if (payload_is_true(data)) {
            argus_trajectory_stop_immediate();
            publish_status();
            ESP_LOGW(TAG, "Software E-STOP active (pulses stopped immediately). Note: MQTT is NOT a safety-rated path.");
        }
        return;
    }

    if (strcmp(topic, CMD_UNLOCK_TOPIC) == 0) {
        esp_err_t err = argus_trajectory_unlock();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Unlock requested. Driver disabled (GPIO 5 HIGH), shaft released.");
        } else {
            ESP_LOGE(TAG, "Failed to disable driver on unlock: %s", esp_err_to_name(err));
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

// ================= MQTT BROKER =================

static void mqtt_broker_message_cb(const char *topic, const char *payload, void *user_ctx)
{
    (void)user_ctx;
    handle_mqtt_command(topic, payload);
}

static void mqtt_broker_start(void)
{
    const argus_mqtt_broker_config_t broker_config = {
        .port = MQTT_BROKER_PORT,
        .on_message = mqtt_broker_message_cb,
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
        int32_t target_rpm = argus_trajectory_get_target_rpm_milli();
        int32_t applied_rpm = argus_trajectory_get_applied_rpm_milli();
        int32_t generated_rpm = argus_step_gen_get_generated_rpm_milli();
        int64_t steps = argus_step_gen_get_step_count();
        bool driver_enabled = argus_step_gen_is_driver_enabled();
        bool ramp_active = argus_trajectory_is_ramp_active();
        argus_step_gen_error_t err = argus_step_gen_get_error();

        const argus_config_t *cfg = argus_config_get();
        uint64_t freq_mhz = argus_conversions_rpm_to_mhz(generated_rpm, cfg);

        ESP_LOGI(TAG, "status: target_rpm_milli=%ld applied_rpm_milli=%ld generated_rpm_milli=%ld freq_hz=%ld.%03ld steps=%lld enabled=%d ramp=%d err=%d",
                 (long)target_rpm, (long)applied_rpm, (long)generated_rpm,
                 (long)(freq_mhz / 1000), (long)(freq_mhz % 1000),
                 (long long)steps, driver_enabled, ramp_active, err);

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
            printf("\n");
            printf("===================================================\n");
            printf("=== Argus V2 Pump Controller Diagnostic Menu ===\n");
            printf("===================================================\n");
            printf("[1] Ramped 0.5 RPM equivalent (66.67 Hz)\n");
            printf("[2] Ramped 0.6 RPM equivalent (80.00 Hz)\n");
            printf("[3] Ramped 0.7 RPM equivalent (93.33 Hz)\n");
            printf("[4] Ramped 1.0 RPM equivalent (133.33 Hz)\n");
            printf("[5] Ramped 20.0 RPM equivalent (2.67 kHz)\n");
            printf("[6] Ramped 72.0 RPM equivalent (9.60 kHz)\n");
            printf("[7] Ramped 100.0 RPM equivalent (13.33 kHz)\n");
            printf("[8] Ramped 200.0 RPM equivalent (26.67 kHz)\n");
            printf("[9] Controlled 0.1 RPM incremental changes (5s sweep)\n");
            printf("[0] Controlled ramp-like rate changes (sweep 0.5 to 72 RPM)\n");
            printf("[s] Stop normal (ramps applied speed to zero)\n");
            printf("[u] Unlock/Disable driver (releases holding torque)\n");
            printf("[r] Diagnostic RECOVERY (stop, unlock, wait 500ms)\n");
            printf("[d] Toggle STEP pin slowly (1 Hz) for multimeter/LED testing\n");
            printf("[t] Re-run automated tests\n");
            printf("=== GPIO 5 active-low ENABLE: LOW=Enabled/Holding, HIGH=Unlocked ===\n");
            printf("Select option: ");
            fflush(stdout);
            print_menu = false;
        }

        int c = getchar();
        if (c == EOF || c == '\n' || c == '\r') {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        printf("%c\n", c);
        print_menu = true; // Reset flag to display menu after command executes

        switch (c) {
            case '1': {
                printf("Testing Ramped 0.5 RPM equivalent (500 milli-RPM)\n");
                argus_trajectory_set_target_rpm_milli(500, true);
                break;
            }
            case '2': {
                printf("Testing Ramped 0.6 RPM equivalent (600 milli-RPM)\n");
                argus_trajectory_set_target_rpm_milli(600, true);
                break;
            }
            case '3': {
                printf("Testing Ramped 0.7 RPM equivalent (700 milli-RPM)\n");
                argus_trajectory_set_target_rpm_milli(700, true);
                break;
            }
            case '4': {
                printf("Testing Ramped 1.0 RPM equivalent (1000 milli-RPM)\n");
                argus_trajectory_set_target_rpm_milli(1000, true);
                break;
            }
            case '5': {
                printf("Testing Ramped 20.0 RPM equivalent (20000 milli-RPM)\n");
                argus_trajectory_set_target_rpm_milli(20000, true);
                break;
            }
            case '6': {
                printf("Testing Ramped 72.0 RPM equivalent (72000 milli-RPM)\n");
                argus_trajectory_set_target_rpm_milli(72000, true);
                break;
            }
            case '7': {
                printf("Testing Ramped 100.0 RPM equivalent (100000 milli-RPM)\n");
                argus_trajectory_set_target_rpm_milli(100000, true);
                break;
            }
            case '8': {
                printf("Testing Ramped 200.0 RPM equivalent (200000 milli-RPM)\n");
                argus_trajectory_set_target_rpm_milli(200000, true);
                break;
            }
            case '9': {
                printf("Starting 0.1 RPM incremental changes (sweep 0.5 to 1.5 RPM)...\n");
                for (int32_t rpm = 500; rpm <= 1500; rpm += 100) {
                    printf("  Setting target speed: %ld.%ld RPM (%ld milli-RPM), step count=%lld\n",
                           (long)(rpm / 1000), (long)((rpm % 1000) / 100), (long)rpm,
                           (long long)argus_step_gen_get_step_count());
                    argus_trajectory_set_target_rpm_milli(rpm, true);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                break;
            }
            case '0': {
                printf("Starting controlled ramp-like changes (sweep 0.5 to 72 RPM and back)...\n");
                for (int32_t rpm = 500; rpm <= 72000; rpm += 1000) {
                    argus_trajectory_set_target_rpm_milli(rpm, true);
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                for (int32_t rpm = 72000; rpm >= 500; rpm -= 1000) {
                    argus_trajectory_set_target_rpm_milli(rpm, true);
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                printf("Ramp sweep completed.\n");
                break;
            }
            case 's': {
                printf("Stopping normal (ramping applied speed to zero, retaining holding torque).\n");
                argus_trajectory_stop_normal();
                break;
            }
            case 'u': {
                printf("Unlocking/Disabling motor driver (releasing holding torque).\n");
                argus_trajectory_unlock();
                break;
            }
            case 'r': {
                printf("Triggering Diagnostic RECOVERY sequence...\n");
                argus_trajectory_recover();
                break;
            }
            case 'd': {
                printf("Toggling STEP pin slowly (1 Hz) for 10 seconds. Check voltage on GPIO 3 (PLS)...\n");
                argus_trajectory_stop_immediate();
                argus_step_gen_enable_driver(); 
                for (int i = 0; i < 10; i++) {
                    printf("  STEP (GPIO 3) -> LOW (Active)\n");
                    gpio_set_level(GPIO_NUM_3, 0);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    printf("  STEP (GPIO 3) -> HIGH (Inactive)\n");
                    gpio_set_level(GPIO_NUM_3, 1);
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                printf("Slow toggle test completed. STEP returned to Inactive (HIGH).\n");
                gpio_set_level(GPIO_NUM_3, 1);
                break;
            }
            case 't': {
                printf("Re-running automated tests...\n");
                argus_tests_run_all();
                break;
            }
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
    ESP_LOGI(TAG, "Argus Pump Controller V2 firmware starting...");

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

    // 5. Arm & Start Step Generator & Trajectory Periodic Task
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

    ESP_LOGI(TAG, "V2 Pump Controller Phase 3A startup completed successfully.");
}
