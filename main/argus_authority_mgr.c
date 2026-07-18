/**
 * @file argus_authority_mgr.c
 * @brief Exclusive Control Authority & Owner Manager Implementation
 */

#include "argus_authority_mgr.h"
#include "argus_cmd_router.h"
#include "argus_state_mgr.h"
#include "argus_nvs_config.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static argus_authority_core_t s_authority = {
    .mode = ARGUS_AUTHORITY_NONE,
    .owner = ARGUS_AUTH_OWNER_NONE,
    .generation = 1,
    .last_error = ESP_OK
};

static SemaphoreHandle_t s_auth_mutex = NULL;
static StaticSemaphore_t s_auth_mutex_buffer;
static bool s_initialized = false;

esp_err_t argus_authority_mgr_init(void)
{
    if (!s_initialized) {
        s_auth_mutex = xSemaphoreCreateMutexStatic(&s_auth_mutex_buffer);
        s_authority.mode = ARGUS_AUTHORITY_NONE;
        s_authority.owner = ARGUS_AUTH_OWNER_NONE;
        s_authority.generation = 1;
        s_authority.last_error = ESP_OK;
        s_initialized = true;
    }
    return ESP_OK;
}

esp_err_t argus_authority_mgr_get_snapshot(argus_authority_snapshot_t *out_snap)
{
    if (!out_snap) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) argus_authority_mgr_init();

    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    out_snap->mode = s_authority.mode;
    out_snap->owner = s_authority.owner;
    out_snap->generation = s_authority.generation;
    xSemaphoreGive(s_auth_mutex);

    return ESP_OK;
}

#include "esp_log.h"

const char *argus_authority_mgr_get_mode_name(argus_control_authority_t mode)
{
    switch (mode) {
        case ARGUS_AUTHORITY_NONE: return "NONE";
        case ARGUS_AUTHORITY_SUPERVISORY: return "SUPERVISORY";
        case ARGUS_AUTHORITY_SERVICE_TRANSITION: return "SERVICE_TRANSITION";
        case ARGUS_AUTHORITY_LOCAL_SERVICE: return "LOCAL_SERVICE";
        default: return "UNKNOWN";
    }
}

const char *argus_authority_mgr_get_owner_name(argus_authority_owner_t owner)
{
    switch (owner) {
        case ARGUS_AUTH_OWNER_NONE: return "NONE";
        case ARGUS_AUTH_OWNER_MQTT: return "MQTT";
        case ARGUS_AUTH_OWNER_BROWSER: return "BROWSER";
        case ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI: return "DIAGNOSTIC_CLI";
        default: return "UNKNOWN";
    }
}

esp_err_t argus_authority_mgr_set_mode(argus_control_authority_t new_mode, argus_authority_owner_t new_owner)
{
    if (!s_initialized) argus_authority_mgr_init();

    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    argus_control_authority_t old_mode = s_authority.mode;
    argus_authority_owner_t old_owner = s_authority.owner;
    s_authority.mode = new_mode;
    s_authority.owner = new_owner;
    s_authority.generation++;
    uint32_t gen = s_authority.generation;
    xSemaphoreGive(s_auth_mutex);

    if (old_mode != new_mode || old_owner != new_owner) {
        ESP_LOGI("argus_auth_mgr", "authority: %s/%s -> %s/%s (gen %lu)",
                 argus_authority_mgr_get_mode_name(old_mode), argus_authority_mgr_get_owner_name(old_owner),
                 argus_authority_mgr_get_mode_name(new_mode), argus_authority_mgr_get_owner_name(new_owner),
                 (unsigned long)gen);
    }

    return ESP_OK;
}

static esp_err_t prod_prepare_transition(void *ctx) {
    (void)ctx;
    return argus_authority_prepare_service_transition();
}

static esp_err_t prod_grant_local(void *ctx, argus_authority_owner_t owner) {
    (void)ctx;
    return argus_authority_grant_local_service(owner);
}

static void prod_abort_transition(void *ctx) {
    (void)ctx;
    argus_authority_abort_service_transition();
}

void argus_authority_get_production_service_ops(argus_service_authority_ops_t *out_ops) {
    if (out_ops) {
        out_ops->prepare_transition = prod_prepare_transition;
        out_ops->grant_local = prod_grant_local;
        out_ops->abort_transition = prod_abort_transition;
        out_ops->ctx = NULL;
    }
}

