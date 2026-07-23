/**
 * @file argus_net_mgr.h
 * @brief Dedicated Network-Mode & Wi-Fi Lifecycle Manager for Argus Pump Controller V2
 */

#ifndef ARGUS_NET_MGR_H
#define ARGUS_NET_MGR_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include "esp_err.h"
#include "argus_authority_mgr.h"
#include "esp_wifi.h"

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
    ARGUS_NET_MODE_SECURITY_RECOVERY_TRANSITION,
    ARGUS_NET_MODE_SECURITY_RECOVERY_AP_ONLY,
    ARGUS_NET_MODE_NETWORK_FAULT
} argus_network_mode_t;

typedef enum {
    ARGUS_STA_DISABLED = 0,
    ARGUS_STA_IDLE,
    ARGUS_STA_CONNECTING,
    ARGUS_STA_ASSOCIATED_WAITING_IP,
    ARGUS_STA_CONNECTED,
    ARGUS_STA_RETRY_WAIT,
    ARGUS_STA_ACTION_REQUIRED
} argus_sta_state_t;


typedef enum {
    ARGUS_WIFI_APPLY_IDLE = 0,
    ARGUS_WIFI_APPLY_PREPARING,
    ARGUS_WIFI_APPLY_WAITING_DISCONNECT,
    ARGUS_WIFI_APPLY_APPLYING_CONFIG,
    ARGUS_WIFI_APPLY_CONNECTING,
    ARGUS_WIFI_APPLY_COMPLETE,
    ARGUS_WIFI_APPLY_FAILED,
    ARGUS_WIFI_APPLY_CANCELLED
} argus_wifi_apply_state_t;

typedef enum {
    ARGUS_WIFI_TXN_NONE = 0,
    ARGUS_WIFI_TXN_APPLY_CONFIG,
    ARGUS_WIFI_TXN_MANUAL_RECONNECT
} argus_wifi_transaction_kind_t;

typedef enum {
    ARGUS_DISCONNECT_CAT_NONE = 0,
    ARGUS_DISCONNECT_CAT_AUTHENTICATION,
    ARGUS_DISCONNECT_CAT_AP_UNAVAILABLE,
    ARGUS_DISCONNECT_CAT_IP_TIMEOUT,
    ARGUS_DISCONNECT_CAT_UNKNOWN
} argus_disconnect_category_t;

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
    ARGUS_NET_ERR_REBOOT_PREP_FAILED,
    ARGUS_NET_ERR_TIMER_COMMAND_FAILED,
    ARGUS_NET_ERR_WIFI_TRANSACTION_FAILED,
    ARGUS_NET_ERR_SECURITY_RECOVERY_FAILED
} argus_net_err_t;

typedef enum {
    ARGUS_NET_EVT_SERVICE_REQUEST = 0,
    ARGUS_NET_EVT_SERVICE_EXIT,
    ARGUS_NET_EVT_STA_ASSOCIATED,
    ARGUS_NET_EVT_STA_CONNECTED,
    ARGUS_NET_EVT_STA_DISCONNECTED,
    ARGUS_NET_EVT_AP_CLIENT_CONNECTED,
    ARGUS_NET_EVT_AP_CLIENT_DISCONNECTED,
    ARGUS_NET_EVT_RESTART_REQUEST,         /**< Coordinated restart (deferred to net_mgr task) */
    ARGUS_NET_EVT_FACTORY_RESET_REQUEST,   /**< Coordinated configuration reset + reboot */
    ARGUS_NET_EVT_MANUAL_RECONNECT_REQUEST,/**< Operator requests manual Wi-Fi reconnect */
    ARGUS_NET_EVT_AUTO_RECONNECT_WAKEUP,   /**< Timer wakeup for auto-reconnect */
    ARGUS_NET_EVT_APPLY_WIFI_CONFIG,       /**< Apply new Wi-Fi credentials without restart */
    ARGUS_NET_EVT_STA_STOPPED,             /**< Wi-Fi driver confirms physical STA stop */
    ARGUS_NET_EVT_SECURITY_RECOVERY_REQUEST /**< Physical KEY1 recovery request */
} argus_net_event_type_t;

typedef enum {
    ARGUS_STA_EVENT_ASSOCIATED = 0,
    ARGUS_STA_EVENT_IP_ACQUIRED,
    ARGUS_STA_EVENT_DISCONNECTED,
    ARGUS_STA_EVENT_IP_TIMEOUT,
    ARGUS_STA_EVENT_STOPPED
} argus_sta_lifecycle_event_t;

typedef enum {
    ARGUS_STA_EVENT_IGNORE = 0,
    ARGUS_STA_EVENT_PROCESS,
    ARGUS_STA_EVENT_CONFIRM_DISABLED
} argus_sta_event_action_t;

