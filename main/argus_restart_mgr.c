/**
 * @file argus_restart_mgr.c
 * @brief Coordinated Restart Transaction Implementation
 *
 * Single production-used restart orchestration function with injectable ops.
 * Both production and pure tests execute the same decision logic.
 */

#include "argus_restart_mgr.h"
#include "argus_authority_mgr.h"
#include "argus_cmd_router.h"
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
        .dispatch_locked = false,
        .authority_revoked = false,
        .http_stopped = false,
        .reboot_called = false,
    };

    if (!ops || !ops->get_state_snapshot || !ops->revoke_authority ||
        !ops->response_grace_delay || !ops->stop_http || !ops->reboot ||
        !ops->lock_dispatch || !ops->unlock_dispatch) {
        result.failed_at_step = ARGUS_RESTART_STEP_PREFLIGHT_SAFETY;
        return result;
    }

    /* Step 1: Lock dispatch gate — blocks new normal commands.
     * In-flight commands complete before the lock is acquired. */
    ops->lock_dispatch(ops->ctx);
    result.dispatch_locked = true;

    /* Step 2: Preflight safety check (under dispatch lock) */
    argus_state_snapshot_t snap;
    ops->get_state_snapshot(ops->ctx, &snap);

    if (!argus_restart_is_safe(&snap)) {
        result.failed_at_step = ARGUS_RESTART_STEP_PREFLIGHT_SAFETY;
        ops->unlock_dispatch(ops->ctx);
        result.dispatch_locked = false;
        return result;
    }

    /* Step 3: Revoke authority (under dispatch lock) — prevents new motion commands */
    esp_err_t err = ops->revoke_authority(ops->ctx);
    if (err == ESP_OK) {
        result.authority_revoked = true;
    } else {
        result.failed_at_step = ARGUS_RESTART_STEP_REVOKE_AUTHORITY;
        ops->unlock_dispatch(ops->ctx);
        result.dispatch_locked = false;
        return result;
    }

    /* Step 4: Unlock dispatch gate.
     * Authority is now NONE — normal commands are rejected by the authority
     * check, not by the dispatch gate. E-stop retains its existing bypass. */
    ops->unlock_dispatch(ops->ctx);
    result.dispatch_locked = false;

    /* Step 5: Response grace delay — lets HTTP response drain */
    ops->response_grace_delay(ops->ctx);

    /* Step 6: Stop HTTP server */
    err = ops->stop_http(ops->ctx);
    if (err == ESP_OK) {
        result.http_stopped = true;
    } else {
        result.failed_at_step = ARGUS_RESTART_STEP_STOP_HTTP;
        return result;
    }

    /* Step 7: Final safety revalidation */
    ops->get_state_snapshot(ops->ctx, &snap);
    if (!argus_restart_is_safe(&snap)) {
        result.failed_at_step = ARGUS_RESTART_STEP_FINAL_SAFETY;
        return result;
    }

    /* Step 8: Reboot — does not return on real hardware */
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
    return argus_http_server_stop();
}

static void prod_lock_dispatch(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Locking command dispatch gate.");
    argus_cmd_router_lock_dispatch();
}

static void prod_unlock_dispatch(void *ctx)
{
    (void)ctx;
    ESP_LOGD(TAG, "Unlocking command dispatch gate.");
    argus_cmd_router_unlock_dispatch();
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
    out_ops->lock_dispatch = prod_lock_dispatch;
    out_ops->unlock_dispatch = prod_unlock_dispatch;
    out_ops->revoke_authority = prod_revoke_authority;
    out_ops->response_grace_delay = prod_response_grace_delay;
    out_ops->stop_http = prod_stop_http;
    out_ops->reboot = prod_reboot;
    out_ops->ctx = NULL;
}
