#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "argus_machine_service.h"
#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
#include "argus_mqtt_invalidation.h"
#endif

#define ARGUS_MQTT_BROKER_CLIENT_ID_CAP 33U
#define ARGUS_MQTT_BROKER_TOPIC_CAP 160U
#define ARGUS_MQTT_BROKER_PAYLOAD_CAP 385U
/* Retained-value store, sized from the CONTRACT rather than a round number.
 *
 * At 32 it was already only one slot clear of the 31 retained topics the
 * runtime publishes, and the two admission-condition topics added in this
 * pass pushed it over: the broker correctly refused to evict authoritative
 * state and the network_fault_action and auth_throttle values simply never
 * became retained, so a client subscribing later would not have learned about
 * a live fault. Found on hardware under load, not in review.
 *
 * ARGUS_MQTT_RETAINED_TOPICS_REQUIRED counts what the contract publishes with
 * retain=true; a static assertion in argus_mqtt_contract.c ties the two
 * together, and the live occupancy is reported by
 * argus_mqtt_broker_get_capacity() so a future overflow is visible rather
 * than silent. */
#define ARGUS_MQTT_BROKER_RETAINED_CAPACITY 40U

/* Admission budgets, declared here so the suite can assert the relationships
 * between them rather than restating the numbers. The authoritative
 * definitions and the measurement they derive from live in
 * argus_mqtt_broker.c; these mirror them and are checked against the
 * implementation by a compile-time assertion there. */
#define ARGUS_MQTT_BROKER_MAX_CLIENTS_DECLARED 3U
#define ARGUS_MQTT_BROKER_MAX_PRECONNECT_DECLARED 3U
#define ARGUS_MQTT_BROKER_MAX_PRECONNECT_UNPROVEN_DECLARED 2U
#define ARGUS_MQTT_BROKER_MAX_PRECONNECT_PER_SOURCE_DECLARED 1U
#define ARGUS_MQTT_BROKER_MAX_CONNECTIONS_DECLARED \
    (ARGUS_MQTT_BROKER_MAX_CLIENTS_DECLARED + \
     ARGUS_MQTT_BROKER_MAX_PRECONNECT_DECLARED)
#define ARGUS_MQTT_BROKER_USERNAME_CAP (ARGUS_SECURITY_ID_MAX + 1U)
#define ARGUS_MQTT_BROKER_PASSWORD_CAP 129U

typedef struct {
    char client_id[ARGUS_MQTT_BROKER_CLIENT_ID_CAP];
    uint8_t username[ARGUS_MQTT_BROKER_USERNAME_CAP];
    size_t username_len;
    uint8_t password[ARGUS_MQTT_BROKER_PASSWORD_CAP];
    size_t password_len;
    uint16_t keep_alive_s;
} argus_mqtt_connect_request_t;

typedef enum {
    ARGUS_MQTT_CONNECT_PARSE_OK = 0,
    ARGUS_MQTT_CONNECT_PARSE_MALFORMED,
    ARGUS_MQTT_CONNECT_PARSE_PROTOCOL,
    ARGUS_MQTT_CONNECT_PARSE_FLAGS,
    ARGUS_MQTT_CONNECT_PARSE_CLIENT_ID,
    ARGUS_MQTT_CONNECT_PARSE_CREDENTIALS,
    ARGUS_MQTT_CONNECT_PARSE_TOO_LARGE,
} argus_mqtt_connect_parse_result_t;

argus_mqtt_connect_parse_result_t argus_mqtt_broker_parse_connect(
    const uint8_t *packet, size_t length,
    argus_mqtt_connect_request_t *out);

