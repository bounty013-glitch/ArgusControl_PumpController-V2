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
#include "argus_restart_mgr.h"
#include "argus_service_policy.h"
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

static argus_sta_state_t s_sta_state = ARGUS_STA_DISABLED;
static uint8_t s_last_disconnect_reason = 0;
static argus_disconnect_category_t s_last_disconnect_category = ARGUS_DISCONNECT_CAT_NONE;
static uint32_t s_consecutive_failures = 0;
static argus_wifi_transaction_t s_wifi_transaction;
static uint32_t s_timer_generation = 0;
static _Atomic uint32_t s_active_transaction_generation = 0;
static _Atomic uint32_t s_auto_retry_timer_generation = 0;
static _Atomic uint32_t s_ip_timeout_timer_generation = 0;
static uint32_t s_auth_failures = 0;
static argus_service_cancel_failure_t s_last_service_cancel_failure =
    ARGUS_SERVICE_CANCEL_FAILURE_NONE;
static esp_err_t s_last_service_cancel_error = ESP_OK;
static TimerHandle_t s_auto_retry_timer = NULL;
static TimerHandle_t s_ip_timeout_timer = NULL;

static bool has_valid_commissioned_config(void)
{
    argus_config_payload_t cfg = {0};
    bool has_cfg = false;
    bool valid = argus_nvs_config_get_effective(&cfg, &has_cfg) == ESP_OK &&
                 has_cfg && argus_nvs_config_is_commissioned(&cfg) &&
                 strcmp(cfg.sta_pass, ARGUS_CONFIG_MASK_STRING) != 0;
    memset(&cfg, 0, sizeof(cfg));
    return valid;
}


argus_disconnect_category_t argus_net_classify_disconnect(uint8_t reason, const char **out_name)
{
    const char *name = "UNKNOWN";
    argus_disconnect_category_t cat = ARGUS_DISCONNECT_CAT_UNKNOWN;

    switch(reason) {
        /* Authentication / Credential Failures */
        case WIFI_REASON_AUTH_EXPIRE:                       name = "AUTH_EXPIRE"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_AUTH_LEAVE:                        name = "AUTH_LEAVE"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_NOT_AUTHED:                        name = "NOT_AUTHED"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_ASSOC_NOT_AUTHED:                  name = "ASSOC_NOT_AUTHED"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_MIC_FAILURE:                       name = "MIC_FAILURE"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:            name = "4WAY_HANDSHAKE_TIMEOUT"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:          name = "GROUP_KEY_UPDATE_TIMEOUT"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:                name = "IE_IN_4WAY_DIFFERS"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_GROUP_CIPHER_INVALID:              name = "GROUP_CIPHER_INVALID"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:           name = "PAIRWISE_CIPHER_INVALID"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_AKMP_INVALID:                      name = "AKMP_INVALID"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_UNSUPP_RSN_IE_VERSION:             name = "UNSUPP_RSN_IE_VERSION"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_INVALID_RSN_IE_CAP:                name = "INVALID_RSN_IE_CAP"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_802_1X_AUTH_FAILED:                name = "802_1X_AUTH_FAILED"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_CIPHER_SUITE_REJECTED:             name = "CIPHER_SUITE_REJECTED"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_INVALID_PMKID:                     name = "INVALID_PMKID"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_AUTH_FAIL:                         name = "AUTH_FAIL"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_HANDSHAKE_TIMEOUT:                 name = "HANDSHAKE_TIMEOUT"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: name = "NO_AP_FOUND_W_COMPATIBLE_SECURITY"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: name = "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD"; cat = ARGUS_DISCONNECT_CAT_AUTHENTICATION; break;

        /* AP / Environment Failures */
        case WIFI_REASON_ASSOC_EXPIRE:                      name = "ASSOC_EXPIRE"; cat = ARGUS_DISCONNECT_CAT_AP_UNAVAILABLE; break;
        case WIFI_REASON_ASSOC_TOOMANY:                     name = "ASSOC_TOOMANY"; cat = ARGUS_DISCONNECT_CAT_AP_UNAVAILABLE; break;
        case WIFI_REASON_NOT_ASSOCED:                       name = "NOT_ASSOCED"; cat = ARGUS_DISCONNECT_CAT_AP_UNAVAILABLE; break;
        case WIFI_REASON_ASSOC_LEAVE:                       name = "ASSOC_LEAVE"; cat = ARGUS_DISCONNECT_CAT_AP_UNAVAILABLE; break;
        case WIFI_REASON_BSS_TRANSITION_DISASSOC:           name = "BSS_TRANSITION_DISASSOC"; cat = ARGUS_DISCONNECT_CAT_AP_UNAVAILABLE; break;
        case WIFI_REASON_BEACON_TIMEOUT:                    name = "BEACON_TIMEOUT"; cat = ARGUS_DISCONNECT_CAT_AP_UNAVAILABLE; break;
        case WIFI_REASON_NO_AP_FOUND:                       name = "NO_AP_FOUND"; cat = ARGUS_DISCONNECT_CAT_AP_UNAVAILABLE; break;
        case WIFI_REASON_ASSOC_FAIL:                        name = "ASSOC_FAIL"; cat = ARGUS_DISCONNECT_CAT_AP_UNAVAILABLE; break;
        case WIFI_REASON_CONNECTION_FAIL:                   name = "CONNECTION_FAIL"; cat = ARGUS_DISCONNECT_CAT_AP_UNAVAILABLE; break;
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:     name = "NO_AP_FOUND_IN_RSSI_THRESHOLD"; cat = ARGUS_DISCONNECT_CAT_AP_UNAVAILABLE; break;

        default:                                            name = "UNKNOWN"; cat = ARGUS_DISCONNECT_CAT_UNKNOWN; break;
    }

    if (out_name) {
        *out_name = name;
    }
    return cat;
}

argus_sta_state_t argus_net_evaluate_retry(argus_disconnect_category_t cat, uint32_t auth_failures)
{
    if (cat == ARGUS_DISCONNECT_CAT_AUTHENTICATION && auth_failures >= 3) {
        return ARGUS_STA_ACTION_REQUIRED;
    }
    return ARGUS_STA_RETRY_WAIT;
}

bool argus_net_can_manual_reconnect(argus_network_mode_t net_mode,
                                    argus_sta_state_t sta_state,
                                    bool has_valid_commissioned_config)
{
    if (!has_valid_commissioned_config ||
        (net_mode != ARGUS_NET_MODE_COMMISSIONED_STA &&
         net_mode != ARGUS_NET_MODE_AP_DISCOVERABLE &&
         net_mode != ARGUS_NET_MODE_NETWORK_FAULT)) {
        return false;
    }
    return sta_state == ARGUS_STA_ACTION_REQUIRED ||
           sta_state == ARGUS_STA_RETRY_WAIT ||
           sta_state == ARGUS_STA_IDLE;
}

esp_err_t argus_net_event_post_status(bool queued)
{
    return queued ? ESP_OK : ESP_ERR_NO_MEM;
}

bool argus_net_timer_generation_is_current(uint32_t event_generation,
                                           uint32_t active_generation)
{
    return event_generation == active_generation;
}

esp_err_t argus_net_timer_command_status(bool command_queued)
{
    return command_queued ? ESP_OK : ESP_FAIL;
}

argus_sta_event_action_t argus_net_decide_sta_event(
    argus_network_mode_t mode,
    argus_sta_lifecycle_event_t event,
    uint32_t event_generation,
    uint32_t active_transaction_generation,
    bool sta_started,
    bool sta_connected,
    bool sta_ip_acquired)
{
    if (mode == ARGUS_NET_MODE_SERVICE_AP_ONLY) {
        return (event == ARGUS_STA_EVENT_DISCONNECTED ||
                event == ARGUS_STA_EVENT_STOPPED)
                   ? ARGUS_STA_EVENT_CONFIRM_DISABLED
                   : ARGUS_STA_EVENT_IGNORE;
    }
    if (mode == ARGUS_NET_MODE_SERVICE_TRANSITION ||
        mode == ARGUS_NET_MODE_BOOTSTRAP) {
        return ARGUS_STA_EVENT_IGNORE;
    }
    if (mode != ARGUS_NET_MODE_UNCOMMISSIONED_AP &&
        mode != ARGUS_NET_MODE_COMMISSIONED_STA &&
        mode != ARGUS_NET_MODE_AP_DISCOVERABLE &&
        mode != ARGUS_NET_MODE_NETWORK_FAULT) {
        return ARGUS_STA_EVENT_IGNORE;
    }

    if (event_generation != active_transaction_generation &&
        (event_generation != 0 || active_transaction_generation != 0)) {
        return ARGUS_STA_EVENT_IGNORE;
    }

    switch (event) {
        case ARGUS_STA_EVENT_ASSOCIATED:
            return sta_started && sta_connected
                       ? ARGUS_STA_EVENT_PROCESS : ARGUS_STA_EVENT_IGNORE;
        case ARGUS_STA_EVENT_IP_ACQUIRED:
            return sta_started && sta_connected && sta_ip_acquired
                       ? ARGUS_STA_EVENT_PROCESS : ARGUS_STA_EVENT_IGNORE;
        case ARGUS_STA_EVENT_DISCONNECTED:
            return sta_started && !sta_connected && !sta_ip_acquired
                       ? ARGUS_STA_EVENT_PROCESS : ARGUS_STA_EVENT_IGNORE;
        case ARGUS_STA_EVENT_IP_TIMEOUT:
            return sta_started && !sta_ip_acquired
                       ? ARGUS_STA_EVENT_PROCESS : ARGUS_STA_EVENT_IGNORE;
        case ARGUS_STA_EVENT_STOPPED:
            return !sta_started && !sta_connected && !sta_ip_acquired
                       ? ARGUS_STA_EVENT_PROCESS : ARGUS_STA_EVENT_IGNORE;
        default:
            return ARGUS_STA_EVENT_IGNORE;
    }
}

