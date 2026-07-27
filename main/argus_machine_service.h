#ifndef ARGUS_MACHINE_SERVICE_H
#define ARGUS_MACHINE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "argus_authorization.h"
#include "argus_security_store.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_MACHINE_ID_LENGTH 32U
#define ARGUS_MACHINE_SECRET_BYTES 32U
#define ARGUS_MACHINE_SECRET_LENGTH 43U
#define ARGUS_MACHINE_AUTH_BUCKETS 8U
#define ARGUS_MACHINE_AUTH_FAILURE_LIMIT 5U
#define ARGUS_MACHINE_AUTH_WINDOW_US UINT64_C(60000000)
#define ARGUS_MACHINE_AUTH_COOLDOWN_US UINT64_C(30000000)
// Concurrent machine-auth derivations admitted to the KDF worker.
//
// Lowered 2 -> 1. The worker is serialised with a depth-1 queue, so at 2 an
// attacker could keep one derivation RUNNING and a second QUEUED
// continuously, leaving no queue slot for the browser login path that shares
// the worker: a human trying to recover the controller waited the full
// ARGUS_KDF_SUBMIT_WAIT_MS and then failed closed with 503. At 1, a machine
// request is either running or rejected outright, so the queue slot is
// always available to the human/recovery path.
#define ARGUS_MACHINE_AUTH_KDF_ADMISSION_MAX 1U

// GLOBAL rate bound on machine-auth KDF work, deliberately NOT keyed on
// source address.
//
// The per-source buckets below cannot bound total work: there are only
// ARGUS_MACHINE_AUTH_BUCKETS of them, they are evicted LRU, and a BLOCKED
// bucket is the stalest so it is evicted first - an attacker clears its own
// cooldown simply by connecting from a few other addresses. Since every
// CONNECT with a well-formed username reaches the KDF (deliberately, so an
// unknown identity is indistinguishable from a wrong password), that made
// ~2 s of PBKDF2 per TCP connection reachable by an unauthenticated peer,
// and authentication as a whole - MQTT and browser - could be denied
// indefinitely.
//
// This budget is a plain global token bucket: at most _BURST derivations may
// start within any _WINDOW_US, whoever asks. It cannot be bypassed by
// cycling addresses because it does not look at the address at all.
//
// Sizing: a legitimate reconnect storm is small and rare - the HMI, at most
// a couple of service tools, each retrying every few seconds. 8 per 10 s
// covers that with margin while capping an attacker at 8 derivations
// (~16 s of worker time) per 10 s instead of a continuous 100% duty cycle.
// A refused request costs the caller nothing and is retried; the bound
// delays a legitimate reconnect during an active flood, it does not deny it
// indefinitely, and the browser recovery path stays available throughout via
// the reserved queue slot described above.
#define ARGUS_MACHINE_AUTH_KDF_GLOBAL_BURST 8U
#define ARGUS_MACHINE_AUTH_KDF_GLOBAL_WINDOW_US UINT64_C(10000000)

// PROVEN-SOURCE RESERVATION - and precisely what it is not.
//
// The bucket above bounds total work but says nothing about WHO gets it. An
// attacker that keeps the bucket empty starves every other caller equally,
// including the rotary HMI trying to reconnect. Review asked whether bounded,
// technically sound admission fairness is possible inside the existing
// protocol. It is - but only weakly, and the weakness must be stated rather
// than dressed up.
//
// What is available before credential verification: the source address, the
// receiving interface, and history. Everything in MQTT CONNECT itself -
// client id, username - is attacker-chosen text, so reserving capacity for
// "the HMI's identifier" reserves it for whoever types that identifier.
// Source address is not authenticated either, but it is not free: the peer
// must complete a TCP handshake from it, so it must actually receive at that
// address on this L2 segment.
//
// So: an address that has completed a SUCCESSFUL machine authentication
// within _PROVEN_TTL_US is "proven", and proven sources keep a reserved share
// of both the KDF budget and the pre-connect socket pool. Entries can only be
// created by a successful authentication, and the table is separate from the
// LRU failure buckets precisely so that an attacker cycling addresses cannot
// evict one.
//
// SUPPORTED GUARANTEE: a flood from addresses that have never authenticated
// cannot consume the whole KDF budget or the whole pre-connect pool; a
// recently authenticated client retains a share and reconnects with bounded
// delay.
// NOT GUARANTEED, and not claimed anywhere: anything against a flood that
// originates from - or successfully spoofs - a proven address; anything
// before the first successful authentication after boot, when the table is
// empty and the HMI competes as an unproven source; any identification of a
// legitimate client. Distinguishing one before credential verification needs
// a pre-authentication challenge (TLS-PSK, or an HMAC cookie carried in
// CONNECT). That is a wire-protocol change and is recorded as future work,
// not attempted here.
#define ARGUS_MACHINE_AUTH_PROVEN_SOURCES 2U
#define ARGUS_MACHINE_AUTH_PROVEN_TTL_US UINT64_C(600000000)
// Of _GLOBAL_BURST, the most that unproven sources may spend per window. The
// remainder is reachable only by proven sources.
#define ARGUS_MACHINE_AUTH_KDF_UNPROVEN_BURST 5U

typedef struct {
    char display_name[ARGUS_SECURITY_DISPLAY_MAX + 1U];
    argus_machine_client_type_t client_type;
    uint8_t allowed_transports;
    uint8_t allowed_interfaces;
    char scope[ARGUS_SECURITY_SCOPE_MAX + 1U];
    char topic_scope[ARGUS_SECURITY_TOPIC_SCOPE_MAX + 1U];
    char api_scope[ARGUS_SECURITY_API_SCOPE_MAX + 1U];
    argus_permission_set_t permissions;
} argus_machine_enrollment_request_t;

