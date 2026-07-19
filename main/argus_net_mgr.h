/**
 * @file argus_net_mgr.h
 * @brief Dedicated Network-Mode & Wi-Fi Lifecycle Manager for Argus Pump Controller V2
 */

#ifndef ARGUS_NET_MGR_H
#define ARGUS_NET_MGR_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "argus_authority_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARGUS_NET_MODE_BOOTSTRAP = 0,
    ARGUS_NET_MODE_UNCOMMISSIONED_AP,
    ARGUS_NET_MODE_COMMISSIONED_STA,
    ARGUS_NET_MODE_AP_DISCOVERABLE,
    ARGUS_NET_MODE_SERVICE_TRANSITION,
    ARGUS_NET_MODE_SERVICE_AP_ONLY,
    ARGUS_NET_MODE_NETWORK_FAULT
} argus_network_mode_t;

typedef enum {
    ARGUS_NET_ERR_NONE = 0,
    ARGUS_NET_ERR_NVS_CORRUPT,
    ARGUS_NET_ERR_SLOT_CRC_FAILED,
    ARGUS_NET_ERR_UNSUPPORTED_SCHEMA,
    ARGUS_NET_ERR_COMMIT_FAILED,
    ARGUS_NET_ERR_READBACK_FAILED,
    ARGUS_NET_ERR_STA_CONNECT_TIMEOUT,
    ARGUS_NET_ERR_AP_START_FAILED,
    ARGUS_NET_ERR_STOP_TIMEOUT,
    ARGUS_NET_ERR_STA_SHUTDOWN_FAILED,
    ARGUS_NET_ERR_SERVICE_CRED_MISSING,
    ARGUS_NET_ERR_QUEUE_OVERFLOW,
    ARGUS_NET_ERR_AUTHORITY_VIOLATION,
    ARGUS_NET_ERR_OWNER_VIOLATION,
    ARGUS_NET_ERR_RESET_FAILED,
    ARGUS_NET_ERR_REBOOT_PREP_FAILED
} argus_net_err_t;

typedef enum {
    ARGUS_NET_EVT_SERVICE_REQUEST = 0,
    ARGUS_NET_EVT_SERVICE_EXIT,
    ARGUS_NET_EVT_STA_CONNECTED,
    ARGUS_NET_EVT_STA_DISCONNECTED,
    ARGUS_NET_EVT_AP_CLIENT_CONNECTED,
    ARGUS_NET_EVT_AP_CLIENT_DISCONNECTED,
    ARGUS_NET_EVT_RESTART_REQUEST          /**< Coordinated restart (deferred to net_mgr task) */
} argus_net_event_type_t;

typedef struct {
    argus_net_event_type_t type;
    argus_authority_owner_t requested_owner;
} argus_net_event_t;

/**
 * @brief Initialize network-mode manager task and Wi-Fi event handlers.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if build service credential missing.
 */
esp_err_t argus_net_mgr_init(void);

/**
 * @brief Get current active network mode.
 */
argus_network_mode_t argus_net_mgr_get_mode(void);

/**
 * @brief Post a network event to the network manager task queue.
 * @param evt Network event payload.
 * @return ESP_OK if queued, ESP_ERR_NO_MEM if queue is full.
 */
esp_err_t argus_net_mgr_post_event(const argus_net_event_t *evt);

/**
 * @brief Get last network error code.
 */
argus_net_err_t argus_net_mgr_get_last_error(void);

/**
 * @brief Get human-readable string name for network mode.
 */
const char *argus_net_mgr_get_mode_name(argus_network_mode_t mode);

/**
 * @brief Enable service AP discoverability alongside active STA (APSTA mode).
 */
esp_err_t argus_net_mgr_enable_ap_discoverable(void);

typedef struct {
    esp_err_t (*request_normal_stop)(void *ctx);
    esp_err_t (*verify_stopped)(void *ctx);
    esp_err_t (*stop_broker)(void *ctx);
    esp_err_t (*verify_broker_stopped)(void *ctx);
    esp_err_t (*disconnect_sta)(void *ctx);
    esp_err_t (*verify_sta_disconnected)(void *ctx);
    esp_err_t (*verify_sta_ip_released)(void *ctx);
    esp_err_t (*set_wifi_ap_only)(void *ctx);
    esp_err_t (*verify_ap_active)(void *ctx);
    esp_err_t (*verify_machine_safe)(void *ctx);  /**< Final pre-grant machine-state/E-stop check */
    void *ctx;
} argus_service_transition_ops_t;

/**
 * @brief Pure service transition orchestration function operating on caller-provided authority ops and network ops struct.
 */
esp_err_t argus_net_mgr_orchestrate_service_entry(argus_network_mode_t *net_mode,
                                                   argus_authority_owner_t requested_owner,
                                                   const argus_service_authority_ops_t *auth_ops,
                                                   const argus_service_transition_ops_t *ops);

/**
 * @brief Synchronously execute coordinated service-entry transition with physical network isolation.
 * @param requested_owner Target local owner (ARGUS_AUTH_OWNER_BROWSER or ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI).
 * @return ESP_OK on verified success, error code on failure.
 */
esp_err_t argus_net_mgr_request_service(argus_authority_owner_t requested_owner);

/**
 * @brief Request coordinated service-exit transition with motion stop, network restore, and reboot.
 * @param requested_owner Current owner requesting exit.
 * @return ESP_OK if reboot initiated, error code on failure (reboot does not occur on error).
 */
esp_err_t argus_net_mgr_request_service_exit(argus_authority_owner_t requested_owner);

/**
 * @brief Request a coordinated restart via the net_mgr event queue.
 *
 * Checks machine state before accepting. Rejects if motion is active
 * (machine not in HOLDING/UNLOCKED) or E-stop is latched.
 *
 * The actual restart is deferred to the net_mgr task so that the calling
 * context (e.g. HTTP handler) can transmit its response before shutdown.
 *
 * @return ESP_OK if restart accepted and queued,
 *         ESP_ERR_INVALID_STATE if motion active or machine unsafe,
 *         ESP_ERR_NO_MEM if event queue is full.
 */
esp_err_t argus_net_mgr_request_restart(void);

typedef void (*argus_net_mgr_mqtt_broker_start_fn_t)(void);
void argus_net_mgr_register_broker_start_cb(argus_net_mgr_mqtt_broker_start_fn_t cb);

/**
 * @brief Race-free network observation assembled from mutex-protected mode/error
 *        state and atomic lifecycle flags.
 */
typedef struct {
    argus_network_mode_t mode;
    argus_net_err_t last_error;
    bool sta_connected;
    bool sta_ip_acquired;
    bool ap_started;
    bool mqtt_broker_running;
} argus_net_snapshot_t;

esp_err_t argus_net_mgr_get_snapshot(argus_net_snapshot_t *out_snap);

bool argus_net_mgr_is_sta_started(void);
bool argus_net_mgr_is_sta_connected(void);
bool argus_net_mgr_is_sta_ip_acquired(void);
bool argus_net_mgr_is_ap_started(void);
const char *argus_net_mgr_get_wifi_driver_mode_name(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGUS_NET_MGR_H */
