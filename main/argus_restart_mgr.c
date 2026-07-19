/**
 * @file argus_restart_mgr.c
 * @brief Coordinated Restart Transaction Implementation
 *
 * Single production-used restart orchestration function with injectable ops.
 * Both production and pure tests execute the same decision logic.
 */

#include "argus_restart_mgr.h"
#include "argus_authority_mgr.h"
#include "argus_http_server.h"
#include "argus_state_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "argus_restart";

/* ── Pure safety evaluation ────────────────────────────────────────── */

bool argus_restart_is_safe(const argus_state_snapshot_t *snap)
{
    if (!snap) return false;
    if (snap->estop_latched) return false;
    if (snap->machine_state == ARGUS_STATE_EMERGENCY_STOPPED) return false;
    if (snap->machine_state == ARGUS_STATE_FAULTED) return false;
    return (snap->machine_state == ARGUS_STATE_HOLDING ||
            snap->machine_state == ARGUS_STATE_UNLOCKED);
}

/* ── Restart transaction ───────────────────────────────────────────── */

argus_restart_result_t argus_restart_execute(const argus_restart_ops_t *ops)
{
    argus_restart_result_t result = {
        .accepted = false,
        .failed_at_step = 0,
        .authority_revoked = false,
        .http_stopped = false,
        .reboot_called = false,
    };

    if (!ops || !ops->get_state_snapshot || !ops->revoke_authority ||
        !ops->response_grace_delay || !ops->stop_http || !ops->reboot) {
        result.failed_at_step = ARGUS_RESTART_STEP_PREFLIGHT_SAFETY;
        return result;
    }

    /* Step 1: Preflight safety check */
    argus_state_snapshot_t snap;
    ops->get_state_snapshot(ops->ctx, &snap);

    if (!argus_restart_is_safe(&snap)) {
        result.failed_at_step = ARGUS_RESTART_STEP_PREFLIGHT_SAFETY;
        return result;
    }

    /* Step 2: Revoke authority — prevents new motion commands */
    esp_err_t err = ops->revoke_authority(ops->ctx);
    result.authority_revoked = true;
    if (err != ESP_OK) {
        result.failed_at_step = ARGUS_RESTART_STEP_REVOKE_AUTHORITY;
        return result;
    }

    /* Step 3: Response grace delay — lets HTTP response drain */
    ops->response_grace_delay(ops->ctx);

    /* Step 4: Stop HTTP server */
    err = ops->stop_http(ops->ctx);
    result.http_stopped = true;
    if (err != ESP_OK) {
        result.failed_at_step = ARGUS_RESTART_STEP_STOP_HTTP;
        return result;
    }

    /* Step 5: Final safety revalidation */
    ops->get_state_snapshot(ops->ctx, &snap);
    if (!argus_restart_is_safe(&snap)) {
        result.failed_at_step = ARGUS_RESTART_STEP_FINAL_SAFETY;
        return result;
    }

    /* Step 6: Reboot — does not return on real hardware */
    result.accepted = true;
    result.reboot_called = true;
    ops->reboot(ops->ctx);

    return result;  /* Only reached in tests with mock reboot */
}

/* ── Production operations ─────────────────────────────────────────── */

static void prod_get_state_snapshot(void *ctx, argus_state_snapshot_t *out)
{
    (void)ctx;
    argus_state_mgr_get_snapshot(out);
}

static esp_err_t prod_revoke_authority(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Revoking authority for restart.");
    return argus_authority_mgr_set_mode(ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);
}

static void prod_response_grace_delay(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Response grace delay (500ms)...");
    vTaskDelay(pdMS_TO_TICKS(500));
}

static esp_err_t prod_stop_http(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Stopping HTTP server.");
    argus_http_server_stop();
    return ESP_OK;
}

static void prod_reboot(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Safety revalidated. Rebooting now.");
    esp_restart();
}

void argus_restart_get_production_ops(argus_restart_ops_t *out_ops)
{
    if (!out_ops) return;
    out_ops->get_state_snapshot = prod_get_state_snapshot;
    out_ops->revoke_authority = prod_revoke_authority;
    out_ops->response_grace_delay = prod_response_grace_delay;
    out_ops->stop_http = prod_stop_http;
    out_ops->reboot = prod_reboot;
    out_ops->ctx = NULL;
}
