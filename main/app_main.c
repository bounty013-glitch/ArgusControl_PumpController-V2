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
#include "nvs_flash.h"

#include "argus_mqtt_broker.h"
#include "argus_stepper.h"

// ================= APP CONFIG =================

#define UNIT_NAME       "ArgusMotorTest"
#define WIFI_HOST       "ArgusMotorTestNode"
#define WIFI_SSID       "CherryHome1"
#define WIFI_PASS       "Minstrel13"

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

// ================= STEPPER CONFIG =================

#define ARGUS_STEP_GPIO GPIO_NUM_3
#define ARGUS_DIR_GPIO  GPIO_NUM_4
#define ARGUS_EN_GPIO   GPIO_NUM_5

#define ARGUS_ENABLE_DELAY_MS              20U
#define ARGUS_MAX_STEP_FREQ_HZ             250000U
#define ARGUS_MOTOR_FULL_STEPS_PER_REV     200U
#define ARGUS_MICROSTEPS                   32U
#define ARGUS_GEARBOX_RATIO_NUM            10U
#define ARGUS_GEARBOX_RATIO_DEN            1U
#define ARGUS_MAX_MOTOR_RPM                2000U
#define ARGUS_RAMP_INTERVAL_MS             20U
#define ARGUS_RAMP_RPM_PER_SEC_MILLI       10000U

#define MAX_OUTPUT_RPM_MILLI               72000
#define DEFAULT_SPEED_PCT                  25

// ================= GLOBALS =================

static const char *TAG = "argus_motor_mqtt";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_CONNECT_WAIT_MS 5000

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
    argus_stepper_status_t status = {0};
    if (argus_stepper_get_status(&status) != ESP_OK) {
        return;
    }

    char payload[32];

    snprintf(payload, sizeof(payload), "%s", status.run_commanded ? "true" : "false");
    publish_status_topic(STATUS_RUN_TOPIC, payload, true);

    snprintf(payload, sizeof(payload), "%ld.%03ld",
             (long)(status.applied_rpm_milli / 1000),
             (long)((status.applied_rpm_milli < 0 ? -status.applied_rpm_milli : status.applied_rpm_milli) % 1000));
    publish_status_topic(STATUS_RPM_TOPIC, payload, true);

    snprintf(payload, sizeof(payload), "%s", status.driver_locked ? "true" : "false");
    publish_status_topic(STATUS_LOCKED_TOPIC, payload, true);

    snprintf(payload, sizeof(payload), "%s", status.emergency_stopped ? "true" : "false");
    publish_status_topic(STATUS_ESTOP_TOPIC, payload, true);
}

static void handle_speed_pct(int pct)
{
    if (pct < 0) {
        pct = 0;
    }

    if (pct > 100) {
        pct = 100;
    }

    int32_t rpm_milli = (pct * MAX_OUTPUT_RPM_MILLI) / 100;

    esp_err_t err = argus_stepper_set_speed_rpm_milli(rpm_milli);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Speed set: %d%% = %ld.%03ld RPM output",
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
            esp_err_t err = argus_stepper_start();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Motor RUN");
            } else {
                ESP_LOGE(TAG, "Failed to start motor: %s", esp_err_to_name(err));
            }
        } else {
            esp_err_t err = argus_stepper_stop();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Motor soft stop via run=false");
            } else {
                ESP_LOGE(TAG, "Failed to stop motor: %s", esp_err_to_name(err));
            }
        }

        publish_status();
        return;
    }

    if (strcmp(topic, CMD_STOP_TOPIC) == 0) {
        if (payload_is_true(data)) {
            esp_err_t err = argus_stepper_stop();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Motor soft stop");
            } else {
                ESP_LOGE(TAG, "Failed to stop motor: %s", esp_err_to_name(err));
            }
            publish_status();
        }
        return;
    }

    if (strcmp(topic, CMD_ESTOP_TOPIC) == 0) {
        if (payload_is_true(data)) {
            esp_err_t err = argus_stepper_emergency_stop();
            if (err == ESP_OK) {
                ESP_LOGW(TAG, "Motor E-STOP");
            } else {
                ESP_LOGE(TAG, "Failed to emergency stop motor: %s", esp_err_to_name(err));
            }
            publish_status();
        }
        return;
    }

    if (strcmp(topic, CMD_UNLOCK_TOPIC) == 0) {
        if (payload_is_true(data)) {
            esp_err_t err = argus_stepper_unlock();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Motor unlocked");
            } else {
                ESP_LOGE(TAG, "Failed to unlock motor: %s", esp_err_to_name(err));
            }
            publish_status();
        }
        return;
    }

    ESP_LOGW(TAG, "Unknown MQTT command topic: %s", topic);
}

