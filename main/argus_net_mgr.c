/**
 * @file argus_net_mgr.c
 * @brief Dedicated Network-Mode & Wi-Fi Lifecycle Manager Implementation
 */

#include "argus_net_mgr.h"
#include "argus_identity.h"
#include "argus_nvs_config.h"
#include "argus_authority_mgr.h"
#include "argus_state_mgr.h"
#include "argus_cmd_router.h"
#include "argus_mqtt_broker.h"
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

static volatile bool s_sta_started = false;
static volatile bool s_sta_connected = false;
static volatile bool s_sta_ip_acquired = false;
static volatile bool s_ap_started = false;
static argus_net_mgr_mqtt_broker_start_fn_t s_broker_start_cb = NULL;

void argus_net_mgr_register_broker_start_cb(argus_net_mgr_mqtt_broker_start_fn_t cb)
{
    s_broker_start_cb = cb;
}

bool argus_net_mgr_is_sta_started(void) { return s_sta_started; }
bool argus_net_mgr_is_sta_connected(void) { return s_sta_connected; }
bool argus_net_mgr_is_sta_ip_acquired(void) { return s_sta_ip_acquired; }
bool argus_net_mgr_is_ap_started(void) { return s_ap_started; }

const char *argus_net_mgr_get_wifi_driver_mode_name(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) return "UNKNOWN";
    switch (mode) {
        case WIFI_MODE_NULL: return "NULL";
        case WIFI_MODE_STA: return "STA";
        case WIFI_MODE_AP: return "AP";
        case WIFI_MODE_APSTA: return "APSTA";
        default: return "UNKNOWN";
    }
}

static void set_net_mode(argus_network_mode_t new_mode);

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            s_sta_started = true;
        } else if (event_id == WIFI_EVENT_STA_STOP) {
            s_sta_started = false;
            s_sta_connected = false;
            s_sta_ip_acquired = false;
        } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            s_sta_connected = true;
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            s_sta_connected = false;
            s_sta_ip_acquired = false;
            argus_net_event_t evt = { .type = ARGUS_NET_EVT_STA_DISCONNECTED };
            argus_net_mgr_post_event(&evt);
        } else if (event_id == WIFI_EVENT_AP_START) {
            s_ap_started = true;
        } else if (event_id == WIFI_EVENT_AP_STOP) {
            s_ap_started = false;
        } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            argus_net_event_t evt = { .type = ARGUS_NET_EVT_AP_CLIENT_CONNECTED };
            argus_net_mgr_post_event(&evt);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            argus_net_event_t evt = { .type = ARGUS_NET_EVT_AP_CLIENT_DISCONNECTED };
            argus_net_mgr_post_event(&evt);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_sta_ip_acquired = true;
        argus_net_event_t evt = { .type = ARGUS_NET_EVT_STA_CONNECTED };
        argus_net_mgr_post_event(&evt);
    }
}

