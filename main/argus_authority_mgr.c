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

static argus_authority_snapshot_t s_authority = {
    .mode = ARGUS_AUTHORITY_NONE,
    .owner = ARGUS_AUTH_OWNER_NONE,
    .generation = 1
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

esp_err_t argus_authority_mgr_set_mode(argus_control_authority_t new_mode, argus_authority_owner_t new_owner)
{
    if (!s_initialized) argus_authority_mgr_init();

    xSemaphoreTake(s_auth_mutex, portMAX_DELAY);
    s_authority.mode = new_mode;
    s_authority.owner = new_owner;
    s_authority.generation++;
    xSemaphoreGive(s_auth_mutex);

    return ESP_OK;
}

esp_err_t argus_authority_request_service(argus_authority_owner_t requested_owner)
{
    if (requested_owner != ARGUS_AUTH_OWNER_BROWSER && requested_owner != ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI) {
        return ESP_ERR_INVALID_ARG;
    }

    // Step 1: Acquire s_dispatch_mutex to finish in-flight normal commands
    argus_cmd_router_lock_dispatch();

    // Step 2: Acquire s_auth_mutex, update mode to SERVICE_TRANSITION, increment generation
    argus_authority_mgr_set_mode(ARGUS_AUTHORITY_SERVICE_TRANSITION, ARGUS_AUTH_OWNER_NONE);

    // Step 3: Release s_dispatch_mutex
    argus_cmd_router_unlock_dispatch();

    // Step 4: Perform controlled stop without holding locks
    esp_err_t err = argus_state_mgr_stop_normal();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        // If normal stop fails, trigger E-stop
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

    if (timeout_ms <= 0) {
        // Stop timeout: trigger E-stop
        argus_state_mgr_estop();
    }

    // Step 5: Transition to LOCAL_SERVICE for requested owner
    argus_authority_mgr_set_mode(ARGUS_AUTHORITY_LOCAL_SERVICE, requested_owner);
    return ESP_OK;
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
