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
#include "argus_http_server.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
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

static _Atomic bool s_sta_started = false;
static _Atomic bool s_sta_connected = false;
static _Atomic bool s_sta_ip_acquired = false;
static _Atomic bool s_ap_started = false;
static argus_net_mgr_mqtt_broker_start_fn_t s_broker_start_cb = NULL;

// Unused: event group removed; verification uses bounded atomic polling.
// Retained as comment for Phase 4B if event-bit waits are needed.

void argus_net_mgr_register_broker_start_cb(argus_net_mgr_mqtt_broker_start_fn_t cb)
{
    s_broker_start_cb = cb;
}

bool argus_net_mgr_is_sta_started(void) { return atomic_load(&s_sta_started); }
bool argus_net_mgr_is_sta_connected(void) { return atomic_load(&s_sta_connected); }
bool argus_net_mgr_is_sta_ip_acquired(void) { return atomic_load(&s_sta_ip_acquired); }
bool argus_net_mgr_is_ap_started(void) { return atomic_load(&s_ap_started); }

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
            atomic_store(&s_sta_started, true);
        } else if (event_id == WIFI_EVENT_STA_STOP) {
            atomic_store(&s_sta_started, false);
            atomic_store(&s_sta_connected, false);
            atomic_store(&s_sta_ip_acquired, false);
        } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            atomic_store(&s_sta_connected, true);
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            atomic_store(&s_sta_connected, false);
            atomic_store(&s_sta_ip_acquired, false);
            argus_net_event_t evt = { .type = ARGUS_NET_EVT_STA_DISCONNECTED };
            argus_net_mgr_post_event(&evt);
        } else if (event_id == WIFI_EVENT_AP_START) {
            atomic_store(&s_ap_started, true);
        } else if (event_id == WIFI_EVENT_AP_STOP) {
            atomic_store(&s_ap_started, false);
        } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            argus_net_event_t evt = { .type = ARGUS_NET_EVT_AP_CLIENT_CONNECTED };
            argus_net_mgr_post_event(&evt);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            argus_net_event_t evt = { .type = ARGUS_NET_EVT_AP_CLIENT_DISCONNECTED };
            argus_net_mgr_post_event(&evt);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        atomic_store(&s_sta_ip_acquired, true);
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
                    argus_net_mgr_request_service_exit(evt.requested_owner);
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

        /* Start HTTP server for commissioning portal (non-fatal) */
        esp_err_t http_err = argus_http_server_start();
        if (http_err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP server start failed in UNCOMMISSIONED_AP: %s", esp_err_to_name(http_err));
        }
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
        // Do NOT manually set s_ap_started here; the Wi-Fi event handler
        // (WIFI_EVENT_AP_START) is the authoritative source.
        set_net_mode(ARGUS_NET_MODE_AP_DISCOVERABLE);
        ESP_LOGI(TAG, "Service AP discoverability enabled cleanly in APSTA mode.");
    } else {
        ESP_LOGE(TAG, "Failed to enable Service AP: %s. Preserving COMMISSIONED_STA.", esp_err_to_name(err));
    }
    xSemaphoreGive(s_net_mutex);

    /* Start HTTP server outside s_net_mutex. The status handler takes
     * s_net_mutex via argus_net_mgr_get_snapshot() for a coherent network
     * observation. Holding s_net_mutex across server lifecycle operations
     * would deadlock if httpd_stop() needed to wait for such a handler. */
    if (err == ESP_OK) {
        esp_err_t http_err = argus_http_server_start();
        if (http_err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP server start failed in AP_DISCOVERABLE: %s", esp_err_to_name(http_err));
        }
    }

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
        !ops->verify_sta_ip_released || !ops->set_wifi_ap_only || !ops->verify_ap_active ||
        !ops->verify_machine_safe) {
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

    // Step 14: Final pre-grant machine-state and E-stop verification
    esp_err_t safe_err = ops->verify_machine_safe(ops->ctx);
    if (safe_err != ESP_OK) {
        auth_ops->abort_transition(auth_ops->ctx);
        *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
        return safe_err;
    }

    // Step 15: Grant LOCAL_SERVICE/<owner> last
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
    // After esp_wifi_disconnect() but before set_mode(AP), STA interface may
    // still be started. Only verify connection and IP are released here.
    int timeout_ms = 2000;
    while (timeout_ms > 0) {
        if (!atomic_load(&s_sta_connected)) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout_ms -= 50;
    }
    return atomic_load(&s_sta_connected) ? ESP_ERR_TIMEOUT : ESP_OK;
}

