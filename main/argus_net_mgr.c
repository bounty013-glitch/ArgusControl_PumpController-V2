/**
 * @file argus_net_mgr.c
 * @brief Dedicated Network-Mode & Wi-Fi Lifecycle Manager Implementation
 */

#include "argus_net_mgr.h"
#include "argus_identity.h"
#include "argus_nvs_config.h"
#include "argus_authority_mgr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "argus_net_mgr";

static argus_network_mode_t s_net_mode = ARGUS_NET_MODE_BOOTSTRAP;
static argus_net_err_t s_last_error = ARGUS_NET_ERR_NONE;

static QueueHandle_t s_event_queue = NULL;
static SemaphoreHandle_t s_net_mutex = NULL;
static StaticSemaphore_t s_net_mutex_buffer;
static bool s_initialized = false;

static esp_netif_t *s_netif_ap = NULL;
static esp_netif_t *s_netif_sta = NULL;

#ifndef CONFIG_ARGUS_SERVICE_AP_PASS
#error "CONFIG_ARGUS_SERVICE_AP_PASS is not defined. Provision via sdkconfig."
#endif

_Static_assert(sizeof(CONFIG_ARGUS_SERVICE_AP_PASS) - 1 >= 8,
               "CONFIG_ARGUS_SERVICE_AP_PASS must be at least 8 characters");

_Static_assert(sizeof(CONFIG_ARGUS_SERVICE_AP_PASS) - 1 <= 63,
               "CONFIG_ARGUS_SERVICE_AP_PASS must not exceed 63 characters");

static bool validate_build_service_credential(void)
{
    const char *pass = CONFIG_ARGUS_SERVICE_AP_PASS;
    if (!pass) return false;
    size_t len = strlen(pass);
    return (len >= 8 && len <= 63);
}

static void set_net_mode(argus_network_mode_t new_mode);

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            argus_net_event_t evt = { .type = ARGUS_NET_EVT_STA_DISCONNECTED };
            argus_net_mgr_post_event(&evt);
        } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            argus_net_event_t evt = { .type = ARGUS_NET_EVT_AP_CLIENT_CONNECTED };
            argus_net_mgr_post_event(&evt);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            argus_net_event_t evt = { .type = ARGUS_NET_EVT_AP_CLIENT_DISCONNECTED };
            argus_net_mgr_post_event(&evt);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        argus_net_event_t evt = { .type = ARGUS_NET_EVT_STA_CONNECTED };
        argus_net_mgr_post_event(&evt);
    }
}

static void net_mgr_task(void *pvParameters)
{
    argus_net_event_t evt;
    while (1) {
        if (xQueueReceive(s_event_queue, &evt, portMAX_DELAY) == pdTRUE) {
            xSemaphoreTake(s_net_mutex, portMAX_DELAY);
            switch (evt.type) {
                case ARGUS_NET_EVT_SERVICE_REQUEST:
                    set_net_mode(ARGUS_NET_MODE_SERVICE_TRANSITION);
                    argus_authority_request_service(evt.requested_owner);
                    set_net_mode(ARGUS_NET_MODE_SERVICE_AP_ONLY);
                    break;

                case ARGUS_NET_EVT_SERVICE_EXIT:
                    set_net_mode(ARGUS_NET_MODE_SERVICE_TRANSITION);
                    argus_authority_request_exit();
                    break;

                case ARGUS_NET_EVT_STA_CONNECTED:
                    if (s_net_mode == ARGUS_NET_MODE_COMMISSIONED_STA || s_net_mode == ARGUS_NET_MODE_NETWORK_FAULT) {
                        set_net_mode(ARGUS_NET_MODE_AP_DISCOVERABLE);
                        argus_authority_mgr_set_mode(ARGUS_AUTHORITY_SUPERVISORY, ARGUS_AUTH_OWNER_MQTT);
                    }
                    break;

                case ARGUS_NET_EVT_STA_DISCONNECTED:
                    // Incidental STA loss retains current trajectory (fail-operational)
                    break;

                case ARGUS_NET_EVT_AP_CLIENT_CONNECTED:
                    // AP association alone does NOT grant motion authority
                    break;

                case ARGUS_NET_EVT_AP_CLIENT_DISCONNECTED:
                    // Client disconnect in service mode keeps LOCAL_SERVICE
                    break;

                default:
                    break;
            }
            xSemaphoreGive(s_net_mutex);
        }
    }
}