// ================= WIFI =================

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI("WIFI", "Station started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW("WIFI", "Disconnected. Reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI("WIFI", "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_connect_blocking(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, WIFI_HOST));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_CONNECT_WAIT_MS)
        );

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI("WIFI", "Connected to %s", WIFI_SSID);
            break;
        }

        ESP_LOGW("WIFI", "Waiting for WiFi...");
    }
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

    ESP_LOGI(TAG, "Local MQTT broker ready on port %u", MQTT_BROKER_PORT);
}

// ================= STATUS TASK =================

static void status_task(void *arg)
{
    (void)arg;

    while (true) {
        argus_stepper_status_t status = {0};

        if (argus_stepper_get_status(&status) == ESP_OK) {
            ESP_LOGI(TAG,
                     "cmd=%ld.%03ld rpm applied=%ld.%03ld rpm enabled=%d locked=%d run=%d active=%d e_stop=%d dir_fwd=%d pos_steps=%lu",
                     (long)(status.commanded_rpm_milli / 1000),
                     (long)((status.commanded_rpm_milli < 0 ? -status.commanded_rpm_milli : status.commanded_rpm_milli) % 1000),
                     (long)(status.applied_rpm_milli / 1000),
                     (long)((status.applied_rpm_milli < 0 ? -status.applied_rpm_milli : status.applied_rpm_milli) % 1000),
                     status.enabled,
                     status.driver_locked,
                     status.run_commanded,
                     status.motion_active,
                     status.emergency_stopped,
                     status.direction_forward,
                     (unsigned long)status.position_steps);

            publish_status();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ================= APP MAIN =================

void app_main(void)
{
    ESP_LOGI(TAG, "Firmware build: %s %s", __DATE__, __TIME__);

    const argus_stepper_hw_config_t stepper_config = {
        .step_gpio = ARGUS_STEP_GPIO,
        .dir_gpio = ARGUS_DIR_GPIO,
        .en_gpio = ARGUS_EN_GPIO,
        .enable_pin_active_high = true,
        .has_enable_pin = true,
        .ledc_freq_hz = 1000U,
        .max_step_freq_hz = ARGUS_MAX_STEP_FREQ_HZ,
        .enable_delay_ms = ARGUS_ENABLE_DELAY_MS,
        .motor_full_steps_per_rev = ARGUS_MOTOR_FULL_STEPS_PER_REV,
        .microsteps = ARGUS_MICROSTEPS,
        .gearbox_ratio_num = ARGUS_GEARBOX_RATIO_NUM,
        .gearbox_ratio_den = ARGUS_GEARBOX_RATIO_DEN,
        .max_motor_rpm = ARGUS_MAX_MOTOR_RPM,
        .ramp_interval_ms = ARGUS_RAMP_INTERVAL_MS,
        .ramp_rpm_per_sec_milli = ARGUS_RAMP_RPM_PER_SEC_MILLI,
    };

    ESP_ERROR_CHECK(argus_stepper_init(&stepper_config));

    handle_speed_pct(DEFAULT_SPEED_PCT);

    ESP_LOGI(TAG, "Motor initialized. It will NOT auto-start.");

    wifi_connect_blocking();
    mqtt_broker_start();

    xTaskCreate(status_task, "status_task", 4096, NULL, 5, NULL);
}