esp_err_t argus_authority_core_set_mode(argus_authority_core_t *core,
                                       argus_control_authority_t new_mode,
                                       argus_authority_owner_t new_owner)
{
    if (!core) return ESP_ERR_INVALID_ARG;
    core->mode = new_mode;
    core->owner = new_owner;
    core->generation++;
    return ESP_OK;
}

esp_err_t argus_authority_core_prepare_service_transition(argus_authority_core_t *core)
{
    if (!core) return ESP_ERR_INVALID_ARG;
    return argus_authority_core_set_mode(core, ARGUS_AUTHORITY_SERVICE_TRANSITION, ARGUS_AUTH_OWNER_NONE);
}

esp_err_t argus_authority_core_grant_local_service(argus_authority_core_t *core,
                                                   argus_authority_owner_t requested_owner)
{
    if (!core) return ESP_ERR_INVALID_ARG;
    if (requested_owner != ARGUS_AUTH_OWNER_BROWSER && requested_owner != ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI) {
        argus_authority_core_abort_service_transition(core);
        return ESP_ERR_INVALID_ARG;
    }
    if (core->mode != ARGUS_AUTHORITY_SERVICE_TRANSITION) {
        argus_authority_core_abort_service_transition(core);
        return ESP_ERR_INVALID_STATE;
    }
    return argus_authority_core_set_mode(core, ARGUS_AUTHORITY_LOCAL_SERVICE, requested_owner);
}

void argus_authority_core_abort_service_transition(argus_authority_core_t *core)
{
    if (core) {
        argus_authority_core_set_mode(core, ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);
    }
}

