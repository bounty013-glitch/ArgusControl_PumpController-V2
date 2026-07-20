/**
 * @file argus_authority_mgr.c
 * @brief Exclusive Control Authority & Owner Manager Implementation
 */

#include "argus_authority_mgr.h"
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

    esp_err_t pair_err = argus_authority_validate_pair(new_mode, new_owner);
    if (pair_err != ESP_OK) {
        return pair_err;
    }

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

esp_err_t argus_authority_validate_pair(argus_control_authority_t mode,
                                        argus_authority_owner_t owner)
{
    switch (mode) {
        case ARGUS_AUTHORITY_NONE:
            return owner == ARGUS_AUTH_OWNER_NONE ? ESP_OK : ESP_ERR_INVALID_ARG;

        case ARGUS_AUTHORITY_SUPERVISORY:
            return owner == ARGUS_AUTH_OWNER_MQTT ? ESP_OK : ESP_ERR_INVALID_ARG;

        case ARGUS_AUTHORITY_SERVICE_TRANSITION:
            return owner == ARGUS_AUTH_OWNER_NONE ? ESP_OK : ESP_ERR_INVALID_ARG;

        case ARGUS_AUTHORITY_LOCAL_SERVICE:
            return (owner == ARGUS_AUTH_OWNER_BROWSER ||
                    owner == ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI)
                       ? ESP_OK
                       : ESP_ERR_INVALID_ARG;

        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t prod_prepare_transition(void *ctx) {
    (void)ctx;
    // State-only: set SERVICE_TRANSITION/NONE and increment generation under s_auth_mutex.
    // Does NOT acquire dispatch, stop motion, poll state, call E-stop, or delay.
    return argus_authority_mgr_set_mode(ARGUS_AUTHORITY_SERVICE_TRANSITION, ARGUS_AUTH_OWNER_NONE);
}

static esp_err_t prod_grant_local(void *ctx, argus_authority_owner_t owner) {
    (void)ctx;
    // State-only: set LOCAL_SERVICE/<owner> under s_auth_mutex.
    // Machine-state validation is performed by the orchestrator before this call.
    return argus_authority_grant_local_service(owner);
}

static void prod_abort_transition(void *ctx) {
    (void)ctx;
    // State-only: set NONE/NONE under s_auth_mutex.
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
    esp_err_t pair_err = argus_authority_validate_pair(new_mode, new_owner);
    if (pair_err != ESP_OK) {
        core->last_error = pair_err;
        return pair_err;
    }
    core->mode = new_mode;
    core->owner = new_owner;
    core->generation++;
    core->last_error = ESP_OK;
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

    // State-only: machine-state validation is performed by the orchestrator's
    // verify_machine_safe callback before this grant call.
    argus_authority_mgr_set_mode(ARGUS_AUTHORITY_LOCAL_SERVICE, requested_owner);
    return ESP_OK;
}

void argus_authority_abort_service_transition(void)
{
    ESP_LOGW("argus_auth_mgr", "Aborting service transition -> setting authority NONE/NONE");
    argus_authority_mgr_set_mode(ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);
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