typedef enum {
    ARGUS_SVC_POLICY_OK = 0,
    ARGUS_SVC_POLICY_IDEMPOTENT,
    ARGUS_SVC_POLICY_REJECT_MODE,
    ARGUS_SVC_POLICY_REJECT_AUTHORITY,
    ARGUS_SVC_POLICY_TRANSITION_IN_PROGRESS
} argus_svc_policy_result_t;

typedef struct {
    argus_network_mode_t mode;
    argus_sta_state_t sta_state;
    argus_wifi_apply_state_t apply_state;
    argus_control_authority_t authority_mode;
    argus_authority_owner_t authority_owner;
    uint32_t authority_generation;
    uint32_t timer_generation;
    uint32_t transaction_generation;
    uint32_t auto_retry_timer_generation;
    uint32_t ip_timeout_timer_generation;
    uint32_t consecutive_failures;
    uint8_t last_disconnect_reason;
    argus_disconnect_category_t last_disconnect_category;
    bool sta_connected;
    bool sta_ip_acquired;
    bool ap_started;
    bool mqtt_broker_running;
    bool mqtt_broker_stopped;
    bool mqtt_broker_observable;
    bool commissioned;
    bool wifi_transaction_active;
    bool auto_retry_timer_active;
    bool ip_timeout_timer_active;
} argus_service_entry_fingerprint_t;

/* Pure helpers for classification and retry decisions */
argus_disconnect_category_t argus_net_classify_disconnect(uint8_t reason, const char **out_name);
argus_sta_state_t argus_net_evaluate_retry(argus_disconnect_category_t cat, uint32_t consecutive_failures);
bool argus_net_can_manual_reconnect(argus_network_mode_t net_mode,
                                    argus_sta_state_t sta_state,
                                    bool has_valid_commissioned_config);
esp_err_t argus_net_event_post_status(bool queued);
bool argus_net_timer_generation_is_current(uint32_t event_generation,
                                           uint32_t active_generation);
esp_err_t argus_net_timer_command_status(bool command_queued);
argus_sta_event_action_t argus_net_decide_sta_event(
    argus_network_mode_t mode,
    argus_sta_lifecycle_event_t event,
    uint32_t event_generation,
    uint32_t active_transaction_generation,
    bool sta_started,
    bool sta_connected,
    bool sta_ip_acquired);
void argus_net_apply_sta_event_action(argus_network_mode_t mode,
                                      argus_sta_lifecycle_event_t event,
                                      argus_sta_event_action_t action,
                                      argus_sta_state_t *sta_state,
                                      bool *sta_started,
                                      bool *sta_connected,
                                      bool *sta_ip_acquired);