void argus_net_apply_sta_event_action(argus_network_mode_t mode,
                                      argus_sta_event_action_t action,
                                      argus_sta_state_t *sta_state,
                                      bool *sta_started,
                                      bool *sta_connected,
                                      bool *sta_ip_acquired)
{
    if (!sta_state || !sta_started || !sta_connected || !sta_ip_acquired ||
        action == ARGUS_STA_EVENT_PROCESS) {
        return;
    }
    if (action == ARGUS_STA_EVENT_IGNORE ||
        mode == ARGUS_NET_MODE_SERVICE_TRANSITION ||
        mode == ARGUS_NET_MODE_SERVICE_AP_ONLY) {
        *sta_started = false;
        *sta_connected = false;
        *sta_ip_acquired = false;
    }
    if (mode == ARGUS_NET_MODE_SERVICE_AP_ONLY &&
        action == ARGUS_STA_EVENT_CONFIRM_DISABLED) {
        *sta_state = ARGUS_STA_DISABLED;
    }
}

void argus_net_failure_evidence_record(argus_wifi_failure_evidence_t *evidence,
                                       uint8_t reason,
                                       argus_disconnect_category_t category)
{
    if (!evidence) return;
    evidence->reason = reason;
    evidence->category = category;
    evidence->consecutive_failures++;
    if (category == ARGUS_DISCONNECT_CAT_AUTHENTICATION) {
        evidence->authentication_streak++;
    } else {
        evidence->authentication_streak = 0;
    }
}

void argus_net_failure_evidence_clear(argus_wifi_failure_evidence_t *evidence)
{
    if (evidence) memset(evidence, 0, sizeof(*evidence));
}

uint32_t argus_net_retry_countdown_seconds(uint32_t remaining_ms)
{
    return remaining_ms == 0 ? 0 : ((remaining_ms - 1U) / 1000U) + 1U;
}

uint32_t argus_net_retry_remaining_ms(uint32_t current_tick,
                                      uint32_t expiry_tick,
                                      bool timer_active,
                                      uint32_t timer_generation,
                                      uint32_t current_generation,
                                      argus_sta_state_t sta_state,
                                      uint32_t tick_period_ms)
{
    if (!timer_active || timer_generation != current_generation ||
        sta_state != ARGUS_STA_RETRY_WAIT || tick_period_ms == 0) {
        return 0;
    }

    uint32_t modular_delta = expiry_tick - current_tick;
    if (modular_delta == 0 || modular_delta > (UINT32_MAX / 2U)) {
        return 0;
    }

    uint64_t remaining_ms = (uint64_t)modular_delta * tick_period_ms;
    return remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_ms;
}


static void auto_retry_timer_cb(TimerHandle_t xTimer)
{
    argus_net_event_t evt = {
        .type = ARGUS_NET_EVT_AUTO_RECONNECT_WAKEUP,
        .timer_generation = atomic_load(&s_auto_retry_timer_generation)
    };
    if (argus_net_mgr_post_event(&evt) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to post auto-retry event to queue");
    }
}

static void ip_timeout_timer_cb(TimerHandle_t xTimer)
{
    /* Fake a disconnect reason 0 for IP timeout */
    argus_net_event_t evt = {
        .type = ARGUS_NET_EVT_STA_DISCONNECTED,
        .disconnect_reason = 0,
        .timer_generation = atomic_load(&s_ip_timeout_timer_generation)
    };
    if (argus_net_mgr_post_event(&evt) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to post IP timeout event to queue");
    }
}
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
            argus_net_event_t evt = {
                .type = ARGUS_NET_EVT_STA_STOPPED,
                .transaction_generation = atomic_load(&s_active_transaction_generation)
            };
            argus_net_mgr_post_event(&evt);
        } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            atomic_store(&s_sta_connected, true);
            argus_net_event_t evt = {
                .type = ARGUS_NET_EVT_STA_ASSOCIATED,
                .transaction_generation = atomic_load(&s_active_transaction_generation)
            };
            argus_net_mgr_post_event(&evt);
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            atomic_store(&s_sta_connected, false);
            atomic_store(&s_sta_ip_acquired, false);
            wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
            argus_net_event_t evt = {
                .type = ARGUS_NET_EVT_STA_DISCONNECTED,
                .disconnect_reason = disconn->reason,
                .transaction_generation = atomic_load(&s_active_transaction_generation)
            };
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
        argus_net_event_t evt = {
            .type = ARGUS_NET_EVT_STA_CONNECTED,
            .transaction_generation = atomic_load(&s_active_transaction_generation)
        };
        argus_net_mgr_post_event(&evt);
    }
}

static void argus_wifi_apply_get_production_ops(argus_wifi_apply_ops_t *ops);
static esp_err_t argus_net_mgr_request_service_internal(
    argus_authority_owner_t requested_owner,
    const argus_service_entry_fingerprint_t *expected_preflight);

static void apply_sta_event_action_locked(argus_sta_event_action_t action)
{
    argus_sta_state_t state = s_sta_state;
    bool started = atomic_load(&s_sta_started);
    bool connected = atomic_load(&s_sta_connected);
    bool ip_acquired = atomic_load(&s_sta_ip_acquired);
    argus_net_apply_sta_event_action(s_net_mode, action, &state, &started,
                                     &connected, &ip_acquired);
    s_sta_state = state;
    atomic_store(&s_sta_started, started);
    atomic_store(&s_sta_connected, connected);
    atomic_store(&s_sta_ip_acquired, ip_acquired);
}