static void net_mgr_task(void *pvParameters)
{
    argus_net_event_t evt;
    while (1) {
        if (xQueueReceive(s_event_queue, &evt, portMAX_DELAY) == pdTRUE) {
            switch (evt.type) {
                case ARGUS_NET_EVT_SERVICE_REQUEST:
                    argus_net_mgr_request_service(evt.requested_owner);
                    break;

                case ARGUS_NET_EVT_SERVICE_EXIT:
                    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
                    set_net_mode(ARGUS_NET_MODE_SERVICE_TRANSITION);
                    argus_authority_request_exit();
                    xSemaphoreGive(s_net_mutex);
                    break;

                case ARGUS_NET_EVT_STA_CONNECTED:
                    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
                    if (s_net_mode == ARGUS_NET_MODE_COMMISSIONED_STA || s_net_mode == ARGUS_NET_MODE_NETWORK_FAULT) {
                        set_net_mode(ARGUS_NET_MODE_COMMISSIONED_STA);
                        if (s_broker_start_cb != NULL) {
                            s_broker_start_cb();
                        }
                        if (argus_mqtt_broker_is_running()) {
                            argus_authority_mgr_set_mode(ARGUS_AUTHORITY_SUPERVISORY, ARGUS_AUTH_OWNER_MQTT);
                            ESP_LOGI(TAG, "STA IP acquired & MQTT broker running. Granted SUPERVISORY/MQTT authority.");
                        } else {
                            ESP_LOGE(TAG, "STA IP acquired, but MQTT broker failed to start. Supervisory authority withheld.");
                        }
                    }
                    xSemaphoreGive(s_net_mutex);
                    break;

                case ARGUS_NET_EVT_STA_DISCONNECTED:
                    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
                    if (s_net_mode == ARGUS_NET_MODE_COMMISSIONED_STA || s_net_mode == ARGUS_NET_MODE_AP_DISCOVERABLE) {
                        ESP_LOGW(TAG, "STA disconnected/IP lost. Revoking SUPERVISORY MQTT authority & stopping broker listener.");
                        argus_authority_mgr_set_mode(ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);
                        argus_mqtt_broker_stop();
                    }
                    xSemaphoreGive(s_net_mutex);
                    break;

                case ARGUS_NET_EVT_AP_CLIENT_CONNECTED:
                    break;

                case ARGUS_NET_EVT_AP_CLIENT_DISCONNECTED:
                    break;

                default:
                    break;
            }
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
    if (s_net_mode != ARGUS_NET_MODE_COMMISSIONED_STA) {
        ESP_LOGE(TAG, "Cannot enable AP discoverability: network mode must be COMMISSIONED_STA (current: %s)", argus_net_mgr_get_mode_name(s_net_mode));
        xSemaphoreGive(s_net_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    argus_identity_t id;
    argus_identity_get(&id);

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, id.service_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, CONFIG_ARGUS_SERVICE_AP_PASS, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(id.service_ssid);
    ap_config.ap.max_connection = 1;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }

    if (err == ESP_OK) {
        s_ap_started = true;
        set_net_mode(ARGUS_NET_MODE_AP_DISCOVERABLE);
        ESP_LOGI(TAG, "Service AP discoverability enabled cleanly in APSTA mode.");
    } else {
        ESP_LOGE(TAG, "Failed to enable Service AP: %s. Preserving COMMISSIONED_STA.", esp_err_to_name(err));
    }
    xSemaphoreGive(s_net_mutex);

    return err;
}

argus_network_mode_t argus_net_mgr_get_mode(void) { return s_net_mode; }
argus_net_err_t argus_net_mgr_get_last_error(void) { return s_last_error; }

esp_err_t argus_net_mgr_orchestrate_service_entry(argus_network_mode_t *net_mode,
                                                   argus_authority_owner_t requested_owner,
                                                   const argus_service_authority_ops_t *auth_ops,
                                                   const argus_service_transition_ops_t *ops)
{
    if (!net_mode || !auth_ops || !ops) return ESP_ERR_INVALID_ARG;

    // Validate authority ops table
    if (!auth_ops->prepare_transition || !auth_ops->grant_local || !auth_ops->abort_transition) {
        return ESP_ERR_INVALID_ARG;
    }

    // Validate transition ops table
    if (!ops->request_normal_stop || !ops->verify_stopped || !ops->stop_broker ||
        !ops->verify_broker_stopped || !ops->disconnect_sta || !ops->verify_sta_disconnected ||
        !ops->verify_sta_ip_released || !ops->set_wifi_ap_only || !ops->verify_ap_active) {
        return ESP_ERR_INVALID_ARG;
    }

    if (requested_owner != ARGUS_AUTH_OWNER_BROWSER && requested_owner != ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI) {
        return ESP_ERR_INVALID_ARG;
    }

    // Step 2: Prepare transition (SERVICE_TRANSITION/NONE) and increment generation
    esp_err_t prep_err = auth_ops->prepare_transition(auth_ops->ctx);
    if (prep_err != ESP_OK) {
        auth_ops->abort_transition(auth_ops->ctx);
        *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
        return prep_err;
    }

    // Step 4: Request controlled normal stop
    esp_err_t stop_err = ops->request_normal_stop(ops->ctx);
    if (stop_err != ESP_OK) {
        auth_ops->abort_transition(auth_ops->ctx);
        *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
        return stop_err;
    }

    // Step 5: Verify machine reached HOLDING or UNLOCKED
    esp_err_t verify_stop_err = ops->verify_stopped(ops->ctx);
    if (verify_stop_err != ESP_OK) {
        auth_ops->abort_transition(auth_ops->ctx);
        *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
        return verify_stop_err;
    }

    // Step 6: Stop MQTT broker
    esp_err_t broker_err = ops->stop_broker(ops->ctx);
    if (broker_err != ESP_OK) {
        auth_ops->abort_transition(auth_ops->ctx);
        *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
        return broker_err;
    }

    // Step 7: Verify MQTT broker is stopped (bounded 2.0s polling wait)
    esp_err_t verify_broker_err = ops->verify_broker_stopped(ops->ctx);
    if (verify_broker_err != ESP_OK) {
        auth_ops->abort_transition(auth_ops->ctx);
        *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
        return verify_broker_err;
    }

    // Step 8: Disconnect STA
    esp_err_t disc_err = ops->disconnect_sta(ops->ctx);
    if (disc_err != ESP_OK) {
        auth_ops->abort_transition(auth_ops->ctx);
        *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
        return disc_err;
    }

    // Step 9: Verify STA is disconnected (bounded 2.0s polling wait)
    esp_err_t verify_disc_err = ops->verify_sta_disconnected(ops->ctx);
    if (verify_disc_err != ESP_OK) {
        auth_ops->abort_transition(auth_ops->ctx);
        *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
        return verify_disc_err;
    }

    // Step 10: Verify STA IP is released (bounded 2.0s polling wait)
    esp_err_t verify_ip_err = ops->verify_sta_ip_released(ops->ctx);
    if (verify_ip_err != ESP_OK) {
        auth_ops->abort_transition(auth_ops->ctx);
        *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
        return verify_ip_err;
    }

    // Step 11: Set Wi-Fi driver to AP-only
    esp_err_t set_ap_err = ops->set_wifi_ap_only(ops->ctx);
    if (set_ap_err != ESP_OK) {
        auth_ops->abort_transition(auth_ops->ctx);
        *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
        return set_ap_err;
    }

    // Step 12: Verify Service AP is active (bounded 2.0s polling wait)
    esp_err_t verify_ap_err = ops->verify_ap_active(ops->ctx);
    if (verify_ap_err != ESP_OK) {
        auth_ops->abort_transition(auth_ops->ctx);
        *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
        return verify_ap_err;
    }

    // Step 13: Set network mode SERVICE_AP_ONLY
    *net_mode = ARGUS_NET_MODE_SERVICE_AP_ONLY;

    // Step 14: Grant LOCAL_SERVICE/<owner> last
    esp_err_t grant_err = auth_ops->grant_local(auth_ops->ctx, requested_owner);
    if (grant_err != ESP_OK) {
        auth_ops->abort_transition(auth_ops->ctx);
        *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
        return grant_err;
    }

    return ESP_OK;
}

static esp_err_t prod_request_normal_stop(void *ctx) {
    (void)ctx;
    return argus_state_mgr_stop_normal();
}

static esp_err_t prod_verify_stopped(void *ctx) {
    (void)ctx;
    argus_state_snapshot_t state_snap;
    int timeout_ms = 5000;
    while (timeout_ms > 0) {
        argus_state_mgr_get_snapshot(&state_snap);
        if (state_snap.machine_state == ARGUS_STATE_HOLDING || state_snap.machine_state == ARGUS_STATE_UNLOCKED) {
            return ESP_OK;
        }
        if (state_snap.machine_state == ARGUS_STATE_EMERGENCY_STOPPED || state_snap.machine_state == ARGUS_STATE_FAULTED || state_snap.estop_latched) {
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout_ms -= 50;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t prod_stop_broker(void *ctx) {
    (void)ctx;
    return argus_mqtt_broker_stop();
}

static esp_err_t prod_verify_broker_stopped(void *ctx) {
    (void)ctx;
    int timeout_ms = 2000;
    while (timeout_ms > 0) {
        if (!argus_mqtt_broker_is_running()) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout_ms -= 50;
    }
    return argus_mqtt_broker_is_running() ? ESP_ERR_TIMEOUT : ESP_OK;
}

static esp_err_t prod_disconnect_sta(void *ctx) {
    (void)ctx;
    return esp_wifi_disconnect();
}

static esp_err_t prod_verify_sta_disconnected(void *ctx) {
    (void)ctx;
    int timeout_ms = 2000;
    while (timeout_ms > 0) {
        if (!s_sta_started && !s_sta_connected) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout_ms -= 50;
    }
    return (s_sta_connected || s_sta_started) ? ESP_ERR_TIMEOUT : ESP_OK;
}

static esp_err_t prod_verify_sta_ip_released(void *ctx) {
    (void)ctx;
    int timeout_ms = 2000;
    while (timeout_ms > 0) {
        if (!s_sta_ip_acquired) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout_ms -= 50;
    }
    return s_sta_ip_acquired ? ESP_ERR_TIMEOUT : ESP_OK;
}

static esp_err_t prod_set_wifi_ap_only(void *ctx) {
    (void)ctx;
    return esp_wifi_set_mode(WIFI_MODE_AP);
}

static esp_err_t prod_verify_ap_active(void *ctx) {
    (void)ctx;
    int timeout_ms = 2000;
    wifi_mode_t mode = WIFI_MODE_NULL;
    while (timeout_ms > 0) {
        if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP && s_ap_started) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout_ms -= 50;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t argus_net_mgr_request_service(argus_authority_owner_t requested_owner)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    if (requested_owner != ARGUS_AUTH_OWNER_BROWSER && requested_owner != ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI) {
        ESP_LOGE(TAG, "Service entry rejected: invalid requested owner (%d)", (int)requested_owner);
        return ESP_ERR_INVALID_ARG;
    }

    // Step 1: Acquire network-transition serialization
    xSemaphoreTake(s_net_mutex, portMAX_DELAY);

    if (s_net_mode != ARGUS_NET_MODE_AP_DISCOVERABLE && s_net_mode != ARGUS_NET_MODE_UNCOMMISSIONED_AP) {
        ESP_LOGE(TAG, "Service entry rejected: starting mode must be AP_DISCOVERABLE or UNCOMMISSIONED_AP (current: %s)",
                 argus_net_mgr_get_mode_name(s_net_mode));
        xSemaphoreGive(s_net_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_ap_started) {
        ESP_LOGE(TAG, "Service entry rejected: Service AP is not started");
        xSemaphoreGive(s_net_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // Step 2: Acquire command-dispatch exclusivity
    argus_cmd_router_lock_dispatch();

    argus_service_authority_ops_t auth_ops;
    argus_authority_get_production_service_ops(&auth_ops);

    argus_service_transition_ops_t prod_ops = {
        .request_normal_stop = prod_request_normal_stop,
        .verify_stopped = prod_verify_stopped,
        .stop_broker = prod_stop_broker,
        .verify_broker_stopped = prod_verify_broker_stopped,
        .disconnect_sta = prod_disconnect_sta,
        .verify_sta_disconnected = prod_verify_sta_disconnected,
        .verify_sta_ip_released = prod_verify_sta_ip_released,
        .set_wifi_ap_only = prod_set_wifi_ap_only,
        .verify_ap_active = prod_verify_ap_active,
        .ctx = NULL
    };

    esp_err_t res = argus_net_mgr_orchestrate_service_entry(&s_net_mode, requested_owner, &auth_ops, &prod_ops);
    if (res != ESP_OK) {
        s_last_error = ARGUS_NET_ERR_STA_SHUTDOWN_FAILED;
    } else {
        ESP_LOGI(TAG, "Coordinated service entry complete. Mode: SERVICE_AP_ONLY, Authority: LOCAL_SERVICE/%s",
                 argus_authority_mgr_get_owner_name(requested_owner));
    }

    // Step 8: Release command-dispatch exclusivity
    argus_cmd_router_unlock_dispatch();

    // Step 9: Release network-transition serialization
    xSemaphoreGive(s_net_mutex);
    return res;
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
