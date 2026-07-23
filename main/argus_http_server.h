/**
 * @file argus_http_server.h
 * @brief Controller-Hosted HTTP Server for Local Browser Portal
 *
 * Phase 4B.1 through 4B.6 status, identity, configuration, service,
 * credential-management, Wi-Fi recovery, and local command API.
 *
 * Lifecycle:
 *   argus_http_server_init() — One-time creation of lifecycle objects.
 *   argus_http_server_start() — Starts the HTTP server (idempotent if running).
 *   argus_http_server_stop()  — Stops the HTTP server (idempotent if stopped).
 *
 * The HTTP server runs in network modes where the Service AP is active:
 *   UNCOMMISSIONED_AP, AP_DISCOVERABLE, SERVICE_AP_ONLY.
 *
 * The HTTP server does NOT run in:
 *   BOOTSTRAP, COMMISSIONED_STA, SERVICE_TRANSITION, NETWORK_FAULT.
 *
 * Current scope:
 *   Phase 4B.1:
 *   - GET endpoints for machine status, identity, and the portal dashboard.
 *   - POST endpoint for portal password change (NVS mutation).
 *   - GET endpoint for logout (clears browser auth cache).
 *
 *   Phase 4B.2:
 *   - GET /api/config: Current configuration (passwords masked, lock state).
 *   - POST /api/config/save: Scoped config update (identity or WiFi).
 *     Identity is one-time-provisioned and locked. WiFi is always editable.
 *   - GET /config/identity: Identity provisioning page.
 *   - GET /config/wifi: WiFi configuration page.
 *   - POST /api/restart: Coordinated restart (motion safety checked).
 *
 *   Phase 4B.3/4B.3a:
 *   - Authenticated service entry/exit POST endpoints.
 *   - Authenticated manual Wi-Fi reconnect POST endpoint.
 *
 *   Phase 4B.4:
 *   - POST /api/command: Strict browser-local command admission and routing.
 *   - Available only in SERVICE_AP_ONLY with LOCAL_SERVICE/BROWSER authority.
 *
 *   Phase 4B.5:
 *   - GET /controls: Authenticated technician motion controls and live status.
 *
 *   Phase 4B.6:
 *   - POST /api/factory-reset: Strictly confirmed, deferred configuration reset.
 *
 * Access control:
 *   Human browser routes are SoftAP-only. Phase 4D.3 session, capability,
 *   Origin, Content-Type, and CSRF middleware protects each classified route.
 *
 * Configuration write gates:
 *   Config writes (POST /api/config/save) are only allowed in:
 *     UNCOMMISSIONED_AP, SERVICE_AP_ONLY.
 *   Config reads (GET /api/config) are allowed in all HTTP-active modes.
 *   Restart is allowed in all HTTP-active modes (motion safety checked).
 *
 * Security:
 *   - HTTP Basic Auth is retired and has no compatibility fallback.
 *   - Credentials use the encrypted Phase 4D security stores.
 *   - Authentication and credential-storage errors fail closed.
 *   - No secrets returned (passwords, AP credentials, WiFi passwords).
 *   - sta_pass_set boolean replaces actual password in config responses.
 *   - No permissive CORS headers.
 *   - Cache-Control: no-store on all API responses.
 *   - Method validation on all handlers.
 *   - Content-Type validation on POST endpoints.
 *   - HTML escaping on all device-supplied values in the portal DOM.
 *   - Password buffers are zeroed after use where technically meaningful.
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "argus_state_mgr.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One-time initialization of HTTP server lifecycle objects.
 *
 * Creates firmware-lifetime mutex. Must be called once before start/stop.
 * Safe to call from app_main before the scheduler or networking is ready.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM on allocation failure,
 *         ESP_ERR_INVALID_STATE if already initialized.
 */
esp_err_t argus_http_server_init(void);

/**
 * @brief Start the HTTP server on the Service AP interface.
 *
 * Registers URI handlers and begins accepting connections. If any handler
 * registration fails, the server is stopped and the start fails cleanly.
 * Idempotent: returns ESP_OK if the server is already running.
 *
 * @return ESP_OK on success, ESP_FAIL on server start or registration failure,
 *         ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t argus_http_server_start(void);

/**
 * @brief Stop the HTTP server.
 *
 * Unregisters handlers and stops accepting connections. Shutdown duration
 * depends on esp_http_server draining active sessions (not bounded by
 * this module). Idempotent: returns ESP_OK if already stopped.
 *
 * If httpd_stop() fails, the server handle is preserved (not cleared) to
 * prevent unverified duplicate starts. is_running() will continue to return
 * true until a successful stop.
 *
 * IMPORTANT: Must NOT be called from within an HTTP request handler.
 * Use argus_net_mgr event posting for deferred lifecycle transitions.
 *
 * @return ESP_OK on success, error code if httpd_stop() failed,
 *         ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t argus_http_server_stop(void);

/**
 * @brief Query whether the HTTP server is currently running.
 *
 * @return true if the server handle is non-NULL, false otherwise.
 */
bool argus_http_server_is_running(void);

/* ── Pure test seams (diagnostic builds only) ────────────────────── */

#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
/**
 * @brief Test-visible wrapper for static json_escape().
 *
 * Escapes src for safe JSON string embedding:
 *   - Backslash-escapes " and \\
 *   - Replaces control characters (< 0x20) with space
 *   - Truncates output to dst_size (always NUL-terminated)
 *
 * NOTE: This is JSON escaping, not HTML escaping. The portal JavaScript
 * uses a separate h() function for DOM-safe rendering.
 */
void argus_http_test_json_escape(const char *src, char *dst, size_t dst_size);

/** @brief Format the production-used machine-status JSON object. */
int argus_http_test_format_machine_status_json(
    const argus_state_snapshot_t *snapshot,
    char *out,
    size_t out_size);

/**
 * @brief Confirm the production browser-command POST registration contract.
 */
bool argus_http_test_command_registration(void);

/** @brief Confirm the factory-reset production POST registration. */
bool argus_http_test_factory_reset_registration(void);

/** @brief Confirm the dedicated controls-page production registration. */
bool argus_http_test_controls_registration(void);

/** @brief Return the embedded controls page for pure contract tests. */
const char *argus_http_test_controls_page(size_t *out_len);

/** @brief Return the embedded commissioning page for contract tests. */
const char *argus_http_test_commission_page(size_t *out_len);

/** @brief Exercise the strict production login decoder. */
bool argus_http_test_decode_login(const uint8_t *body, size_t body_len);

/** @brief Return the production browser-command capability mapping. */
uint64_t argus_http_test_command_capability(uint32_t command_type);
size_t argus_http_test_registered_route_count(void);
bool argus_http_test_registered_route(
    size_t index, const char **path, httpd_method_t *method);
#endif

#ifdef __cplusplus
}
#endif