static void net_mgr_task(void *pvParameters)
{
    argus_net_event_t evt;
    while (1) {
        if (xQueueReceive(s_event_queue, &evt, portMAX_DELAY) == pdTRUE) {
            switch (evt.type) {
                case ARGUS_NET_EVT_SERVICE_REQUEST:
                    argus_net_mgr_request_service_internal(
                        evt.requested_owner,
                        evt.service_preflight_required ? &evt.service_preflight : NULL);
                    break;

                case ARGUS_NET_EVT_SERVICE_EXIT:
                    argus_net_mgr_request_service_exit(evt.requested_owner);
                    break;

                case ARGUS_NET_EVT_MANUAL_RECONNECT_REQUEST:
                    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
                    if (argus_net_can_manual_reconnect(s_net_mode, s_sta_state,
                                                       has_valid_commissioned_config())) {
                        ESP_LOGI(TAG, "Manual reconnect transaction requested");
                        if (s_wifi_transaction.active) {
                            ESP_LOGW(TAG, "Manual reconnect rejected: Wi-Fi transaction %lu is active",
                                     (unsigned long)s_wifi_transaction.generation);
                            xSemaphoreGive(s_net_mutex);
                            break;
                        }
                        argus_wifi_apply_ops_t ops = {0};
                        argus_wifi_apply_get_production_ops(&ops);
                        uint32_t generation = ++s_timer_generation;
                        atomic_store(&s_active_transaction_generation, generation);
                        esp_err_t err = argus_wifi_transaction_begin_reconnect(
                            &s_wifi_transaction, generation, s_net_mode, &s_sta_state,
                            atomic_load(&s_sta_connected), &ops);
                        if (err != ESP_OK) {
                            atomic_store(&s_active_transaction_generation, 0);
                            s_last_error = ARGUS_NET_ERR_WIFI_TRANSACTION_FAILED;
                            ESP_LOGE(TAG, "Manual reconnect transaction failed: %s", esp_err_to_name(err));
                        }
                    }
                    xSemaphoreGive(s_net_mutex);
                    break;

                case ARGUS_NET_EVT_AUTO_RECONNECT_WAKEUP:
                    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
                    if (!argus_net_timer_generation_is_current(evt.timer_generation,
                                                               s_timer_generation)) {
                        ESP_LOGW(TAG, "Stale auto-reconnect timer event ignored");
                    } else if (s_sta_state == ARGUS_STA_RETRY_WAIT) {
                        ESP_LOGI(TAG, "Auto-reconnect timer fired. Retrying connection...");
                        if (esp_wifi_connect() == ESP_OK) {
                            s_sta_state = ARGUS_STA_CONNECTING;
                        } else {
                            ESP_LOGE(TAG, "esp_wifi_connect() failed");
                            s_sta_state = ARGUS_STA_IDLE;
                        }
                    }
                    xSemaphoreGive(s_net_mutex);
                    break;

                case ARGUS_NET_EVT_STA_ASSOCIATED:
                    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
                    argus_sta_event_action_t assoc_action = argus_net_decide_sta_event(
                        s_net_mode, ARGUS_STA_EVENT_ASSOCIATED,
                        evt.transaction_generation,
                        atomic_load(&s_active_transaction_generation),
                        atomic_load(&s_sta_started), atomic_load(&s_sta_connected),
                        atomic_load(&s_sta_ip_acquired));
                    if (assoc_action == ARGUS_STA_EVENT_PROCESS) {
                        s_sta_state = ARGUS_STA_ASSOCIATED_WAITING_IP;
                        ESP_LOGI(TAG, "STA associated; waiting for IP");
                        atomic_store(&s_ip_timeout_timer_generation, s_timer_generation);
                        if (argus_net_timer_command_status(
                                xTimerStart(s_ip_timeout_timer, 0) == pdPASS) != ESP_OK) {
                            s_sta_state = ARGUS_STA_IDLE;
                            s_last_error = ARGUS_NET_ERR_TIMER_COMMAND_FAILED;
                            ESP_LOGE(TAG, "Failed to schedule STA IP timeout");
                        }
                    } else {
                        apply_sta_event_action_locked(assoc_action);
                        ESP_LOGW(TAG, "Ignored delayed STA association in mode %s",
                                 argus_net_mgr_get_mode_name(s_net_mode));
                    }
                    xSemaphoreGive(s_net_mutex);
                    break;

                case ARGUS_NET_EVT_STA_CONNECTED:
                    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
                    argus_sta_event_action_t ip_action = argus_net_decide_sta_event(
                        s_net_mode, ARGUS_STA_EVENT_IP_ACQUIRED,
                        evt.transaction_generation,
                        atomic_load(&s_active_transaction_generation),
                        atomic_load(&s_sta_started), atomic_load(&s_sta_connected),
                        atomic_load(&s_sta_ip_acquired));
                    if (ip_action != ARGUS_STA_EVENT_PROCESS) {
                        apply_sta_event_action_locked(ip_action);
                        ESP_LOGW(TAG, "Ignored delayed STA IP acquisition in mode %s",
                                 argus_net_mgr_get_mode_name(s_net_mode));
                        xSemaphoreGive(s_net_mutex);
                        break;
                    }
                    s_timer_generation++;
                    if (xTimerStop(s_ip_timeout_timer, 0) != pdPASS ||
                        xTimerStop(s_auto_retry_timer, 0) != pdPASS) {
                        s_last_error = ARGUS_NET_ERR_TIMER_COMMAND_FAILED;
                        ESP_LOGE(TAG, "Failed to stop one or more Wi-Fi timers after IP acquisition");
                    }
                    bool transaction_completed = false;
                    argus_wifi_transaction_handle_got_ip(
                        &s_wifi_transaction, evt.transaction_generation, &transaction_completed);
                    if (transaction_completed) {
                        atomic_store(&s_active_transaction_generation, 0);
                    } else if (!s_wifi_transaction.active &&
                               s_wifi_transaction.state == ARGUS_WIFI_APPLY_FAILED) {
                        argus_wifi_transaction_init(&s_wifi_transaction);
                    }
                    s_sta_state = ARGUS_STA_CONNECTED;
                    s_consecutive_failures = 0;
                    s_auth_failures = 0;
                    s_last_disconnect_category = ARGUS_DISCONNECT_CAT_NONE;
                    s_last_disconnect_reason = 0;
                    s_last_error = ARGUS_NET_ERR_NONE;
                    s_last_service_cancel_failure = ARGUS_SERVICE_CANCEL_FAILURE_NONE;
                    s_last_service_cancel_error = ESP_OK;
                    if (s_net_mode == ARGUS_NET_MODE_UNCOMMISSIONED_AP ||
                        s_net_mode == ARGUS_NET_MODE_AP_DISCOVERABLE ||
                        s_net_mode == ARGUS_NET_MODE_COMMISSIONED_STA ||
                        s_net_mode == ARGUS_NET_MODE_NETWORK_FAULT) {
                        /* Commissioned boot lands in AP_DISCOVERABLE (APSTA).
                         * STA connect does NOT change to COMMISSIONED_STA —
                         * AP and HTTP remain active per operator policy. */
                        if (s_net_mode == ARGUS_NET_MODE_NETWORK_FAULT ||
                            s_net_mode == ARGUS_NET_MODE_UNCOMMISSIONED_AP) {
                            set_net_mode(ARGUS_NET_MODE_AP_DISCOVERABLE);
                        }
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
                    bool is_ip_timeout = evt.disconnect_reason == 0;
                    argus_sta_event_action_t disconnect_action = argus_net_decide_sta_event(
                        s_net_mode, is_ip_timeout ? ARGUS_STA_EVENT_IP_TIMEOUT
                                                  : ARGUS_STA_EVENT_DISCONNECTED,
                        is_ip_timeout ? evt.timer_generation
                                      : evt.transaction_generation,
                        is_ip_timeout ? s_timer_generation
                                      : atomic_load(&s_active_transaction_generation),
                        atomic_load(&s_sta_started), atomic_load(&s_sta_connected),
                        atomic_load(&s_sta_ip_acquired));
                    if (disconnect_action == ARGUS_STA_EVENT_CONFIRM_DISABLED) {
                        apply_sta_event_action_locked(disconnect_action);
                        ESP_LOGI(TAG, "Delayed STA disconnect confirmed disabled service state");
                        xSemaphoreGive(s_net_mutex);
                        break;
                    }
                    if (disconnect_action == ARGUS_STA_EVENT_IGNORE) {
                        ESP_LOGW(TAG, "Ignored delayed STA disconnect in mode %s",
                                 argus_net_mgr_get_mode_name(s_net_mode));
                        xSemaphoreGive(s_net_mutex);
                        break;
                    }
                    if (xTimerStop(s_ip_timeout_timer, 0) != pdPASS) {
                        s_last_error = ARGUS_NET_ERR_TIMER_COMMAND_FAILED;
                    }

                    if (evt.disconnect_reason != 0) {
                        argus_wifi_apply_ops_t ops = {0};
                        argus_wifi_apply_get_production_ops(&ops);
                        bool transaction_handled = false;
                        esp_err_t transaction_err = argus_wifi_transaction_handle_disconnect(
                            &s_wifi_transaction, evt.transaction_generation, &s_sta_state,
                            &ops, &transaction_handled);
                        if (transaction_handled) {
                            if (transaction_err != ESP_OK) {
                                atomic_store(&s_active_transaction_generation, 0);
                                s_last_error = ARGUS_NET_ERR_WIFI_TRANSACTION_FAILED;
                                ESP_LOGE(TAG, "Wi-Fi transaction resume failed: %s",
                                         esp_err_to_name(transaction_err));
                            } else {
                                ESP_LOGI(TAG, "Intentional disconnect matched transaction %lu",
                                         (unsigned long)evt.transaction_generation);
                            }
                            xSemaphoreGive(s_net_mutex);
                            break;
                        }

                        bool transaction_failed = false;
                        argus_wifi_transaction_handle_connection_failure(
                            &s_wifi_transaction, evt.transaction_generation,
                            ESP_ERR_WIFI_CONN, &transaction_failed);
                        if (transaction_failed) {
                            atomic_store(&s_active_transaction_generation, 0);
                            s_last_error = ARGUS_NET_ERR_WIFI_TRANSACTION_FAILED;
                        }
                    }

                    if (s_net_mode == ARGUS_NET_MODE_UNCOMMISSIONED_AP ||
                        s_net_mode == ARGUS_NET_MODE_COMMISSIONED_STA ||
                        s_net_mode == ARGUS_NET_MODE_AP_DISCOVERABLE ||
                        s_net_mode == ARGUS_NET_MODE_NETWORK_FAULT) {
                        ESP_LOGW(TAG, "STA disconnected/IP lost. Revoking SUPERVISORY MQTT authority & stopping broker listener.");
                        argus_authority_mgr_set_mode(ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);
                        argus_mqtt_broker_stop();

                        s_last_disconnect_reason = evt.disconnect_reason;
                        const char *reason_name;

                        if (evt.disconnect_reason == 0) {
                            if (!argus_net_timer_generation_is_current(evt.timer_generation,
                                                                       s_timer_generation)) {
                                ESP_LOGW(TAG, "Stale IP timeout event ignored");
                                xSemaphoreGive(s_net_mutex);
                                break;
                            }
                            reason_name = "IP_ACQUISITION_TIMEOUT";
                            s_last_disconnect_category = ARGUS_DISCONNECT_CAT_IP_TIMEOUT;
                        } else {
                            s_last_disconnect_category = argus_net_classify_disconnect(evt.disconnect_reason, &reason_name);
                        }

                        s_consecutive_failures++;
                        if (s_last_disconnect_category == ARGUS_DISCONNECT_CAT_AUTHENTICATION) {
                            s_auth_failures++;
                        } else {
                            s_auth_failures = 0;
                        }

                        ESP_LOGW(TAG, "STA disconnected: reason=%d (%s), category=%d", evt.disconnect_reason, reason_name, s_last_disconnect_category);

                        s_sta_state = argus_net_evaluate_retry(s_last_disconnect_category, s_auth_failures);
                        if (s_sta_state == ARGUS_STA_ACTION_REQUIRED) {
                            ESP_LOGE(TAG, "Automatic retry suppressed after consecutive authentication failures");
                            ESP_LOGE(TAG, "Operator action required: verify Wi-Fi configuration");
                        } else {
                            ESP_LOGI(TAG, "Retry %lu scheduled in 15 seconds", s_consecutive_failures);
                            uint32_t retry_generation = ++s_timer_generation;
                            atomic_store(&s_auto_retry_timer_generation, retry_generation);
                            if (argus_net_timer_command_status(
                                    xTimerStart(s_auto_retry_timer, 0) == pdPASS) != ESP_OK) {
                                s_sta_state = ARGUS_STA_IDLE;
                                s_last_error = ARGUS_NET_ERR_TIMER_COMMAND_FAILED;
                                ESP_LOGE(TAG, "Failed to schedule automatic reconnect");
                            }
                        }
                    } else {
                        s_sta_state = ARGUS_STA_IDLE;
                    }

                    xSemaphoreGive(s_net_mutex);
                    break;

                case ARGUS_NET_EVT_AP_CLIENT_CONNECTED:
                    break;

                case ARGUS_NET_EVT_AP_CLIENT_DISCONNECTED:
                    break;

                case ARGUS_NET_EVT_APPLY_WIFI_CONFIG: {
                    ESP_LOGI(TAG, "Applying new Wi-Fi credentials dynamically...");
                    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
                    if (s_wifi_transaction.active) {
                        ESP_LOGW(TAG, "Wi-Fi apply rejected: transaction %lu is active",
                                 (unsigned long)s_wifi_transaction.generation);
                        xSemaphoreGive(s_net_mutex);
                        break;
                    }
                    argus_wifi_apply_ops_t ops = {0};
                    argus_wifi_apply_get_production_ops(&ops);
                    uint32_t generation = ++s_timer_generation;
                    atomic_store(&s_active_transaction_generation, generation);
                    esp_err_t err = argus_wifi_transaction_begin_apply(
                        &s_wifi_transaction, generation, s_net_mode, &s_sta_state,
                        atomic_load(&s_sta_connected), &ops);
                    if (err != ESP_OK) {
                        atomic_store(&s_active_transaction_generation, 0);
                        s_last_error = ARGUS_NET_ERR_WIFI_TRANSACTION_FAILED;
                        ESP_LOGE(TAG, "Wi-Fi config apply failed: %s", esp_err_to_name(err));
                    }
                    xSemaphoreGive(s_net_mutex);
                    break;

                case ARGUS_NET_EVT_STA_STOPPED:
                    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
                    argus_sta_event_action_t stop_action = argus_net_decide_sta_event(
                        s_net_mode, ARGUS_STA_EVENT_STOPPED,
                        evt.transaction_generation,
                        atomic_load(&s_active_transaction_generation),
                        atomic_load(&s_sta_started), atomic_load(&s_sta_connected),
                        atomic_load(&s_sta_ip_acquired));
                    if (stop_action == ARGUS_STA_EVENT_CONFIRM_DISABLED) {
                        apply_sta_event_action_locked(stop_action);
                        ESP_LOGI(TAG, "STA stop confirmed disabled service state");
                    } else if (stop_action == ARGUS_STA_EVENT_PROCESS) {
                        s_sta_state = ARGUS_STA_DISABLED;
                        argus_authority_mgr_set_mode(
                            ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);
                        argus_mqtt_broker_stop();
                        set_net_mode(ARGUS_NET_MODE_NETWORK_FAULT);
                        s_last_error = ARGUS_NET_ERR_STA_SHUTDOWN_FAILED;
                        ESP_LOGE(TAG, "Unexpected operational STA stop; entered NETWORK_FAULT");
                    }
                    xSemaphoreGive(s_net_mutex);
                    break;
                }

                case ARGUS_NET_EVT_RESTART_REQUEST: {
                    ESP_LOGI(TAG, "Restart request received. Executing restart transaction...");

                    argus_restart_ops_t restart_ops;
                    argus_restart_get_production_ops(&restart_ops);
                    argus_restart_result_t result = argus_restart_execute(&restart_ops);

                    if (!result.accepted) {
                        ESP_LOGW(TAG, "Restart rejected at step %d. Authority %s. Fail closed.",
                                 result.failed_at_step,
                                 result.authority_revoked ? "revoked" : "intact");
                        /* Fail closed: authority is NOT restored if revoked.
                         * Operator must power-cycle manually. */
                    }
                    /* If accepted, reboot() was called — unreachable */
                    break;
                }

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
    s_auto_retry_timer = xTimerCreate("auto_reconnect", pdMS_TO_TICKS(15000), pdFALSE, NULL, auto_retry_timer_cb);
    s_ip_timeout_timer = xTimerCreate("ip_timeout", pdMS_TO_TICKS(10000), pdFALSE, NULL, ip_timeout_timer_cb);

    if (s_auto_retry_timer == NULL || s_ip_timeout_timer == NULL || s_event_queue == NULL || s_net_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create network manager primitives");
        return ESP_ERR_NO_MEM;
    }
    argus_wifi_transaction_init(&s_wifi_transaction);

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
    bool has_cfg = false;
    bool commissioned = (argus_nvs_config_get_effective(&cfg_payload, &has_cfg) == ESP_OK) &&
                         has_cfg && argus_nvs_config_is_commissioned(&cfg_payload);

    if (!commissioned) {
        // Uncommissioned AP-only mode
        s_net_mode = ARGUS_NET_MODE_UNCOMMISSIONED_AP;
        s_sta_state = ARGUS_STA_DISABLED;
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
        /* Commissioned APSTA mode — AP and HTTP portal remain active.
         * This is an operator-approved field-service policy (see DHR-011).
         * STA connects to configured network for MQTT supervisory path.
         * AP visibility does NOT grant local motor authority. */
        s_net_mode = ARGUS_NET_MODE_AP_DISCOVERABLE;
        argus_authority_mgr_set_mode(ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);

        argus_identity_t id;
        argus_identity_get(&id);

        /* Configure AP interface */
        wifi_config_t ap_config = {0};
        strlcpy((char *)ap_config.ap.ssid, id.service_ssid, sizeof(ap_config.ap.ssid));
        strlcpy((char *)ap_config.ap.password, CONFIG_ARGUS_SERVICE_AP_PASS, sizeof(ap_config.ap.password));
        ap_config.ap.ssid_len = strlen(id.service_ssid);
        ap_config.ap.max_connection = 1;
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

        /* Configure STA interface */
        wifi_config_t sta_config = {0};
        strlcpy((char *)sta_config.sta.ssid, cfg_payload.sta_ssid, sizeof(sta_config.sta.ssid));
        strlcpy((char *)sta_config.sta.password, cfg_payload.sta_pass, sizeof(sta_config.sta.password));

        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        esp_wifi_start();
        s_sta_state = ARGUS_STA_CONNECTING;
        if (esp_wifi_connect() != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect() failed");
            s_sta_state = ARGUS_STA_IDLE;
        }

        /* Start HTTP portal — non-fatal if it fails */
        esp_err_t http_err = argus_http_server_start();
        if (http_err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP server start failed in AP_DISCOVERABLE: %s", esp_err_to_name(http_err));
        }
    }

    if (xTaskCreate(net_mgr_task, "argus_net_mgr", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create network manager task");
        return ESP_ERR_NO_MEM;
    }
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

        if (s_net_mode == ARGUS_NET_MODE_SERVICE_TRANSITION || s_net_mode == ARGUS_NET_MODE_SERVICE_AP_ONLY) {
            s_timer_generation++;
            atomic_store(&s_active_transaction_generation, 0);
            argus_wifi_transaction_cancel(&s_wifi_transaction);
            if (xTimerStop(s_auto_retry_timer, 0) != pdPASS ||
                xTimerStop(s_ip_timeout_timer, 0) != pdPASS) {
                s_last_error = ARGUS_NET_ERR_TIMER_COMMAND_FAILED;
                ESP_LOGE(TAG, "Network mode transition could not cancel all Wi-Fi timers");
            }
        }
    }
}

esp_err_t argus_net_mgr_enable_ap_discoverable(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
    if (s_net_mode == ARGUS_NET_MODE_AP_DISCOVERABLE) {
        /* Already in APSTA mode — idempotent success */
        ESP_LOGI(TAG, "Already in AP_DISCOVERABLE mode (idempotent).");
        xSemaphoreGive(s_net_mutex);
        return ESP_OK;
    }
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
        !ops->set_sta_disabled || !ops->verify_machine_safe) {
        return ESP_ERR_INVALID_ARG;
    }

    if (requested_owner != ARGUS_AUTH_OWNER_BROWSER && requested_owner != ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI) {
        return ESP_ERR_INVALID_ARG;
    }

    // Prepare transition (SERVICE_TRANSITION/NONE) and increment generation
    esp_err_t prep_err = auth_ops->prepare_transition(auth_ops->ctx);
    if (prep_err != ESP_OK) {
        auth_ops->abort_transition(auth_ops->ctx);
        *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
        return prep_err;
    }

    *net_mode = ARGUS_NET_MODE_SERVICE_TRANSITION;

    // Drop dispatch lock
    if (ops->unlock_dispatch) ops->unlock_dispatch(ops->ctx);

    // Drop network lock to stop HTTP safely
    if (ops->unlock_net) ops->unlock_net(ops->ctx);
    esp_err_t http_stop_err = ESP_FAIL;
    if (ops->stop_http) {
        http_stop_err = ops->stop_http(ops->ctx);
    } else {
        http_stop_err = ESP_OK; // Missing hook acts as success for tests
    }
    if (ops->lock_net) ops->lock_net(ops->ctx);

    esp_err_t op_err = ESP_OK;
    const char* fail_stage = "unknown";

    op_err = ops->request_normal_stop(ops->ctx);
    if (op_err != ESP_OK) { fail_stage = "ensure machine stopped"; goto abort_transition; }

    op_err = ops->verify_stopped(ops->ctx);
    if (op_err != ESP_OK) { fail_stage = "verify machine stopped"; goto abort_transition; }

    op_err = ops->stop_broker(ops->ctx);
    if (op_err != ESP_OK) { fail_stage = "ensure broker stopped"; goto abort_transition; }

    op_err = ops->verify_broker_stopped(ops->ctx);
    if (op_err != ESP_OK) { fail_stage = "verify broker stopped"; goto abort_transition; }

    op_err = ops->disconnect_sta(ops->ctx);
    if (op_err != ESP_OK) { fail_stage = "ensure STA disconnected"; goto abort_transition; }

    op_err = ops->verify_sta_disconnected(ops->ctx);
    if (op_err != ESP_OK) { fail_stage = "ensure STA disconnected (verify disconnected)"; goto abort_transition; }

    op_err = ops->verify_sta_ip_released(ops->ctx);
    if (op_err != ESP_OK) { fail_stage = "verify STA IP released"; goto abort_transition; }

    op_err = ops->set_wifi_ap_only(ops->ctx);
    if (op_err != ESP_OK) { fail_stage = "ensure AP-only mode"; goto abort_transition; }

    op_err = ops->verify_ap_active(ops->ctx);
    if (op_err != ESP_OK) { fail_stage = "verify AP active"; goto abort_transition; }

    op_err = ops->set_sta_disabled(ops->ctx);
    if (op_err != ESP_OK) { fail_stage = "commit STA disabled state"; goto abort_transition; }

    op_err = ops->verify_machine_safe(ops->ctx);
    if (op_err != ESP_OK) { fail_stage = "verify machine safe"; goto abort_transition; }

    // Re-acquire dispatch lock and revalidate
    if (ops->lock_dispatch) ops->lock_dispatch(ops->ctx);

    if (ops->revalidate_network) {
        op_err = ops->revalidate_network(ops->ctx);
        if (op_err != ESP_OK) {
            fail_stage = "revalidate state at grant point";
            goto abort_transition_with_dispatch;
        }
    }

    // Grant LOCAL_SERVICE/<owner> last
    op_err = auth_ops->grant_local(auth_ops->ctx, requested_owner);
    if (op_err != ESP_OK) {
        fail_stage = "grant LOCAL_SERVICE authority";
        goto abort_transition_with_dispatch;
    }

    *net_mode = ARGUS_NET_MODE_SERVICE_AP_ONLY;

    if (ops->unlock_dispatch) ops->unlock_dispatch(ops->ctx);
    if (ops->unlock_net) ops->unlock_net(ops->ctx);

    if (ops->start_http) {
        ops->start_http(ops->ctx);
    }

    // Orchestrator returns with locks DROPPED, caller is responsible for maintaining logic.
    return ESP_OK;

abort_transition_with_dispatch:
    ESP_LOGE(TAG, "Service entry failed during transition (%s): %s", fail_stage, esp_err_to_name(op_err));
    auth_ops->abort_transition(auth_ops->ctx);
    *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;
    if (ops->unlock_dispatch) ops->unlock_dispatch(ops->ctx);
    goto abort_restore_http;

abort_transition:
    ESP_LOGE(TAG, "Service entry failed during transition (%s): %s", fail_stage, esp_err_to_name(op_err));
    auth_ops->abort_transition(auth_ops->ctx);
    *net_mode = ARGUS_NET_MODE_NETWORK_FAULT;

abort_restore_http:
    if (http_stop_err == ESP_OK) {
        if (ops->unlock_net) ops->unlock_net(ops->ctx);
        if (ops->start_http) ops->start_http(ops->ctx);
        // Note: We return with net unlocked. The caller expects this.
        return op_err;
    }

    // If HTTP was not stopped, we still need to drop net_mutex before returning to match success path.
    if (ops->unlock_net) ops->unlock_net(ops->ctx);
    return op_err;
}

static esp_err_t prod_request_normal_stop(void *ctx) {
    (void)ctx;
    argus_state_snapshot_t state_snap;
    argus_state_mgr_get_snapshot(&state_snap);
    if (state_snap.machine_state == ARGUS_STATE_HOLDING || state_snap.machine_state == ARGUS_STATE_UNLOCKED) {
        return ESP_OK; // Already stopped
    }
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
    if (!argus_mqtt_broker_is_running()) {
        return ESP_OK; // Already stopped
    }
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

esp_err_t argus_net_mgr_eval_sta_disconnect_req(wifi_mode_t wifi_mode, esp_err_t wifi_mode_err, bool sta_started, bool sta_connected, bool sta_ip_acquired, bool *out_disconnect_needed)
{
    if (!out_disconnect_needed) return ESP_ERR_INVALID_ARG;
    *out_disconnect_needed = true; // Default to requiring disconnect

    // If driver is AP-only, STA should not have connection or IP.
    if (wifi_mode_err == ESP_OK && wifi_mode == WIFI_MODE_AP) {
        if (sta_connected || sta_ip_acquired) {
            ESP_LOGE(TAG, "Contradictory state: WIFI_MODE_AP but STA flags active (conn:%d, ip:%d)",
                     sta_connected, sta_ip_acquired);
            return ESP_ERR_INVALID_STATE;
        }
        *out_disconnect_needed = false;
        return ESP_OK;
    }

    // If STA is already disconnected without an IP, we don't need a redundant disconnect command.
    if (!sta_connected && !sta_ip_acquired) {
        *out_disconnect_needed = false;
        return ESP_OK;
    }

    return ESP_OK;
}

static esp_err_t prod_disconnect_sta(void *ctx) {
    (void)ctx;

    wifi_mode_t wifi_mode = WIFI_MODE_NULL;
    esp_err_t wifi_err = esp_wifi_get_mode(&wifi_mode);

    bool sta_started = atomic_load(&s_sta_started);
    bool sta_connected = atomic_load(&s_sta_connected);
    bool sta_ip_acquired = atomic_load(&s_sta_ip_acquired);

    bool disconnect_needed = false;
    esp_err_t eval_err = argus_net_mgr_eval_sta_disconnect_req(wifi_mode, wifi_err, sta_started, sta_connected, sta_ip_acquired, &disconnect_needed);
    if (eval_err != ESP_OK) {
        return eval_err;
    }

    if (!disconnect_needed) {
        ESP_LOGI(TAG, "STA already absent/disconnected, skipping esp_wifi_disconnect");
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_STARTED || err == ESP_ERR_WIFI_NOT_INIT) {
        return ESP_OK; // Not started, so disconnected
    }
    // ESP_ERR_WIFI_NOT_CONNECT is also technically okay, but we preserve original error if it wasn't requested.
    return err;
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
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP) {
        return ESP_OK; // Already AP only
    }
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
static esp_err_t prod_stop_http(void *ctx) {
    (void)ctx;
    return argus_http_server_stop();
}
static esp_err_t prod_start_http(void *ctx) {
    (void)ctx;
    return argus_http_server_start();
}
static esp_err_t prod_unlock_net(void *ctx) {
    (void)ctx;
    xSemaphoreGive(s_net_mutex);
    return ESP_OK;
}
static esp_err_t prod_lock_net(void *ctx) {
    (void)ctx;
    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
    return ESP_OK;
}
static esp_err_t prod_lock_dispatch(void *ctx) {
    (void)ctx;
    argus_cmd_router_lock_dispatch();
    return ESP_OK;
}
static esp_err_t prod_unlock_dispatch(void *ctx) {
    (void)ctx;
    argus_cmd_router_unlock_dispatch();
    return ESP_OK;
}
static esp_err_t prod_revalidate_network(void *ctx) {
    (void)ctx;
    wifi_mode_t wifi_mode = WIFI_MODE_NULL;
    esp_err_t wifi_err = esp_wifi_get_mode(&wifi_mode);
    if (wifi_err != ESP_OK || wifi_mode != WIFI_MODE_AP ||
        !atomic_load(&s_ap_started) ||
        atomic_load(&s_sta_started) ||
        atomic_load(&s_sta_connected) ||
        atomic_load(&s_sta_ip_acquired)) {
        ESP_LOGE(TAG, "Service entry: network not fully AP-only at grant point (wifi_err=%s, mode=%d)",
                 esp_err_to_name(wifi_err), (int)wifi_mode);
        return ESP_ERR_INVALID_STATE;
    }

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
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}



static void wifi_transaction_scrub_config(argus_wifi_transaction_t *txn)
{
    memset(&txn->pending_config, 0, sizeof(txn->pending_config));
    txn->config_staged = false;
}

static esp_err_t prod_set_sta_disabled(void *ctx)
{
    (void)ctx;
    if (atomic_load(&s_sta_started) || atomic_load(&s_sta_connected) ||
        atomic_load(&s_sta_ip_acquired)) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sta_state = ARGUS_STA_DISABLED;
    return ESP_OK;
}

static esp_err_t wifi_transaction_finish_error(argus_wifi_transaction_t *txn,
                                               esp_err_t err)
{
    wifi_transaction_scrub_config(txn);
    txn->active = false;
    txn->intentional_disconnect_requested = false;
    txn->state = ARGUS_WIFI_APPLY_FAILED;
    txn->last_error = err;
    return err;
}

void argus_wifi_transaction_init(argus_wifi_transaction_t *txn)
{
    if (!txn) return;
    memset(txn, 0, sizeof(*txn));
    txn->state = ARGUS_WIFI_APPLY_IDLE;
    txn->last_error = ESP_OK;
}

bool argus_wifi_transaction_event_matches(const argus_wifi_transaction_t *txn,
                                          uint32_t event_generation)
{
    return txn && txn->active && txn->generation != 0 &&
           txn->generation == event_generation;
}

static esp_err_t wifi_transaction_validate_inputs(argus_wifi_transaction_t *txn,
                                                  argus_network_mode_t net_mode,
                                                  argus_sta_state_t *sta_state,
                                                  const argus_wifi_apply_ops_t *ops)
{
    if (!txn || !sta_state || !ops) return ESP_ERR_INVALID_ARG;
    if (!ops->stop_timers || !ops->revoke_supervisory || !ops->stop_broker ||
        !ops->verify_broker_stopped || !ops->load_config || !ops->validate_config ||
        !ops->disconnect_sta || !ops->apply_sta_config || !ops->connect_sta) {
        return ESP_ERR_INVALID_ARG;
    }
    if (txn->active) return ESP_ERR_INVALID_STATE;
    if (net_mode != ARGUS_NET_MODE_AP_DISCOVERABLE &&
        net_mode != ARGUS_NET_MODE_UNCOMMISSIONED_AP &&
        net_mode != ARGUS_NET_MODE_COMMISSIONED_STA &&
        net_mode != ARGUS_NET_MODE_NETWORK_FAULT) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t wifi_transaction_prepare(argus_wifi_transaction_t *txn,
                                          uint32_t generation,
                                          argus_wifi_transaction_kind_t kind,
                                          argus_network_mode_t net_mode,
                                          argus_sta_state_t *sta_state,
                                          const argus_wifi_apply_ops_t *ops)
{
    esp_err_t err = wifi_transaction_validate_inputs(txn, net_mode, sta_state, ops);
    if (err != ESP_OK) return err;
    if (generation == 0) return ESP_ERR_INVALID_ARG;

    argus_wifi_transaction_init(txn);
    txn->active = true;
    txn->kind = kind;
    txn->generation = generation;
    txn->state = ARGUS_WIFI_APPLY_PREPARING;

    err = ops->stop_timers(ops->ctx);
    if (err != ESP_OK) return wifi_transaction_finish_error(txn, err);

    err = ops->revoke_supervisory(ops->ctx);
    if (err != ESP_OK) return wifi_transaction_finish_error(txn, err);
    txn->authority_revoked = true;

    err = ops->stop_broker(ops->ctx);
    if (err != ESP_OK) return wifi_transaction_finish_error(txn, err);

    err = ops->verify_broker_stopped(ops->ctx);
    if (err != ESP_OK) return wifi_transaction_finish_error(txn, err);
    txn->broker_stopped = true;

    bool has_cfg = false;
    err = ops->load_config(ops->ctx, &txn->pending_config, &has_cfg);
    if (err != ESP_OK) return wifi_transaction_finish_error(txn, err);
    txn->config_staged = has_cfg;

    err = ops->validate_config(ops->ctx, &txn->pending_config, has_cfg);
    if (err != ESP_OK) return wifi_transaction_finish_error(txn, err);
    return ESP_OK;
}

static esp_err_t wifi_transaction_connect(argus_wifi_transaction_t *txn,
                                          argus_sta_state_t *sta_state,
                                          const argus_wifi_apply_ops_t *ops)
{
    esp_err_t err;
    if (txn->kind == ARGUS_WIFI_TXN_APPLY_CONFIG) {
        txn->state = ARGUS_WIFI_APPLY_APPLYING_CONFIG;
        err = ops->apply_sta_config(ops->ctx, &txn->pending_config);
        if (err != ESP_OK) return wifi_transaction_finish_error(txn, err);
        txn->config_applied = true;
    }

    wifi_transaction_scrub_config(txn);
    err = ops->connect_sta(ops->ctx);
    if (err != ESP_OK) {
        *sta_state = ARGUS_STA_IDLE;
        return wifi_transaction_finish_error(txn, err);
    }
    *sta_state = ARGUS_STA_CONNECTING;
    txn->state = ARGUS_WIFI_APPLY_CONNECTING;
    return ESP_OK;
}

static esp_err_t wifi_transaction_begin(argus_wifi_transaction_t *txn,
                                        uint32_t generation,
                                        argus_wifi_transaction_kind_t kind,
                                        argus_network_mode_t net_mode,
                                        argus_sta_state_t *sta_state,
                                        bool sta_connected,
                                        const argus_wifi_apply_ops_t *ops)
{
    esp_err_t err = wifi_transaction_prepare(txn, generation, kind, net_mode, sta_state, ops);
    if (err != ESP_OK) return err;

    if (sta_connected) {
        txn->state = ARGUS_WIFI_APPLY_WAITING_DISCONNECT;
        txn->intentional_disconnect_requested = true;
        err = ops->disconnect_sta(ops->ctx);
        if (err != ESP_OK) return wifi_transaction_finish_error(txn, err);
        return ESP_OK;
    }
    return wifi_transaction_connect(txn, sta_state, ops);
}

esp_err_t argus_wifi_transaction_begin_apply(argus_wifi_transaction_t *txn,
                                             uint32_t generation,
                                             argus_network_mode_t net_mode,
                                             argus_sta_state_t *sta_state,
                                             bool sta_connected,
                                             const argus_wifi_apply_ops_t *ops)
{
    return wifi_transaction_begin(txn, generation, ARGUS_WIFI_TXN_APPLY_CONFIG,
                                  net_mode, sta_state, sta_connected, ops);
}

esp_err_t argus_wifi_transaction_begin_reconnect(argus_wifi_transaction_t *txn,
                                                 uint32_t generation,
                                                 argus_network_mode_t net_mode,
                                                 argus_sta_state_t *sta_state,
                                                 bool sta_connected,
                                                 const argus_wifi_apply_ops_t *ops)
{
    return wifi_transaction_begin(txn, generation, ARGUS_WIFI_TXN_MANUAL_RECONNECT,
                                  net_mode, sta_state, sta_connected, ops);
}

esp_err_t argus_wifi_transaction_handle_disconnect(argus_wifi_transaction_t *txn,
                                                   uint32_t event_generation,
                                                   argus_sta_state_t *sta_state,
                                                   const argus_wifi_apply_ops_t *ops,
                                                   bool *out_handled)
{
    if (!txn || !sta_state || !ops || !out_handled) return ESP_ERR_INVALID_ARG;
    *out_handled = false;
    if (!argus_wifi_transaction_event_matches(txn, event_generation) ||
        txn->state != ARGUS_WIFI_APPLY_WAITING_DISCONNECT ||
        !txn->intentional_disconnect_requested) {
        return ESP_OK;
    }
    *out_handled = true;
    txn->intentional_disconnect_requested = false;
    return wifi_transaction_connect(txn, sta_state, ops);
}

esp_err_t argus_wifi_transaction_handle_got_ip(argus_wifi_transaction_t *txn,
                                               uint32_t event_generation,
                                               bool *out_completed)
{
    if (!txn || !out_completed) return ESP_ERR_INVALID_ARG;
    *out_completed = false;
    if (!argus_wifi_transaction_event_matches(txn, event_generation) ||
        txn->state != ARGUS_WIFI_APPLY_CONNECTING) {
        return ESP_OK;
    }
    wifi_transaction_scrub_config(txn);
    txn->active = false;
    txn->state = ARGUS_WIFI_APPLY_COMPLETE;
    txn->last_error = ESP_OK;
    *out_completed = true;
    return ESP_OK;
}

esp_err_t argus_wifi_transaction_handle_connection_failure(
    argus_wifi_transaction_t *txn,
    uint32_t event_generation,
    esp_err_t connection_error,
    bool *out_failed)
{
    if (!txn || !out_failed) return ESP_ERR_INVALID_ARG;
    *out_failed = false;
    if (!argus_wifi_transaction_event_matches(txn, event_generation) ||
        txn->state != ARGUS_WIFI_APPLY_CONNECTING) {
        return ESP_OK;
    }
    *out_failed = true;
    return wifi_transaction_finish_error(txn, connection_error);
}

void argus_wifi_transaction_cancel(argus_wifi_transaction_t *txn)
{
    if (!txn) return;
    wifi_transaction_scrub_config(txn);
    txn->active = false;
    txn->intentional_disconnect_requested = false;
    txn->state = ARGUS_WIFI_APPLY_CANCELLED;
    txn->last_error = ESP_ERR_INVALID_STATE;
}

esp_err_t argus_net_cancel_recovery_for_service(
    const argus_service_recovery_cancel_ops_t *ops,
    argus_service_cancel_failure_t *out_failure)
{
    if (out_failure) *out_failure = ARGUS_SERVICE_CANCEL_FAILURE_NONE;
    if (!ops || !ops->transaction || !ops->timer_generation ||
        !ops->active_transaction_generation ||
        !ops->auto_retry_timer_generation || !ops->ip_timeout_timer_generation ||
        !ops->stop_retry_timer || !ops->stop_ip_timeout_timer) {
        return ESP_ERR_INVALID_ARG;
    }

    (*ops->timer_generation)++;
    atomic_store(ops->active_transaction_generation, 0);
    atomic_store(ops->auto_retry_timer_generation, 0);
    atomic_store(ops->ip_timeout_timer_generation, 0);
    argus_wifi_transaction_cancel(ops->transaction);

    esp_err_t retry_err = ops->stop_retry_timer(ops->ctx);
    esp_err_t ip_err = ops->stop_ip_timeout_timer(ops->ctx);
    if (retry_err != ESP_OK) {
        if (out_failure) *out_failure = ARGUS_SERVICE_CANCEL_FAILURE_RETRY_TIMER;
        return retry_err;
    }
    if (ip_err != ESP_OK) {
        if (out_failure) *out_failure = ARGUS_SERVICE_CANCEL_FAILURE_IP_TIMER;
        return ip_err;
    }
    return ESP_OK;
}

void argus_net_record_service_cancel_failure(
    argus_service_cancel_state_t *state,
    argus_service_cancel_failure_t failure,
    esp_err_t error)
{
    if (!state || failure == ARGUS_SERVICE_CANCEL_FAILURE_NONE ||
        error == ESP_OK) {
        return;
    }
    state->sta_state = ARGUS_STA_ACTION_REQUIRED;
    state->net_error = ARGUS_NET_ERR_TIMER_COMMAND_FAILED;
    state->cancel_failure = failure;
    state->cancel_error = error;
}

const char *argus_net_service_cancel_guidance(
    argus_service_cancel_failure_t failure)
{
    if (failure == ARGUS_SERVICE_CANCEL_FAILURE_RETRY_TIMER) {
        return "Auto-retry timer stop failed. Use Reconnect Wi-Fi or Enter Local Service.";
    }
    if (failure == ARGUS_SERVICE_CANCEL_FAILURE_IP_TIMER) {
        return "IP-timeout timer stop failed. Use Reconnect Wi-Fi or Enter Local Service.";
    }
    return "";
}

esp_err_t argus_net_service_commit_recovery(
    argus_svc_policy_result_t policy,
    const argus_service_entry_fingerprint_t *expected,
    const argus_service_entry_fingerprint_t *actual,
    const argus_service_commit_ops_t *ops,
    argus_service_cancel_failure_t *out_failure)
{
    if (out_failure) *out_failure = ARGUS_SERVICE_CANCEL_FAILURE_NONE;
    if (!actual || !ops || !ops->cancel_recovery) {
        return ESP_ERR_INVALID_ARG;
    }
    if (policy != ARGUS_SVC_POLICY_OK ||
        (expected && !argus_service_entry_fingerprint_matches(expected, actual))) {
        return ESP_ERR_INVALID_STATE;
    }
    return ops->cancel_recovery(ops->ctx, out_failure);
}

esp_err_t argus_net_mgr_orchestrate_wifi_apply(argus_network_mode_t *net_mode,
                                               argus_sta_state_t *sta_state,
                                               bool sta_connected,
                                               const argus_wifi_apply_ops_t *ops)
{
    if (!net_mode) return ESP_ERR_INVALID_ARG;
    argus_wifi_transaction_t txn;
    argus_wifi_transaction_init(&txn);
    return argus_wifi_transaction_begin_apply(&txn, 1, *net_mode, sta_state,
                                              sta_connected, ops);
}

// Prod ops implementation
static esp_err_t wifi_apply_stop_timers(void *ctx) {
    BaseType_t retry_result = xTimerStop(s_auto_retry_timer, 0);
    BaseType_t ip_result = xTimerStop(s_ip_timeout_timer, 0);
    return (retry_result == pdPASS && ip_result == pdPASS) ? ESP_OK : ESP_FAIL;
}

static esp_err_t wifi_apply_revoke_supervisory(void *ctx) {
    return argus_authority_mgr_set_mode(ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);
}

static esp_err_t wifi_apply_stop_broker(void *ctx) {
    return argus_mqtt_broker_stop();
}

static esp_err_t wifi_apply_verify_broker_stopped(void *ctx) {
    return argus_mqtt_broker_is_running() ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static esp_err_t wifi_apply_load_config(void *ctx, wifi_config_t *out_cfg, bool *has_cfg) {
    argus_config_payload_t cfg = {0};
    esp_err_t err = argus_nvs_config_get_effective(&cfg, has_cfg);
    if (err == ESP_OK && *has_cfg && argus_nvs_config_is_commissioned(&cfg) &&
        strcmp(cfg.sta_pass, ARGUS_CONFIG_MASK_STRING) != 0) {
        memset(out_cfg, 0, sizeof(wifi_config_t));
        strlcpy((char *)out_cfg->sta.ssid, cfg.sta_ssid, sizeof(out_cfg->sta.ssid));
        strlcpy((char *)out_cfg->sta.password, cfg.sta_pass, sizeof(out_cfg->sta.password));
        out_cfg->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else if (err == ESP_OK) {
        *has_cfg = false;
    }
    memset(&cfg, 0, sizeof(cfg));
    return err;
}

static esp_err_t wifi_apply_validate_config(void *ctx, const wifi_config_t *cfg, bool has_cfg) {
    if (!cfg || !has_cfg) return ESP_ERR_NOT_FOUND;
    size_t ssid_len = strnlen((const char *)cfg->sta.ssid, sizeof(cfg->sta.ssid));
    size_t pass_len = strnlen((const char *)cfg->sta.password, sizeof(cfg->sta.password));
    if (ssid_len == 0 || ssid_len >= sizeof(cfg->sta.ssid) ||
        pass_len < ARGUS_CFG_STA_PASS_MIN || pass_len > ARGUS_CFG_STA_PASS_MAX ||
        strcmp((const char *)cfg->sta.password, ARGUS_CONFIG_MASK_STRING) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t wifi_apply_disconnect_sta(void *ctx) {
    return esp_wifi_disconnect();
}

static esp_err_t wifi_apply_apply_sta_config(void *ctx, const wifi_config_t *cfg) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) return err;
    if (mode == WIFI_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) return err;
    } else if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_wifi_set_config(WIFI_IF_STA, (wifi_config_t *)cfg);
}

static esp_err_t wifi_apply_connect_sta(void *ctx) {
    return esp_wifi_connect();
}

static void argus_wifi_apply_get_production_ops(argus_wifi_apply_ops_t *ops) {
    ops->stop_timers = wifi_apply_stop_timers;
    ops->revoke_supervisory = wifi_apply_revoke_supervisory;
    ops->stop_broker = wifi_apply_stop_broker;
    ops->verify_broker_stopped = wifi_apply_verify_broker_stopped;
    ops->load_config = wifi_apply_load_config;
    ops->validate_config = wifi_apply_validate_config;
    ops->disconnect_sta = wifi_apply_disconnect_sta;
    ops->apply_sta_config = wifi_apply_apply_sta_config;
    ops->connect_sta = wifi_apply_connect_sta;
    ops->ctx = NULL;
}

static void populate_net_snapshot_locked(argus_net_snapshot_t *out_snap)
{
    memset(out_snap, 0, sizeof(*out_snap));
    out_snap->mode = s_net_mode;
    out_snap->last_error = s_last_error;
    out_snap->sta_connected = atomic_load(&s_sta_connected);
    out_snap->sta_ip_acquired = atomic_load(&s_sta_ip_acquired);
    out_snap->ap_started = atomic_load(&s_ap_started);

    argus_mqtt_broker_lifecycle_obs_t broker_obs = {0};
    out_snap->mqtt_broker_observable =
        argus_mqtt_broker_get_lifecycle_obs(&broker_obs) == ESP_OK;
    out_snap->mqtt_broker_running = out_snap->mqtt_broker_observable && broker_obs.running;
    out_snap->mqtt_broker_stopped = out_snap->mqtt_broker_observable && broker_obs.stopped;
    out_snap->commissioned = has_valid_commissioned_config();
    out_snap->sta_state = s_sta_state;
    out_snap->last_disconnect_category = s_last_disconnect_category;
    out_snap->last_disconnect_reason = s_last_disconnect_reason;
    out_snap->consecutive_failures = s_consecutive_failures;
    out_snap->seconds_until_retry = argus_net_mgr_get_retry_seconds();
    out_snap->action_required = argus_net_mgr_is_action_required();
    out_snap->manual_reconnect_permitted = argus_net_can_manual_reconnect(
        s_net_mode, s_sta_state, out_snap->commissioned);
    out_snap->apply_state = s_wifi_transaction.state;
    out_snap->timer_generation = s_timer_generation;
    out_snap->wifi_transaction_active = s_wifi_transaction.active;
    out_snap->transaction_generation = s_wifi_transaction.generation;
    out_snap->auto_retry_timer_active =
        s_auto_retry_timer && xTimerIsTimerActive(s_auto_retry_timer);
    out_snap->auto_retry_timer_generation =
        atomic_load(&s_auto_retry_timer_generation);
    out_snap->ip_timeout_timer_active =
        s_ip_timeout_timer && xTimerIsTimerActive(s_ip_timeout_timer);
    out_snap->ip_timeout_timer_generation =
        atomic_load(&s_ip_timeout_timer_generation);
    out_snap->last_service_cancel_failure = s_last_service_cancel_failure;
    out_snap->last_service_cancel_error = s_last_service_cancel_error;

    if (out_snap->sta_ip_acquired && s_netif_sta) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_netif_sta, &ip_info) == ESP_OK) {
            snprintf(out_snap->sta_ip_address, sizeof(out_snap->sta_ip_address),
                     IPSTR, IP2STR(&ip_info.ip));
        }
    }
}

static esp_err_t service_stop_retry_timer(void *ctx)
{
    (void)ctx;
    return xTimerStop(s_auto_retry_timer, 0) == pdPASS ? ESP_OK : ESP_FAIL;
}

static esp_err_t service_stop_ip_timeout_timer(void *ctx)
{
    (void)ctx;
    return xTimerStop(s_ip_timeout_timer, 0) == pdPASS ? ESP_OK : ESP_FAIL;
}

static esp_err_t service_commit_cancel_recovery(
    void *ctx, argus_service_cancel_failure_t *out_failure)
{
    return argus_net_cancel_recovery_for_service(
        (const argus_service_recovery_cancel_ops_t *)ctx, out_failure);
}

esp_err_t argus_net_mgr_request_manual_reconnect(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
    if (s_net_mode == ARGUS_NET_MODE_UNCOMMISSIONED_AP ) {
        xSemaphoreGive(s_net_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!argus_net_can_manual_reconnect(s_net_mode, s_sta_state,
                                        has_valid_commissioned_config())) {
        xSemaphoreGive(s_net_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreGive(s_net_mutex);

    argus_net_event_t evt = { .type = ARGUS_NET_EVT_MANUAL_RECONNECT_REQUEST };
    return argus_net_mgr_post_event(&evt);
}

static esp_err_t argus_net_mgr_request_service_internal(
    argus_authority_owner_t requested_owner,
    const argus_service_entry_fingerprint_t *expected_preflight)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    if (requested_owner != ARGUS_AUTH_OWNER_BROWSER && requested_owner != ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI) {
        ESP_LOGE(TAG, "Service entry rejected: invalid requested owner (%d)", (int)requested_owner);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_net_mutex, portMAX_DELAY);

    argus_net_snapshot_t net_pre;
    argus_authority_snapshot_t auth_pre;
    argus_net_event_t evaluated_evt = {0};
    populate_net_snapshot_locked(&net_pre);
    esp_err_t auth_err = argus_authority_mgr_get_snapshot(&auth_pre);
    if (auth_err != ESP_OK) {
        ESP_LOGE(TAG, "Service entry rejected: authority snapshot failed: %s",
                 esp_err_to_name(auth_err));
        xSemaphoreGive(s_net_mutex);
        return auth_err;
    }
    argus_svc_policy_result_t policy = argus_service_policy_evaluate_entry_for_owner(
        &net_pre, &auth_pre, requested_owner, &evaluated_evt);
    if (policy != ARGUS_SVC_POLICY_OK ||
        (expected_preflight && !argus_service_entry_fingerprint_matches(
            expected_preflight, &evaluated_evt.service_preflight))) {
        ESP_LOGW(TAG, "Service entry rejected before mutation: policy=%d, preflight_changed=%d",
                 (int)policy,
                 expected_preflight && policy == ARGUS_SVC_POLICY_OK);
        xSemaphoreGive(s_net_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    argus_cmd_router_lock_dispatch();

    argus_net_snapshot_t net_locked;
    argus_authority_snapshot_t auth_locked;
    argus_net_event_t locked_evt = {0};
    populate_net_snapshot_locked(&net_locked);
    auth_err = argus_authority_mgr_get_snapshot(&auth_locked);
    if (auth_err != ESP_OK) {
        ESP_LOGE(TAG, "Service entry rejected after dispatch lock: authority snapshot failed: %s",
                 esp_err_to_name(auth_err));
        argus_cmd_router_unlock_dispatch();
        xSemaphoreGive(s_net_mutex);
        return auth_err;
    }
    policy = argus_service_policy_evaluate_entry_for_owner(
        &net_locked, &auth_locked, requested_owner, &locked_evt);
    argus_service_recovery_cancel_ops_t cancel_ops = {
        .transaction = &s_wifi_transaction,
        .timer_generation = &s_timer_generation,
        .active_transaction_generation = &s_active_transaction_generation,
        .auto_retry_timer_generation = &s_auto_retry_timer_generation,
        .ip_timeout_timer_generation = &s_ip_timeout_timer_generation,
        .stop_retry_timer = service_stop_retry_timer,
        .stop_ip_timeout_timer = service_stop_ip_timeout_timer,
        .ctx = NULL
    };
    argus_service_commit_ops_t commit_ops = {
        .cancel_recovery = service_commit_cancel_recovery,
        .ctx = &cancel_ops
    };
    argus_service_cancel_failure_t cancel_failure =
        ARGUS_SERVICE_CANCEL_FAILURE_NONE;
    esp_err_t commit_err = argus_net_service_commit_recovery(
        policy, expected_preflight, &locked_evt.service_preflight,
        &commit_ops, &cancel_failure);
    if (commit_err != ESP_OK) {
        if (cancel_failure == ARGUS_SERVICE_CANCEL_FAILURE_NONE) {
            ESP_LOGW(TAG, "Service entry state changed before mutation; request rejected");
            argus_cmd_router_unlock_dispatch();
            xSemaphoreGive(s_net_mutex);
            return commit_err;
        }
        argus_service_cancel_state_t failure_state = {
            .sta_state = s_sta_state,
            .net_error = s_last_error,
            .cancel_failure = s_last_service_cancel_failure,
            .cancel_error = s_last_service_cancel_error
        };
        argus_net_record_service_cancel_failure(
            &failure_state, cancel_failure, commit_err);
        s_sta_state = failure_state.sta_state;
        s_last_error = failure_state.net_error;
        s_last_service_cancel_failure = failure_state.cancel_failure;
        s_last_service_cancel_error = failure_state.cancel_error;
        ESP_LOGE(TAG, "Service entry recovery cancellation failed at %s: %s",
                 cancel_failure == ARGUS_SERVICE_CANCEL_FAILURE_RETRY_TIMER
                     ? "auto-retry timer stop" : "IP-timeout timer stop",
                 esp_err_to_name(commit_err));
        argus_cmd_router_unlock_dispatch();
        xSemaphoreGive(s_net_mutex);
        return commit_err;
    }
    s_last_service_cancel_failure = ARGUS_SERVICE_CANCEL_FAILURE_NONE;
    s_last_service_cancel_error = ESP_OK;

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
        .set_sta_disabled = prod_set_sta_disabled,
        .verify_machine_safe = prod_verify_machine_safe,
        .stop_http = prod_stop_http,
        .start_http = prod_start_http,
        .unlock_net = prod_unlock_net,
        .lock_net = prod_lock_net,
        .lock_dispatch = prod_lock_dispatch,
        .unlock_dispatch = prod_unlock_dispatch,
        .revalidate_network = prod_revalidate_network,
        .ctx = NULL
    };

    esp_err_t res = argus_net_mgr_orchestrate_service_entry(&s_net_mode, requested_owner, &auth_ops, &prod_ops);

    if (res != ESP_OK) {
        s_last_error = ARGUS_NET_ERR_STA_SHUTDOWN_FAILED;
    } else {
        ESP_LOGI(TAG, "Coordinated service entry complete. Mode: SERVICE_AP_ONLY, Authority: LOCAL_SERVICE/%s",
                 argus_authority_mgr_get_owner_name(requested_owner));
    }

    // orchestrator returns with net_mutex unlocked and dispatch unlocked!
    return res;
}

esp_err_t argus_net_mgr_request_service(argus_authority_owner_t requested_owner)
{
    return argus_net_mgr_request_service_internal(requested_owner, NULL);
}


esp_err_t argus_net_mgr_post_event(const argus_net_event_t *evt)
{
    if (!evt || !s_event_queue) return ESP_ERR_INVALID_ARG;
    esp_err_t post_status = argus_net_event_post_status(
        xQueueSend(s_event_queue, evt, 0) == pdTRUE);
    if (post_status != ESP_OK) {
        s_last_error = ARGUS_NET_ERR_QUEUE_OVERFLOW;
        return post_status;
    }
    return ESP_OK;
}

esp_err_t argus_net_mgr_get_snapshot(argus_net_snapshot_t *out_snap)
{
    if (!out_snap) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
    populate_net_snapshot_locked(out_snap);
    xSemaphoreGive(s_net_mutex);

    return ESP_OK;
}

esp_err_t argus_net_mgr_evaluate_service_entry(
    argus_authority_owner_t requested_owner,
    argus_net_snapshot_t *out_net,
    argus_authority_snapshot_t *out_auth,
    argus_net_event_t *out_evt,
    argus_svc_policy_result_t *out_policy)
{
    if (!out_net || !out_auth || !out_evt || !out_policy) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_net_mutex, portMAX_DELAY);
    populate_net_snapshot_locked(out_net);
    esp_err_t auth_err = argus_authority_mgr_get_snapshot(out_auth);
    if (auth_err == ESP_OK) {
        *out_policy = argus_service_policy_evaluate_entry_for_owner(
            out_net, out_auth, requested_owner, out_evt);
    }
    xSemaphoreGive(s_net_mutex);
    return auth_err;
}

esp_err_t argus_net_mgr_request_restart(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    /* Pre-flight machine safety check — reject if motion is active */
    argus_state_snapshot_t state_snap;
    argus_state_mgr_get_snapshot(&state_snap);

    if (state_snap.estop_latched ||
        state_snap.machine_state == ARGUS_STATE_EMERGENCY_STOPPED ||
        state_snap.machine_state == ARGUS_STATE_FAULTED) {
        ESP_LOGE(TAG, "Restart rejected: machine in fault/E-stop state (state=%s, estop=%d)",
                 argus_state_mgr_get_state_name(state_snap.machine_state),
                 (int)state_snap.estop_latched);
        return ESP_ERR_INVALID_STATE;
    }

    if (state_snap.machine_state != ARGUS_STATE_HOLDING &&
        state_snap.machine_state != ARGUS_STATE_UNLOCKED) {
        ESP_LOGE(TAG, "Restart rejected: motion may be active (state=%s)",
                 argus_state_mgr_get_state_name(state_snap.machine_state));
        return ESP_ERR_INVALID_STATE;
    }

    /* Post restart event to net_mgr task — deferred so HTTP can respond first */
    argus_net_event_t evt = { .type = ARGUS_NET_EVT_RESTART_REQUEST };
    return argus_net_mgr_post_event(&evt);
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

argus_sta_state_t argus_net_mgr_get_sta_state(void) { return s_sta_state; }

const char *argus_net_mgr_get_sta_state_name(argus_sta_state_t state)
{
    switch(state) {
        case ARGUS_STA_DISABLED: return "DISABLED";
        case ARGUS_STA_IDLE: return "IDLE";
        case ARGUS_STA_CONNECTING: return "CONNECTING";
        case ARGUS_STA_ASSOCIATED_WAITING_IP: return "ASSOCIATED_WAITING_IP";
        case ARGUS_STA_CONNECTED: return "CONNECTED";
        case ARGUS_STA_RETRY_WAIT: return "RETRY_WAIT";
        case ARGUS_STA_ACTION_REQUIRED: return "ACTION_REQUIRED";
        default: return "UNKNOWN";
    }
}

uint8_t argus_net_mgr_get_last_disconnect_reason(void) { return s_last_disconnect_reason; }

const char *argus_net_mgr_get_last_disconnect_reason_name(void)
{
    if (s_last_disconnect_reason == 0 && s_last_disconnect_category == ARGUS_DISCONNECT_CAT_IP_TIMEOUT) {
        return "IP_ACQUISITION_TIMEOUT";
    }
    const char *name;
    argus_net_classify_disconnect(s_last_disconnect_reason, &name);
    return name;
}

const char *argus_net_mgr_get_last_disconnect_category_name(void)
{
    switch(s_last_disconnect_category) {
        case ARGUS_DISCONNECT_CAT_NONE: return "NONE";
        case ARGUS_DISCONNECT_CAT_AUTHENTICATION: return "AUTHENTICATION";
        case ARGUS_DISCONNECT_CAT_AP_UNAVAILABLE: return "AP_UNAVAILABLE";
        case ARGUS_DISCONNECT_CAT_IP_TIMEOUT: return "IP_TIMEOUT";
        case ARGUS_DISCONNECT_CAT_UNKNOWN: return "UNKNOWN";
        default: return "UNKNOWN";
    }
}

uint32_t argus_net_mgr_get_consecutive_failures(void) { return s_consecutive_failures; }

uint32_t argus_net_mgr_get_retry_seconds(void)
{
    bool active = s_auto_retry_timer && xTimerIsTimerActive(s_auto_retry_timer);
    uint32_t expiry = active ? (uint32_t)xTimerGetExpiryTime(s_auto_retry_timer) : 0;
    uint32_t remaining_ms = argus_net_retry_remaining_ms(
        (uint32_t)xTaskGetTickCount(), expiry, active,
        atomic_load(&s_auto_retry_timer_generation), s_timer_generation,
        s_sta_state, portTICK_PERIOD_MS);
    return argus_net_retry_countdown_seconds(remaining_ms);
}

const char *argus_net_mgr_get_wifi_apply_state_name(argus_wifi_apply_state_t state)
{
    switch (state) {
        case ARGUS_WIFI_APPLY_IDLE: return "IDLE";
        case ARGUS_WIFI_APPLY_PREPARING: return "PREPARING";
        case ARGUS_WIFI_APPLY_WAITING_DISCONNECT: return "WAITING_DISCONNECT";
        case ARGUS_WIFI_APPLY_APPLYING_CONFIG: return "APPLYING_CONFIG";
        case ARGUS_WIFI_APPLY_CONNECTING: return "CONNECTING";
        case ARGUS_WIFI_APPLY_COMPLETE: return "COMPLETE";
        case ARGUS_WIFI_APPLY_FAILED: return "FAILED";
        case ARGUS_WIFI_APPLY_CANCELLED: return "CANCELLED";
        default: return "UNKNOWN";
    }
}

bool argus_net_mgr_is_action_required(void) { return s_sta_state == ARGUS_STA_ACTION_REQUIRED; }

const char *argus_net_mgr_get_operator_guidance(void)
{
    if (s_last_service_cancel_failure != ARGUS_SERVICE_CANCEL_FAILURE_NONE) {
        return argus_net_service_cancel_guidance(s_last_service_cancel_failure);
    }
    if (s_wifi_transaction.active) {
        if (s_wifi_transaction.kind == ARGUS_WIFI_TXN_APPLY_CONFIG) {
            return "Applying saved Wi-Fi configuration; previous failure remains visible until recovery";
        }
        return "Manual Wi-Fi recovery in progress; previous failure remains visible until recovery";
    }
    if (s_sta_state == ARGUS_STA_ACTION_REQUIRED) {
        if (s_last_disconnect_category == ARGUS_DISCONNECT_CAT_AUTHENTICATION) {
            return "Check Wi-Fi SSID/password, then Save or Reconnect";
        }
        return "Manual intervention required. Check Wi-Fi configuration.";
    } else if (s_sta_state == ARGUS_STA_RETRY_WAIT) {
        return "Automatic retry scheduled";
    }
    return "Normal operation";
}