esp_err_t argus_authority_prepare_service_transition(void)
{
    // Step 1: Acquire s_dispatch_mutex to finish in-flight normal commands
    argus_cmd_router_lock_dispatch();

    // Step 2: Acquire s_auth_mutex, update mode to SERVICE_TRANSITION, increment generation
    argus_authority_mgr_set_mode(ARGUS_AUTHORITY_SERVICE_TRANSITION, ARGUS_AUTH_OWNER_NONE);

    // Step 3: Release s_dispatch_mutex
    argus_cmd_router_unlock_dispatch();

    // Step 4: Perform controlled stop without holding locks
    esp_err_t err = argus_state_mgr_stop_normal();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        argus_state_mgr_estop();
    }

    // Wait up to 5000 ms for confirmed stopped machine state
    argus_state_snapshot_t state_snap;
    int timeout_ms = 5000;
    while (timeout_ms > 0) {
        argus_state_mgr_get_snapshot(&state_snap);
        if (state_snap.machine_state == ARGUS_STATE_HOLDING ||
            state_snap.machine_state == ARGUS_STATE_UNLOCKED ||
            state_snap.machine_state == ARGUS_STATE_EMERGENCY_STOPPED ||
            state_snap.machine_state == ARGUS_STATE_FAULTED) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout_ms -= 50;
    }

    if (timeout_ms <= 0 || state_snap.machine_state == ARGUS_STATE_EMERGENCY_STOPPED ||
        state_snap.machine_state == ARGUS_STATE_FAULTED || state_snap.estop_latched) {
        argus_state_mgr_estop();
        ESP_LOGE("argus_auth_mgr", "Service transition prep aborted: state=%s, estop_latched=%d.",
                 argus_state_mgr_get_state_name(state_snap.machine_state), (int)state_snap.estop_latched);
        argus_authority_abort_service_transition();
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t argus_authority_grant_local_service(argus_authority_owner_t requested_owner)
{
    if (requested_owner != ARGUS_AUTH_OWNER_BROWSER && requested_owner != ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI) {
        argus_authority_abort_service_transition();
        return ESP_ERR_INVALID_ARG;
    }

    argus_authority_snapshot_t snap;
    argus_authority_mgr_get_snapshot(&snap);
    if (snap.mode != ARGUS_AUTHORITY_SERVICE_TRANSITION) {
        ESP_LOGE("argus_auth_mgr", "Cannot grant local service: current mode is %s (expected SERVICE_TRANSITION)",
                 argus_authority_mgr_get_mode_name(snap.mode));
        argus_authority_abort_service_transition();
        return ESP_ERR_INVALID_STATE;
    }

    argus_state_snapshot_t state_snap;
    argus_state_mgr_get_snapshot(&state_snap);
    if ((state_snap.machine_state != ARGUS_STATE_HOLDING && state_snap.machine_state != ARGUS_STATE_UNLOCKED) ||
        state_snap.estop_latched || state_snap.machine_state == ARGUS_STATE_FAULTED) {
        ESP_LOGE("argus_auth_mgr", "Cannot grant local service: machine state=%s, estop_latched=%d",
                 argus_state_mgr_get_state_name(state_snap.machine_state), (int)state_snap.estop_latched);
        argus_authority_abort_service_transition();
        return ESP_ERR_INVALID_STATE;
    }

    argus_authority_mgr_set_mode(ARGUS_AUTHORITY_LOCAL_SERVICE, requested_owner);
    return ESP_OK;
}

void argus_authority_abort_service_transition(void)
{
    ESP_LOGW("argus_auth_mgr", "Aborting service transition -> setting authority NONE/NONE");
    argus_authority_mgr_set_mode(ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);
}

esp_err_t argus_authority_request_service(argus_authority_owner_t requested_owner)
{
    esp_err_t err = argus_authority_prepare_service_transition();
    if (err != ESP_OK) return err;
    return argus_authority_grant_local_service(requested_owner);
}

esp_err_t argus_authority_request_exit(void)
{
    // Step 1: Acquire s_dispatch_mutex
    argus_cmd_router_lock_dispatch();

    // Step 2: Set SERVICE_TRANSITION
    argus_authority_mgr_set_mode(ARGUS_AUTHORITY_SERVICE_TRANSITION, ARGUS_AUTH_OWNER_NONE);

    // Step 3: Release s_dispatch_mutex
    argus_cmd_router_unlock_dispatch();

    // Step 4: Perform controlled stop
    argus_state_mgr_stop_normal();

    // Verify machine is stopped
    argus_state_snapshot_t state_snap;
    int timeout_ms = 5000;
    while (timeout_ms > 0) {
        argus_state_mgr_get_snapshot(&state_snap);
        if (state_snap.machine_state == ARGUS_STATE_HOLDING || state_snap.machine_state == ARGUS_STATE_UNLOCKED) {
            break;
        }
        // Block exit if E-stop or fault remains unresolved!
        if (state_snap.machine_state == ARGUS_STATE_EMERGENCY_STOPPED || state_snap.machine_state == ARGUS_STATE_FAULTED) {
            argus_authority_mgr_set_mode(ARGUS_AUTHORITY_LOCAL_SERVICE, ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI);
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout_ms -= 50;
    }

    if (timeout_ms <= 0) {
        return ESP_ERR_TIMEOUT;
    }

    // Step 5: Perform restart
    esp_restart();
    return ESP_OK;
}

bool argus_authority_validate_permission(const argus_authority_snapshot_t *snap,
                                         argus_cmd_source_t source,
                                         argus_cmd_type_t cmd_type)
{
    if (!snap) return false;

    // Internal safety actions (E-stop) are ALWAYS allowed regardless of authority mode or owner
    if (source == ARGUS_CMD_SRC_INTERNAL_SAFETY || cmd_type == ARGUS_CMD_TYPE_ESTOP) {
        return true;
    }

    switch (snap->mode) {
        case ARGUS_AUTHORITY_NONE:
            // Uncommissioned: no external motion commands permitted
            return false;

        case ARGUS_AUTHORITY_SUPERVISORY:
            // MQTT owns supervisory motion; CLI and Browser motion rejected
            if (source == ARGUS_CMD_SRC_MQTT_SUPERVISORY && snap->owner == ARGUS_AUTH_OWNER_MQTT) {
                return true;
            }
            return false;

        case ARGUS_AUTHORITY_SERVICE_TRANSITION:
            // Transitioning: reject all non-safety commands
            return false;

        case ARGUS_AUTHORITY_LOCAL_SERVICE:
            // Local service mode: check active owner
            if (source == ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL && snap->owner == ARGUS_AUTH_OWNER_BROWSER) {
                return true;
            }
            if (source == ARGUS_CMD_SRC_CLI_DIAGNOSTIC && snap->owner == ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI) {
                return true;
            }
            return false;

        default:
            return false;
    }
}
