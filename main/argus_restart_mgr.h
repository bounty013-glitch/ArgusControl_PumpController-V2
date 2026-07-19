/**
 * @file argus_restart_mgr.h
 * @brief Coordinated Restart Transaction Seam for Argus Pump Controller V2
 *
 * Provides a testable, injectable restart orchestration function used by
 * both production (argus_net_mgr) and pure unit tests. All side effects
 * are injected via the argus_restart_ops_t operations table.
 */

#ifndef ARGUS_RESTART_MGR_H
#define ARGUS_RESTART_MGR_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "argus_state_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Step indices for restart transaction ordering verification.
 */
typedef enum {
    ARGUS_RESTART_STEP_PREFLIGHT_SAFETY    = 1,
    ARGUS_RESTART_STEP_REVOKE_AUTHORITY    = 2,
    ARGUS_RESTART_STEP_RESPONSE_GRACE      = 3,
    ARGUS_RESTART_STEP_STOP_HTTP           = 4,
    ARGUS_RESTART_STEP_FINAL_SAFETY        = 5,
    ARGUS_RESTART_STEP_REBOOT              = 6,
} argus_restart_step_t;

/**
 * @brief Injectable operations for restart orchestration.
 *
 * All side effects are injected through this table. Production uses
 * real system calls; tests inject mock recorders.
 */
typedef struct {
    /** Get current machine state snapshot (preflight + final check) */
    void (*get_state_snapshot)(void *ctx, argus_state_snapshot_t *out);
    /** Revoke authority to prevent new motion commands */
    esp_err_t (*revoke_authority)(void *ctx);
    /** Response grace delay — lets HTTP response drain */
    void (*response_grace_delay)(void *ctx);
    /** Stop the HTTP server */
    esp_err_t (*stop_http)(void *ctx);
    /** Execute reboot — does not return on success */
    void (*reboot)(void *ctx);
    /** Context pointer passed to all operations */
    void *ctx;
} argus_restart_ops_t;

/**
 * @brief Result of a restart transaction with step-level detail.
 */
typedef struct {
    bool accepted;           /**< true = reboot was called (or would be on real hardware) */
    int  failed_at_step;     /**< 0 = no failure, 1-5 = step that caused rejection */
    bool authority_revoked;  /**< true if revoke_authority() was called */
    bool http_stopped;       /**< true if stop_http() was called */
    bool reboot_called;      /**< true if reboot() was called */
} argus_restart_result_t;

/**
 * @brief Execute a coordinated restart transaction.
 *
 * Transaction ordering:
 * 1. Preflight safety: reject if not HOLDING/UNLOCKED or E-stop/fault
 * 2. Revoke authority: prevent new motion commands
 * 3. Response grace delay: let HTTP response drain
 * 4. Stop HTTP server
 * 5. Final safety revalidation: abort if state changed during grace
 * 6. Reboot
 *
 * On any failure: stops at the failing step, does NOT reboot,
 * does NOT restore authority (fail closed).
 *
 * @param ops Injectable operations table.
 * @return Result with step-level detail.
 */
argus_restart_result_t argus_restart_execute(const argus_restart_ops_t *ops);

/**
 * @brief Evaluate whether a machine state snapshot is safe for restart.
 *
 * Pure function — no side effects. Exported so tests can verify the
 * exact same logic used by the production restart path.
 *
 * @param snap Machine state snapshot to evaluate.
 * @return true if HOLDING or UNLOCKED with no E-stop latched.
 */
bool argus_restart_is_safe(const argus_state_snapshot_t *snap);

/**
 * @brief Get the production restart operations table.
 *
 * Returned ops use real system calls:
 * - argus_state_mgr_get_snapshot()
 * - argus_authority_mgr_set_mode(NONE, NONE)
 * - vTaskDelay(500ms)
 * - argus_http_server_stop()
 * - esp_restart()
 */
void argus_restart_get_production_ops(argus_restart_ops_t *out_ops);

#ifdef __cplusplus
}
#endif

#endif /* ARGUS_RESTART_MGR_H */