typedef struct {
    argus_net_event_type_t type;
    argus_authority_owner_t requested_owner;
    uint8_t disconnect_reason;             /**< Captured from WIFI_EVENT_STA_DISCONNECTED */
    uint32_t timer_generation;
    uint32_t transaction_generation;
    bool service_preflight_required;
    argus_service_entry_fingerprint_t service_preflight;
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

/**
 * @brief Get current STA lifecycle state.
 */
argus_sta_state_t argus_net_mgr_get_sta_state(void);

/**
 * @brief Get human-readable string for STA lifecycle state.
 */
const char *argus_net_mgr_get_sta_state_name(argus_sta_state_t state);
const char *argus_net_mgr_get_wifi_apply_state_name(argus_wifi_apply_state_t state);

/**
 * @brief Get the numeric reason code of the last STA disconnection.
 */
uint8_t argus_net_mgr_get_last_disconnect_reason(void);

/**
 * @brief Get the string name of the last disconnect reason.
 */
const char *argus_net_mgr_get_last_disconnect_reason_name(void);

/**
 * @brief Get the failure category of the last disconnection.
 */
const char *argus_net_mgr_get_last_disconnect_category_name(void);

/**
 * @brief Get the number of consecutive STA connection failures.
 */
uint32_t argus_net_mgr_get_consecutive_failures(void);

/**
 * @brief Get the number of seconds until the next automatic retry.
 */
uint32_t argus_net_mgr_get_retry_seconds(void);

/**
 * @brief Check if operator action is required to resolve Wi-Fi connection issues.
 */
bool argus_net_mgr_is_action_required(void);

/**
 * @brief Get operator guidance message for current Wi-Fi status.
 */
const char *argus_net_mgr_get_operator_guidance(void);


typedef struct {
    esp_err_t (*stop_timers)(void *ctx);
    esp_err_t (*revoke_supervisory)(void *ctx);
    esp_err_t (*stop_broker)(void *ctx);
    esp_err_t (*verify_broker_stopped)(void *ctx);
    esp_err_t (*load_config)(void *ctx, wifi_config_t *out_cfg, bool *has_cfg);
    esp_err_t (*validate_config)(void *ctx, const wifi_config_t *cfg, bool has_cfg);
    esp_err_t (*disconnect_sta)(void *ctx);
    esp_err_t (*apply_sta_config)(void *ctx, const wifi_config_t *cfg);
    esp_err_t (*connect_sta)(void *ctx);
    void *ctx;
} argus_wifi_apply_ops_t;

typedef struct {
    argus_wifi_apply_state_t state;
    argus_wifi_transaction_kind_t kind;
    uint32_t generation;
    bool active;
    bool intentional_disconnect_requested;
    bool config_staged;
    bool authority_revoked;
    bool broker_stopped;
    bool config_applied;
    esp_err_t last_error;
    wifi_config_t pending_config;
} argus_wifi_transaction_t;

void argus_wifi_transaction_init(argus_wifi_transaction_t *txn);
esp_err_t argus_wifi_transaction_begin_apply(argus_wifi_transaction_t *txn,
                                             uint32_t generation,
                                             argus_network_mode_t net_mode,
                                             argus_sta_state_t *sta_state,
                                             bool sta_connected,
                                             const argus_wifi_apply_ops_t *ops);
esp_err_t argus_wifi_transaction_begin_reconnect(argus_wifi_transaction_t *txn,
                                                 uint32_t generation,
                                                 argus_network_mode_t net_mode,
                                                 argus_sta_state_t *sta_state,
                                                 bool sta_connected,
                                                 const argus_wifi_apply_ops_t *ops);
esp_err_t argus_wifi_transaction_handle_disconnect(argus_wifi_transaction_t *txn,
                                                   uint32_t event_generation,
                                                   argus_sta_state_t *sta_state,
                                                   const argus_wifi_apply_ops_t *ops,
                                                   bool *out_handled);
esp_err_t argus_wifi_transaction_handle_got_ip(argus_wifi_transaction_t *txn,
                                               uint32_t event_generation,
                                               bool *out_completed);
esp_err_t argus_wifi_transaction_handle_connection_failure(
    argus_wifi_transaction_t *txn,
    uint32_t event_generation,
    esp_err_t connection_error,
    bool *out_failed);
void argus_wifi_transaction_cancel(argus_wifi_transaction_t *txn);
bool argus_wifi_transaction_event_matches(const argus_wifi_transaction_t *txn,
                                          uint32_t event_generation);

typedef enum {
    ARGUS_SERVICE_CANCEL_FAILURE_NONE = 0,
    ARGUS_SERVICE_CANCEL_FAILURE_RETRY_TIMER,
    ARGUS_SERVICE_CANCEL_FAILURE_IP_TIMER
} argus_service_cancel_failure_t;

typedef struct {
    argus_sta_state_t sta_state;
    argus_net_err_t net_error;
    argus_service_cancel_failure_t cancel_failure;
    esp_err_t cancel_error;
} argus_service_cancel_state_t;

void argus_net_record_service_cancel_failure(
    argus_service_cancel_state_t *state,
    argus_service_cancel_failure_t failure,
    esp_err_t error);
const char *argus_net_service_cancel_guidance(
    argus_service_cancel_failure_t failure);

typedef struct {
    argus_wifi_transaction_t *transaction;
    uint32_t *timer_generation;
    _Atomic uint32_t *active_transaction_generation;
    _Atomic uint32_t *auto_retry_timer_generation;
    _Atomic uint32_t *ip_timeout_timer_generation;
    esp_err_t (*stop_retry_timer)(void *ctx);
    esp_err_t (*stop_ip_timeout_timer)(void *ctx);
    void *ctx;
} argus_service_recovery_cancel_ops_t;

esp_err_t argus_net_cancel_recovery_for_service(
    const argus_service_recovery_cancel_ops_t *ops,
    argus_service_cancel_failure_t *out_failure);

typedef struct {
    esp_err_t (*cancel_recovery)(void *ctx,
                                 argus_service_cancel_failure_t *out_failure);
    void *ctx;
} argus_service_commit_ops_t;

esp_err_t argus_net_service_commit_recovery(
    argus_svc_policy_result_t policy,
    const argus_service_entry_fingerprint_t *expected,
    const argus_service_entry_fingerprint_t *actual,
    const argus_service_commit_ops_t *ops,
    argus_service_cancel_failure_t *out_failure);

/* Compatibility entry point for existing pure tests; production owns a
 * persistent argus_wifi_transaction_t and uses the transaction API above. */
esp_err_t argus_net_mgr_orchestrate_wifi_apply(argus_network_mode_t *net_mode,
                                               argus_sta_state_t *sta_state,
                                               bool sta_connected,
                                               const argus_wifi_apply_ops_t *ops);

typedef struct {
    uint8_t reason;
    argus_disconnect_category_t category;
    uint32_t consecutive_failures;
    uint32_t authentication_streak;
} argus_wifi_failure_evidence_t;

void argus_net_failure_evidence_record(argus_wifi_failure_evidence_t *evidence,
                                       uint8_t reason,
                                       argus_disconnect_category_t category);
void argus_net_failure_evidence_clear(argus_wifi_failure_evidence_t *evidence);
uint32_t argus_net_retry_countdown_seconds(uint32_t remaining_ms);
uint32_t argus_net_retry_remaining_ms(uint32_t current_tick,
                                      uint32_t expiry_tick,
                                      bool timer_active,
                                      uint32_t timer_generation,
                                      uint32_t current_generation,
                                      argus_sta_state_t sta_state,
                                      uint32_t tick_period_ms);

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
    esp_err_t (*set_sta_disabled)(void *ctx);
    esp_err_t (*verify_machine_safe)(void *ctx);  /**< Final pre-grant machine-state/E-stop check */
    
    // Hooks for structural orchestration without singleton pollution
    esp_err_t (*stop_http)(void *ctx);
    esp_err_t (*start_http)(void *ctx);
    esp_err_t (*unlock_net)(void *ctx);
    esp_err_t (*lock_net)(void *ctx);
    esp_err_t (*lock_dispatch)(void *ctx);
    esp_err_t (*unlock_dispatch)(void *ctx);
    esp_err_t (*revalidate_network)(void *ctx); // verify AP active, STA dead

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
esp_err_t argus_net_mgr_request_manual_reconnect(void);

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

/**
 * @brief Preflight and queue one coordinated configuration factory reset.
 * @return ESP_OK when exactly one request was queued; ESP_ERR_INVALID_STATE
 *         when policy rejects; ESP_ERR_NOT_SUPPORTED when already pending.
 */
esp_err_t argus_net_mgr_request_factory_reset(void);

/**
 * @brief Queue physically local AP-only security recovery.
 *
 * This operation changes network lifecycle only. It does not dispatch a
 * command or mutate machine state, target, E-stop/fault, or authority.
 */
esp_err_t argus_net_mgr_request_security_recovery(void);

bool argus_net_security_recovery_request_is_idempotent(
    argus_network_mode_t mode);

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
    bool mqtt_broker_stopped;
    bool mqtt_broker_observable;
    bool commissioned;
    argus_sta_state_t sta_state;
    argus_disconnect_category_t last_disconnect_category;
    uint8_t last_disconnect_reason;
    uint32_t consecutive_failures;
    uint32_t seconds_until_retry;
    bool action_required;
    bool manual_reconnect_permitted;
    char sta_ip_address[16];
    argus_wifi_apply_state_t apply_state;
    uint32_t timer_generation;
    bool wifi_transaction_active;
    bool factory_reset_pending;
    uint32_t transaction_generation;
    bool auto_retry_timer_active;
    uint32_t auto_retry_timer_generation;
    bool ip_timeout_timer_active;
    uint32_t ip_timeout_timer_generation;
    argus_service_cancel_failure_t last_service_cancel_failure;
    esp_err_t last_service_cancel_error;
} argus_net_snapshot_t;

esp_err_t argus_net_mgr_get_snapshot(argus_net_snapshot_t *out_snap);

esp_err_t argus_net_mgr_evaluate_service_entry(
    argus_authority_owner_t requested_owner,
    argus_net_snapshot_t *out_net,
    argus_authority_snapshot_t *out_auth,
    argus_net_event_t *out_evt,
    argus_svc_policy_result_t *out_policy);

bool argus_net_mgr_is_sta_started(void);
bool argus_net_mgr_is_sta_connected(void);
bool argus_net_mgr_is_sta_ip_acquired(void);
bool argus_net_mgr_is_ap_started(void);
const char *argus_net_mgr_get_wifi_driver_mode_name(void);

/**
 * @brief Pure testable helper to evaluate if a STA disconnect driver call is required.
 */
esp_err_t argus_net_mgr_eval_sta_disconnect_req(wifi_mode_t wifi_mode, esp_err_t wifi_mode_err, bool sta_started, bool sta_connected, bool sta_ip_acquired, bool *out_disconnect_needed);

#ifdef __cplusplus
}
#endif

#endif /* ARGUS_NET_MGR_H */