esp_err_t argus_net_mgr_init(void)
{
    if (s_initialized) return ESP_OK;

    s_net_mutex = xSemaphoreCreateMutexStatic(&s_net_mutex_buffer);
    s_event_queue = xQueueCreate(10, sizeof(argus_net_event_t));

    // Validate build service credential
    if (!validate_build_service_credential()) {
        s_last_error = ARGUS_NET_ERR_SERVICE_CRED_MISSING;
        ESP_LOGE(TAG, "Build service AP credential missing or invalid (<8 chars). Failing closed.");
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_init();
    esp_event_loop_create_default();

    s_netif_ap = esp_netif_create_default_wifi_ap();
    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // Disable Wi-Fi power-save to eliminate STEP pulse jitter
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    argus_identity_t id;
    argus_identity_get(&id);

    argus_config_payload_t cfg_payload;
    bool commissioned = (argus_nvs_config_get(&cfg_payload) == ESP_OK) &&
                         argus_nvs_config_is_commissioned(&cfg_payload);

    if (!commissioned) {
        // Uncommissioned AP-only mode
        s_net_mode = ARGUS_NET_MODE_UNCOMMISSIONED_AP;
        argus_authority_mgr_set_mode(ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);

        wifi_config_t ap_config = {0};
        strlcpy((char *)ap_config.ap.ssid, id.service_ssid, sizeof(ap_config.ap.ssid));
        strlcpy((char *)ap_config.ap.password, CONFIG_ARGUS_SERVICE_AP_PASS, sizeof(ap_config.ap.password));
        ap_config.ap.ssid_len = strlen(id.service_ssid);
        ap_config.ap.max_connection = 1;
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

        esp_wifi_set_mode(WIFI_MODE_AP);
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        esp_wifi_start();
    } else {
        // Commissioned STA mode
        s_net_mode = ARGUS_NET_MODE_COMMISSIONED_STA;

        wifi_config_t sta_config = {0};
        strlcpy((char *)sta_config.sta.ssid, cfg_payload.sta_ssid, sizeof(sta_config.sta.ssid));
        strlcpy((char *)sta_config.sta.password, cfg_payload.sta_pass, sizeof(sta_config.sta.password));

        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        esp_wifi_start();
        esp_wifi_connect();
    }

    xTaskCreate(net_mgr_task, "argus_net_mgr", 4096, NULL, 4, NULL);
    s_initialized = true;
    return ESP_OK;
}

const char *argus_net_mgr_get_mode_name(argus_network_mode_t mode)
{
    switch (mode) {
        case ARGUS_NET_MODE_BOOTSTRAP: return "BOOTSTRAP";
        case ARGUS_NET_MODE_UNCOMMISSIONED_AP: return "UNCOMMISSIONED_AP";
        case ARGUS_NET_MODE_COMMISSIONED_STA: return "COMMISSIONED_STA";
        case ARGUS_NET_MODE_AP_DISCOVERABLE: return "AP_DISCOVERABLE";
        case ARGUS_NET_MODE_SERVICE_TRANSITION: return "SERVICE_TRANSITION";
        case ARGUS_NET_MODE_SERVICE_AP_ONLY: return "SERVICE_AP_ONLY";
        case ARGUS_NET_MODE_NETWORK_FAULT: return "NETWORK_FAULT";
        default: return "UNKNOWN";
    }
}

static void set_net_mode(argus_network_mode_t new_mode)
{
    if (s_net_mode != new_mode) {
        ESP_LOGI(TAG, "network: %s -> %s", argus_net_mgr_get_mode_name(s_net_mode), argus_net_mgr_get_mode_name(new_mode));
        s_net_mode = new_mode;
    }
}

esp_err_t argus_net_mgr_enable_ap_discoverable(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
    argus_identity_t id;
    argus_identity_get(&id);

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, id.service_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, CONFIG_ARGUS_SERVICE_AP_PASS, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(id.service_ssid);
    ap_config.ap.max_connection = 1;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    set_net_mode(ARGUS_NET_MODE_AP_DISCOVERABLE);
    xSemaphoreGive(s_net_mutex);

    return ESP_OK;
}

argus_network_mode_t argus_net_mgr_get_mode(void)
{
    return s_net_mode;
}

esp_err_t argus_net_mgr_post_event(const argus_net_event_t *evt)
{
    if (!evt || !s_event_queue) return ESP_ERR_INVALID_ARG;
    if (xQueueSend(s_event_queue, evt, 0) != pdTRUE) {
        s_last_error = ARGUS_NET_ERR_QUEUE_OVERFLOW;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

argus_net_err_t argus_net_mgr_get_last_error(void)
{
    return s_last_error;
}
