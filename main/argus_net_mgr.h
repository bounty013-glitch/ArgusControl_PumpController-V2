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
    ARGUS_NET_EVT_AP_CLIENT_DISCONNECTED
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

#ifdef __cplusplus
}
#endif

#endif /* ARGUS_NET_MGR_H */