typedef struct {
    char identifier[ARGUS_SECURITY_ID_MAX + 1U];
    char secret[ARGUS_MACHINE_SECRET_LENGTH + 1U];
    uint32_t credential_version;
    uint32_t principal_revision;
} argus_machine_credential_once_t;

typedef struct {
    char identifier[ARGUS_SECURITY_ID_MAX + 1U];
    char scope[ARGUS_SECURITY_SCOPE_MAX + 1U];
    char topic_scope[ARGUS_SECURITY_TOPIC_SCOPE_MAX + 1U];
    char api_scope[ARGUS_SECURITY_API_SCOPE_MAX + 1U];
    argus_permission_set_t permissions;
    uint8_t allowed_transports;
    uint8_t allowed_interfaces;
    uint8_t client_type;
    uint32_t credential_version;
    uint32_t principal_revision;
    uint32_t record_security_epoch;
    uint32_t directory_generation;
} argus_machine_principal_t;

typedef enum {
    ARGUS_MACHINE_AUTH_SUCCESS = 0,
    ARGUS_MACHINE_AUTH_INVALID_CREDENTIALS,
    ARGUS_MACHINE_AUTH_THROTTLED,
    ARGUS_MACHINE_AUTH_BUSY,
    ARGUS_MACHINE_AUTH_DIRECTORY_UNAVAILABLE,
    ARGUS_MACHINE_AUTH_TRANSPORT_REJECTED,
    ARGUS_MACHINE_AUTH_INTERFACE_REJECTED,
    ARGUS_MACHINE_AUTH_CLIENT_ID_REJECTED,
    ARGUS_MACHINE_AUTH_INVALID_REQUEST,
} argus_machine_auth_result_t;

typedef struct {
    argus_machine_auth_result_t result;
    argus_machine_principal_t principal;
    bool kdf_invoked;
    uint32_t retry_after_s;
} argus_machine_auth_outcome_t;

esp_err_t argus_machine_service_init(void);

esp_err_t argus_machine_service_enroll(
    const argus_principal_t *actor,
    const argus_machine_enrollment_request_t *request,
    argus_machine_credential_once_t *out);
esp_err_t argus_machine_service_rotate(
    const argus_principal_t *actor, const char *identifier,
    argus_machine_credential_once_t *out);
esp_err_t argus_machine_service_set_enabled(
    const argus_principal_t *actor, const char *identifier, bool enabled);
esp_err_t argus_machine_service_revoke(
    const argus_principal_t *actor, const char *identifier);
esp_err_t argus_machine_service_delete(
    const argus_principal_t *actor, const char *identifier);
esp_err_t argus_machine_service_quarantine_undisclosed(
    const char *identifier);

argus_machine_auth_outcome_t argus_machine_service_authenticate(
    uint32_t peer_key, const char *client_id,
    const uint8_t *username, size_t username_len,
    const uint8_t *password, size_t password_len,
    uint8_t receiving_interface);
esp_err_t argus_machine_service_revalidate(
    const argus_machine_principal_t *principal,
    uint8_t receiving_interface);

// Global machine-auth KDF token bucket, exposed so the pure suite can prove
// the bound holds independently of source address. Not part of the
// production call path beyond authenticate()'s own use.
bool argus_machine_service_kdf_global_admit_for_test(
    uint32_t peer_key, uint64_t now_us, uint32_t *retry_after_s);
void argus_machine_service_kdf_global_reset_for_test(void);

/**
 * @brief True when this source authenticated successfully within the TTL.
 *
 * Used by the MQTT broker to keep part of the pre-connect socket pool out of
 * reach of never-authenticated sources. Read the reservation's exact
 * guarantee - and its limits - on ARGUS_MACHINE_AUTH_PROVEN_SOURCES before
 * relying on it for anything.
 */
bool argus_machine_service_source_is_proven(uint32_t peer_key);
void argus_machine_service_mark_source_proven_for_test(
    uint32_t peer_key, uint64_t now_us);
void argus_machine_service_clear_proven_sources_for_test(void);

/**
 * @brief Operator-visible authentication-throttle condition.
 *
 * Asserted while the global KDF budget is refusing work, so a flood is a
 * reported condition rather than an unexplained inability to connect.
 */
typedef struct {
    /** Any machine authentication is currently being refused: the global KDF
     *  budget is exhausted, or at least one source is in failure lockout. */
    bool throttle_active;
    uint32_t refusals;             /**< Cumulative THROTTLED outcomes. */
    uint32_t global_refusals;      /**< Of those, refused by the KDF budget. */
    uint32_t blocked_sources;      /**< Sources currently in failure lockout. */
    uint32_t seconds_until_clear;  /**< 0 when not throttled. */
    uint32_t window_spent;
    uint32_t window_burst;
    uint32_t unproven_burst;
    uint32_t proven_sources;
} argus_machine_auth_throttle_status_t;

void argus_machine_service_get_throttle_status(
    argus_machine_auth_throttle_status_t *out);

bool argus_machine_service_scope_contains(
    const char *actor_scope, const char *target_scope);
bool argus_machine_service_topic_scope_contains(
    const char *semantic_scope, const char *topic);
bool argus_machine_service_enrollment_allowed(
    const argus_principal_t *actor,
    const argus_machine_enrollment_request_t *request);
void argus_machine_service_zero_credential(
    argus_machine_credential_once_t *credential);
size_t argus_machine_service_kdf_admitted(void);

#ifdef __cplusplus
}
#endif

#endif