static esp_err_t prod_verify_sta_ip_released(void *ctx) {
    (void)ctx;
    int timeout_ms = 2000;
    while (timeout_ms > 0) {
        if (!atomic_load(&s_sta_ip_acquired)) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout_ms -= 50;
    }
    return atomic_load(&s_sta_ip_acquired) ? ESP_ERR_TIMEOUT : ESP_OK;
}

static esp_err_t prod_set_wifi_ap_only(void *ctx) {
    (void)ctx;
    return esp_wifi_set_mode(WIFI_MODE_AP);
}

static esp_err_t prod_verify_ap_active(void *ctx) {
    (void)ctx;
    // After set_wifi_ap_only(), verify full AP-only state:
    // driver mode == AP, AP event fired, STA fully torn down.
    int timeout_ms = 2000;
    wifi_mode_t mode = WIFI_MODE_NULL;
    while (timeout_ms > 0) {
        bool mode_ok = (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP);
        bool ap_ok = atomic_load(&s_ap_started);
        bool sta_gone = !atomic_load(&s_sta_started) && !atomic_load(&s_sta_connected) && !atomic_load(&s_sta_ip_acquired);
        if (mode_ok && ap_ok && sta_gone) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout_ms -= 50;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t prod_verify_machine_safe(void *ctx) {
    (void)ctx;
    argus_state_snapshot_t state_snap;
    argus_state_mgr_get_snapshot(&state_snap);
    if (state_snap.estop_latched || state_snap.machine_state == ARGUS_STATE_EMERGENCY_STOPPED ||
        state_snap.machine_state == ARGUS_STATE_FAULTED) {
        ESP_LOGE(TAG, "Pre-grant safety check failed: state=%s, estop_latched=%d",
                 argus_state_mgr_get_state_name(state_snap.machine_state), (int)state_snap.estop_latched);
        return ESP_ERR_INVALID_STATE;
    }
    if (state_snap.machine_state != ARGUS_STATE_HOLDING && state_snap.machine_state != ARGUS_STATE_UNLOCKED) {
        ESP_LOGE(TAG, "Pre-grant safety check failed: unexpected machine state %s",
                 argus_state_mgr_get_state_name(state_snap.machine_state));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
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

    if (!atomic_load(&s_ap_started)) {
        ESP_LOGE(TAG, "Service entry rejected: Service AP is not started");
        xSemaphoreGive(s_net_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // ---- FIRST DISPATCH GATE: validate authority + set SERVICE_TRANSITION ----
    argus_cmd_router_lock_dispatch();

    argus_service_authority_ops_t auth_ops;
    argus_authority_get_production_service_ops(&auth_ops);

    // Validate current authority is consistent with the starting mode
    argus_authority_snapshot_t auth_pre;
    argus_authority_mgr_get_snapshot(&auth_pre);
    if (s_net_mode == ARGUS_NET_MODE_AP_DISCOVERABLE) {
        // Discoverable mode requires SUPERVISORY/MQTT authority
        if (auth_pre.mode != ARGUS_AUTHORITY_SUPERVISORY ||
            auth_pre.owner != ARGUS_AUTH_OWNER_MQTT) {
            ESP_LOGE(TAG, "Service entry: AP_DISCOVERABLE requires SUPERVISORY/MQTT (got mode=%d, owner=%d)",
                     (int)auth_pre.mode, (int)auth_pre.owner);
            argus_cmd_router_unlock_dispatch();
            xSemaphoreGive(s_net_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    } else if (s_net_mode == ARGUS_NET_MODE_UNCOMMISSIONED_AP) {
        // Uncommissioned mode requires NONE/NONE authority
        if (auth_pre.mode != ARGUS_AUTHORITY_NONE ||
            auth_pre.owner != ARGUS_AUTH_OWNER_NONE) {
            ESP_LOGE(TAG, "Service entry: UNCOMMISSIONED_AP requires NONE/NONE (got mode=%d, owner=%d)",
                     (int)auth_pre.mode, (int)auth_pre.owner);
            argus_cmd_router_unlock_dispatch();
            xSemaphoreGive(s_net_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }

    // Set authority to SERVICE_TRANSITION/NONE (increments generation)
    esp_err_t prep_err = auth_ops.prepare_transition(auth_ops.ctx);
    if (prep_err != ESP_OK) {
        auth_ops.abort_transition(auth_ops.ctx);
        s_net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
        argus_cmd_router_unlock_dispatch();
        xSemaphoreGive(s_net_mutex);
        return prep_err;
    }

    // Capture the transition generation for revalidation
    argus_authority_snapshot_t auth_snap_after_prep;
    argus_authority_mgr_get_snapshot(&auth_snap_after_prep);
    uint32_t transition_generation = auth_snap_after_prep.generation;

    set_net_mode(ARGUS_NET_MODE_SERVICE_TRANSITION);

    argus_cmd_router_unlock_dispatch();
    // ---- END FIRST DISPATCH GATE ----

    /* Stop HTTP server outside s_net_mutex.
     * The HTTP status handler takes s_net_mutex via argus_net_mgr_get_snapshot()
     * for a coherent network observation. httpd_stop() waits for active handlers
     * to finish. If we held s_net_mutex here, an active handler waiting for
     * s_net_mutex would deadlock with httpd_stop(). Releasing first ensures
     * handlers can complete. Mode is already SERVICE_TRANSITION, preventing
     * concurrent mode-change callers. */
    xSemaphoreGive(s_net_mutex);
    esp_err_t http_stop_err = argus_http_server_stop();
    if (http_stop_err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP server stop failed during service transition: %s", esp_err_to_name(http_stop_err));
    }
    xSemaphoreTake(s_net_mutex, portMAX_DELAY);

    // Normal commands reaching the router during this interval acquire dispatch,
    // observe SERVICE_TRANSITION, are rejected, and return promptly.

    // Execute blocking operations without holding dispatch:
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
        .verify_machine_safe = prod_verify_machine_safe,
        .ctx = NULL
    };

    // Execute transition operations (all blocking, no dispatch held)
    esp_err_t op_err = ESP_OK;

    op_err = prod_ops.request_normal_stop(prod_ops.ctx);
    if (op_err != ESP_OK) goto abort_transition;

    op_err = prod_ops.verify_stopped(prod_ops.ctx);
    if (op_err != ESP_OK) goto abort_transition;

    op_err = prod_ops.stop_broker(prod_ops.ctx);
    if (op_err != ESP_OK) goto abort_transition;

    op_err = prod_ops.verify_broker_stopped(prod_ops.ctx);
    if (op_err != ESP_OK) goto abort_transition;

    op_err = prod_ops.disconnect_sta(prod_ops.ctx);
    if (op_err != ESP_OK) goto abort_transition;

    op_err = prod_ops.verify_sta_disconnected(prod_ops.ctx);
    if (op_err != ESP_OK) goto abort_transition;

    op_err = prod_ops.verify_sta_ip_released(prod_ops.ctx);
    if (op_err != ESP_OK) goto abort_transition;

    op_err = prod_ops.set_wifi_ap_only(prod_ops.ctx);
    if (op_err != ESP_OK) goto abort_transition;

    op_err = prod_ops.verify_ap_active(prod_ops.ctx);
    if (op_err != ESP_OK) goto abort_transition;

    op_err = prod_ops.verify_machine_safe(prod_ops.ctx);
    if (op_err != ESP_OK) goto abort_transition;

    // ---- SECOND DISPATCH GATE: revalidate + grant ----
    argus_cmd_router_lock_dispatch();
    {
        // Revalidate authority remains SERVICE_TRANSITION/NONE with same generation
        argus_authority_snapshot_t recheck;
        argus_authority_mgr_get_snapshot(&recheck);

        if (recheck.mode != ARGUS_AUTHORITY_SERVICE_TRANSITION ||
            recheck.owner != ARGUS_AUTH_OWNER_NONE ||
            recheck.generation != transition_generation) {
            ESP_LOGE(TAG, "Service entry: authority changed during transition (gen %lu -> %lu, mode %d)",
                     (unsigned long)transition_generation, (unsigned long)recheck.generation, (int)recheck.mode);
            auth_ops.abort_transition(auth_ops.ctx);
            s_net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
            argus_cmd_router_unlock_dispatch();
            xSemaphoreGive(s_net_mutex);
            return ESP_ERR_INVALID_STATE;
        }

        // Revalidate machine state
        argus_state_snapshot_t state_recheck;
        argus_state_mgr_get_snapshot(&state_recheck);
        if (state_recheck.estop_latched ||
            state_recheck.machine_state == ARGUS_STATE_EMERGENCY_STOPPED ||
            state_recheck.machine_state == ARGUS_STATE_FAULTED ||
            (state_recheck.machine_state != ARGUS_STATE_HOLDING &&
             state_recheck.machine_state != ARGUS_STATE_UNLOCKED)) {
            ESP_LOGE(TAG, "Service entry: machine unsafe at grant point (state=%s, estop=%d)",
                     argus_state_mgr_get_state_name(state_recheck.machine_state),
                     (int)state_recheck.estop_latched);
            auth_ops.abort_transition(auth_ops.ctx);
            s_net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
            argus_cmd_router_unlock_dispatch();
            xSemaphoreGive(s_net_mutex);
            return ESP_ERR_INVALID_STATE;
        }

        // Revalidate full AP-only network state (check wifi_get_mode return)
        wifi_mode_t wifi_mode = WIFI_MODE_NULL;
        esp_err_t wifi_err = esp_wifi_get_mode(&wifi_mode);
        if (wifi_err != ESP_OK || wifi_mode != WIFI_MODE_AP ||
            !atomic_load(&s_ap_started) ||
            atomic_load(&s_sta_started) ||
            atomic_load(&s_sta_connected) ||
            atomic_load(&s_sta_ip_acquired)) {
            ESP_LOGE(TAG, "Service entry: network not fully AP-only at grant point (wifi_err=%s, mode=%d)",
                     esp_err_to_name(wifi_err), (int)wifi_mode);
            auth_ops.abort_transition(auth_ops.ctx);
            s_net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
            argus_cmd_router_unlock_dispatch();
            xSemaphoreGive(s_net_mutex);
            return ESP_ERR_INVALID_STATE;
        }

        // Grant LOCAL_SERVICE/<owner>
        esp_err_t grant_err = auth_ops.grant_local(auth_ops.ctx, requested_owner);
        if (grant_err != ESP_OK) {
            auth_ops.abort_transition(auth_ops.ctx);
            s_net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
            argus_cmd_router_unlock_dispatch();
            xSemaphoreGive(s_net_mutex);
            return grant_err;
        }

        s_net_mode = ARGUS_NET_MODE_SERVICE_AP_ONLY;
    }
    argus_cmd_router_unlock_dispatch();
    // ---- END SECOND DISPATCH GATE ----


    ESP_LOGI(TAG, "Coordinated service entry complete. Mode: SERVICE_AP_ONLY, Authority: LOCAL_SERVICE/%s",
             argus_authority_mgr_get_owner_name(requested_owner));

    xSemaphoreGive(s_net_mutex);

    /* Start HTTP server outside s_net_mutex (non-fatal, structural consistency) */
    {
        esp_err_t http_err = argus_http_server_start();
        if (http_err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP server start failed in SERVICE_AP_ONLY: %s", esp_err_to_name(http_err));
        }
    }
    return ESP_OK;

abort_transition:
    ESP_LOGE(TAG, "Service entry failed during transition: %s (%d)", esp_err_to_name(op_err), op_err);
    auth_ops.abort_transition(auth_ops.ctx);
    s_net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
    s_last_error = ARGUS_NET_ERR_STA_SHUTDOWN_FAILED;
    xSemaphoreGive(s_net_mutex);
    return op_err;
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

esp_err_t argus_net_mgr_get_snapshot(argus_net_snapshot_t *out_snap)
{
    if (!out_snap) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
    out_snap->mode = s_net_mode;
    out_snap->last_error = s_last_error;
    out_snap->sta_connected = atomic_load(&s_sta_connected);
    out_snap->sta_ip_acquired = atomic_load(&s_sta_ip_acquired);
    out_snap->ap_started = atomic_load(&s_ap_started);
    out_snap->mqtt_broker_running = argus_mqtt_broker_is_running();
    xSemaphoreGive(s_net_mutex);

    return ESP_OK;
}

esp_err_t argus_net_mgr_request_service_exit(argus_authority_owner_t requested_owner)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    // Step 1: Acquire network-transition serialization
    xSemaphoreTake(s_net_mutex, portMAX_DELAY);

    if (s_net_mode != ARGUS_NET_MODE_SERVICE_AP_ONLY) {
        ESP_LOGE(TAG, "Service exit rejected: mode must be SERVICE_AP_ONLY (current: %s)",
                 argus_net_mgr_get_mode_name(s_net_mode));
        xSemaphoreGive(s_net_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // ---- FIRST DISPATCH GATE: validate authority + set SERVICE_TRANSITION ----
    argus_cmd_router_lock_dispatch();

    // Validate the caller currently owns LOCAL_SERVICE authority (atomic with dispatch)
    argus_authority_snapshot_t auth_snap;
    argus_authority_mgr_get_snapshot(&auth_snap);
    if (auth_snap.mode != ARGUS_AUTHORITY_LOCAL_SERVICE || auth_snap.owner != requested_owner) {
        ESP_LOGE(TAG, "Service exit rejected: caller does not own LOCAL_SERVICE (mode=%d, owner=%d, requested=%d)",
                 (int)auth_snap.mode, (int)auth_snap.owner, (int)requested_owner);
        argus_cmd_router_unlock_dispatch();
        xSemaphoreGive(s_net_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    set_net_mode(ARGUS_NET_MODE_SERVICE_TRANSITION);
    argus_authority_mgr_set_mode(ARGUS_AUTHORITY_SERVICE_TRANSITION, ARGUS_AUTH_OWNER_NONE);

    // Capture the transition generation for revalidation
    argus_authority_snapshot_t exit_auth_post;
    argus_authority_mgr_get_snapshot(&exit_auth_post);
    uint32_t exit_transition_gen = exit_auth_post.generation;

    argus_cmd_router_unlock_dispatch();
    // ---- END FIRST DISPATCH GATE ----

    // Step 4: Controlled stop (no dispatch held)
    esp_err_t stop_err = argus_state_mgr_stop_normal();
    if (stop_err != ESP_OK) {
        ESP_LOGE(TAG, "Service exit: stop_normal failed: %s (%d)", esp_err_to_name(stop_err), stop_err);
        goto exit_fail_closed;
    }

    // Step 5: Verify stopped (bounded 5s, no dispatch held)
    argus_state_snapshot_t state_snap;
    int timeout_ms = 5000;
    bool stopped_ok = false;
    while (timeout_ms > 0) {
        argus_state_mgr_get_snapshot(&state_snap);
        if (state_snap.machine_state == ARGUS_STATE_HOLDING || state_snap.machine_state == ARGUS_STATE_UNLOCKED) {
            stopped_ok = true;
            break;
        }
        if (state_snap.machine_state == ARGUS_STATE_EMERGENCY_STOPPED ||
            state_snap.machine_state == ARGUS_STATE_FAULTED ||
            state_snap.estop_latched) {
            ESP_LOGE(TAG, "Service exit: machine state=%s, estop_latched=%d during stop",
                     argus_state_mgr_get_state_name(state_snap.machine_state), (int)state_snap.estop_latched);
            stop_err = ESP_ERR_INVALID_STATE;
            goto exit_fail_closed;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout_ms -= 50;
    }

    if (!stopped_ok) {
        ESP_LOGE(TAG, "Service exit: stop timeout");
        stop_err = ESP_ERR_TIMEOUT;
        goto exit_fail_closed;
    }

    // ---- SECOND DISPATCH GATE: revalidate authority+generation+machine, revoke ----
    argus_cmd_router_lock_dispatch();
    {
        // Revalidate authority remains SERVICE_TRANSITION/NONE with matching generation
        argus_authority_snapshot_t exit_recheck;
        argus_authority_mgr_get_snapshot(&exit_recheck);
        if (exit_recheck.mode != ARGUS_AUTHORITY_SERVICE_TRANSITION ||
            exit_recheck.owner != ARGUS_AUTH_OWNER_NONE ||
            exit_recheck.generation != exit_transition_gen) {
            ESP_LOGE(TAG, "Service exit: authority changed during transition (gen %lu -> %lu, mode %d)",
                     (unsigned long)exit_transition_gen, (unsigned long)exit_recheck.generation,
                     (int)exit_recheck.mode);
            argus_authority_mgr_set_mode(ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);
            s_net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
            argus_cmd_router_unlock_dispatch();
            xSemaphoreGive(s_net_mutex);
            return ESP_ERR_INVALID_STATE;
        }

        // Revalidate machine is still safe
        argus_state_mgr_get_snapshot(&state_snap);
        if (state_snap.estop_latched ||
            state_snap.machine_state == ARGUS_STATE_EMERGENCY_STOPPED ||
            state_snap.machine_state == ARGUS_STATE_FAULTED ||
            (state_snap.machine_state != ARGUS_STATE_HOLDING &&
             state_snap.machine_state != ARGUS_STATE_UNLOCKED)) {
            ESP_LOGE(TAG, "Service exit: machine unsafe at revocation point");
            argus_authority_mgr_set_mode(ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);
            s_net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
            argus_cmd_router_unlock_dispatch();
            xSemaphoreGive(s_net_mutex);
            return ESP_ERR_INVALID_STATE;
        }

        // Revoke authority to NONE/NONE
        argus_authority_mgr_set_mode(ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);
    }
    argus_cmd_router_unlock_dispatch();
    // ---- END SECOND DISPATCH GATE ----

    // Step 8: Release net mutex before reboot
    xSemaphoreGive(s_net_mutex);

    ESP_LOGI(TAG, "Service exit complete. Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK; // Unreachable

exit_fail_closed:
    // Fail closed: NONE/NONE + NETWORK_FAULT, no reboot
    argus_authority_mgr_set_mode(ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);
    s_net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
    s_last_error = ARGUS_NET_ERR_STA_SHUTDOWN_FAILED;
    xSemaphoreGive(s_net_mutex);
    return stop_err;
}
