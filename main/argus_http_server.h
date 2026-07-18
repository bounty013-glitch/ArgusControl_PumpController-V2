/**
 * @file argus_http_server.h
 * @brief Controller-Hosted HTTP Server for Local Browser Portal
 *
 * Phase 4B.1: Read-only status and identity API with embedded mobile portal.
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
 * All endpoints are read-only in Phase 4B.1. No configuration mutation,
 * no motion commands, no authority acquisition.
 *
 * Interface enforcement:
 *   In APSTA mode (AP_DISCOVERABLE), connections are only accepted through
 *   the AP interface. Connections arriving on the STA interface are rejected
 *   at the session level before any handler executes.
 *
 * Security:
 *   - No secrets returned (passwords, AP credentials).
 *   - No permissive CORS headers.
 *   - Cache-Control: no-store on all API responses.
 *   - Method validation on all handlers.
 *   - Content-Type validation on POST (future phases).
 *   - HTML escaping on all device-supplied values in the portal DOM.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

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
#endif

#ifdef __cplusplus
}
#endif