typedef struct {
    uint64_t connection_id;
    char client_id[ARGUS_MQTT_BROKER_CLIENT_ID_CAP];
    char topic[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char payload[ARGUS_MQTT_BROKER_PAYLOAD_CAP];
    size_t payload_len;
    uint8_t qos;
    bool retain;
    bool dup;
    bool policy_admitted;
    uint8_t receiving_interface;
    argus_machine_principal_t principal;
} argus_mqtt_broker_message_t;

typedef enum {
    ARGUS_MQTT_BROKER_CLIENT_CONNECTED = 0,
    ARGUS_MQTT_BROKER_CLIENT_DISCONNECTED,
} argus_mqtt_broker_client_event_t;

typedef struct {
    uint64_t connection_id;
    char client_id[ARGUS_MQTT_BROKER_CLIENT_ID_CAP];
    uint8_t receiving_interface;
    argus_machine_principal_t principal;
} argus_mqtt_broker_client_info_t;

typedef void (*argus_mqtt_broker_message_cb_t)(
    const argus_mqtt_broker_message_t *message, void *user_ctx);
typedef esp_err_t (*argus_mqtt_broker_policy_cb_t)(
    const argus_mqtt_broker_message_t *message, void *user_ctx);
typedef void (*argus_mqtt_broker_client_cb_t)(
    argus_mqtt_broker_client_event_t event,
    const argus_mqtt_broker_client_info_t *client,
    void *user_ctx);
typedef argus_machine_auth_outcome_t (*argus_mqtt_broker_auth_cb_t)(
    uint32_t peer_key, const argus_mqtt_connect_request_t *request,
    uint8_t receiving_interface, void *user_ctx);
typedef esp_err_t (*argus_mqtt_broker_revalidate_cb_t)(
    const argus_mqtt_broker_client_info_t *client, void *user_ctx);
typedef esp_err_t (*argus_mqtt_broker_subscribe_policy_cb_t)(
    const argus_mqtt_broker_client_info_t *client,
    const char *filter, void *user_ctx);

typedef struct {
    uint16_t port;
    argus_mqtt_broker_message_cb_t on_message;
    argus_mqtt_broker_policy_cb_t publish_authorize;
    argus_mqtt_broker_policy_cb_t policy_check;
    argus_mqtt_broker_auth_cb_t authenticate;
    argus_mqtt_broker_revalidate_cb_t revalidate;
    argus_mqtt_broker_subscribe_policy_cb_t subscribe_policy;
    argus_mqtt_broker_client_cb_t on_client_event;
    void *user_ctx;
} argus_mqtt_broker_config_t;

esp_err_t argus_mqtt_broker_init(void);
esp_err_t argus_mqtt_broker_start(const argus_mqtt_broker_config_t *config);
esp_err_t argus_mqtt_broker_stop(void);
esp_err_t argus_mqtt_broker_publish(const char *topic, const char *payload, bool retain);

/**
 * @brief Live admission occupancy and the physical costs behind the limits.
 *
 * Exists so the declared client budget can be DEMONSTRATED rather than
 * derived. Pending and authenticated occupancy are reported separately
 * because they are separate resources; the refusal counters say which bound
 * actually fired; the stack low-water is the measurement the per-connection
 * heap cost is dominated by. Read-only and safe in both builds.
 */
typedef struct {
    uint16_t max_connections;
    uint16_t max_authenticated;
    uint16_t max_pending;
    uint16_t max_pending_unproven;
    uint16_t max_pending_per_source;
    uint16_t pending;
    uint16_t authenticated;
    uint16_t peak_pending;
    uint16_t peak_authenticated;
    uint32_t refused_pending_pool;
    uint32_t refused_pending_source;
    uint32_t refused_pending_reserved;
    uint32_t refused_session_pool;
    uint32_t client_task_stack_bytes;
    uint32_t client_stack_min_free_bytes;
    uint32_t connection_record_bytes;
    uint32_t session_record_bytes;
    uint32_t broker_static_bytes;
    uint16_t retained_used;
    uint16_t retained_capacity;
} argus_mqtt_broker_capacity_t;

esp_err_t argus_mqtt_broker_get_capacity(argus_mqtt_broker_capacity_t *out);

/**
 * @brief Read back a retained value, so that "it is published" is checkable.
 *
 * A condition that is computed correctly but never reaches the retained store
 * is not published, and that failure is silent - it happened in this pass.
 * Read-only.
 *
 * @return ESP_OK, or ESP_ERR_NOT_FOUND when nothing is retained on @p topic.
 */
esp_err_t argus_mqtt_broker_get_retained(
    const char *topic, char *out, size_t out_size);

/**
 * @brief The pre-connect admission decision itself, as a pure function.
 *
 * Production calls exactly this after counting occupancy, so a test that
 * drives it is testing the shipped rule rather than a transcription of it.
 * Kept out of the diagnostic gate because it is the rule, not a fixture.
 */
typedef enum {
    ARGUS_MQTT_PRECONNECT_ADMIT = 0,
    ARGUS_MQTT_PRECONNECT_REFUSE_POOL,
    ARGUS_MQTT_PRECONNECT_REFUSE_RESERVED,
    ARGUS_MQTT_PRECONNECT_REFUSE_SOURCE,
} argus_mqtt_preconnect_decision_t;

argus_mqtt_preconnect_decision_t argus_mqtt_broker_preconnect_decide(
    size_t pending, size_t pending_from_source, bool source_proven);
esp_err_t argus_mqtt_broker_fence_machine_authentication(
    const char *identifier);
esp_err_t argus_mqtt_broker_disconnect_machine(const char *identifier);
bool argus_mqtt_broker_is_running(void);

#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
typedef struct {
    int (*shutdown_socket)(int socket_fd, int how, void *ctx);
    void (*after_claim)(
        int selected_socket, bool close_allowed, void *ctx);
    void (*after_release)(
        int selected_socket, bool close_allowed, void *ctx);
    void *ctx;
} argus_mqtt_broker_test_socket_ops_t;

esp_err_t argus_mqtt_broker_test_disconnect_claim(
    int selected_socket,
    const argus_mqtt_broker_test_socket_ops_t *ops);
bool argus_mqtt_broker_test_bind_allowed(
    const argus_mqtt_invalidation_journal_t *invalidations,
    uint64_t captured_generation, const char *identifier,
    bool in_use, bool connected, bool security_invalidated,
    uint64_t connection_id, uint64_t expected_connection_id,
    bool duplicate_client_id);
bool argus_mqtt_broker_test_packet_admitted(
    bool in_use, bool connected, bool security_invalidated,
    uint64_t connection_id, uint64_t expected_connection_id);
bool argus_mqtt_broker_test_disconnect_matches(
    bool in_use, bool connected, int socket_fd,
    const char *principal_identifier, const char *target_identifier);
#endif

typedef enum {
    BROKER_STATE_STOPPED = 0,
    BROKER_STATE_STARTING,
    BROKER_STATE_RUNNING,
    BROKER_STATE_STOPPING
} argus_broker_state_t;

typedef struct {
    argus_broker_state_t state;
    int32_t active_client_count;
    bool has_server_task;
    bool has_listener;
    bool running;
    bool stopped;
} argus_mqtt_broker_lifecycle_obs_t;

typedef esp_err_t (*argus_mqtt_broker_observe_fn_t)(
    void *ctx, argus_mqtt_broker_lifecycle_obs_t *out);
typedef esp_err_t (*argus_mqtt_broker_stop_fn_t)(void *ctx);
typedef void (*argus_mqtt_broker_wait_fn_t)(void *ctx, uint32_t delay_ms);

typedef struct {
    argus_mqtt_broker_observe_fn_t observe;
    argus_mqtt_broker_stop_fn_t stop;
    argus_mqtt_broker_wait_fn_t wait;
    void *ctx;
} argus_mqtt_broker_convergence_ops_t;

bool argus_mqtt_broker_observation_is_stopped(
    const argus_mqtt_broker_lifecycle_obs_t *obs);
bool argus_mqtt_broker_observation_is_running(
    const argus_mqtt_broker_lifecycle_obs_t *obs);
esp_err_t argus_mqtt_broker_get_lifecycle_obs(argus_mqtt_broker_lifecycle_obs_t *out);
esp_err_t argus_mqtt_broker_request_stop_converged(
    const argus_mqtt_broker_convergence_ops_t *ops);
esp_err_t argus_mqtt_broker_verify_stopped_converged(
    const argus_mqtt_broker_convergence_ops_t *ops,
    uint32_t observation_attempts,
    uint32_t delay_ms);
