/**
 * @file argus_cmd_router.c
 * @brief Command Router & Dispatch Serialization Gate Implementation
 */

#include "argus_cmd_router.h"
#include "argus_authority_mgr.h"
#include "argus_state_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static SemaphoreHandle_t s_dispatch_mutex = NULL;
static StaticSemaphore_t s_dispatch_mutex_buffer;
static bool s_initialized = false;

esp_err_t argus_cmd_router_init(void)
{
    if (!s_initialized) {
        s_dispatch_mutex = xSemaphoreCreateMutexStatic(&s_dispatch_mutex_buffer);
        s_initialized = true;
    }
    return ESP_OK;
}

void argus_cmd_router_lock_dispatch(void)
{
    if (!s_initialized) argus_cmd_router_init();
    xSemaphoreTake(s_dispatch_mutex, portMAX_DELAY);
}

void argus_cmd_router_unlock_dispatch(void)
{
    if (s_dispatch_mutex) {
        xSemaphoreGive(s_dispatch_mutex);
    }
}

esp_err_t argus_cmd_router_dispatch(const argus_command_envelope_t *env)
{
    if (!env) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) argus_cmd_router_init();

    // Internal safety actions (E-Stop) bypass dispatch and authority mutexes for microsecond preemption
    if (env->source == ARGUS_CMD_SRC_INTERNAL_SAFETY || env->command_type == ARGUS_CMD_TYPE_ESTOP) {
        return argus_state_mgr_estop();
    }

    // Step 1: Acquire s_dispatch_mutex
    xSemaphoreTake(s_dispatch_mutex, portMAX_DELAY);

    // Step 2: Call argus_authority_mgr_get_snapshot() (briefly acquires s_auth_mutex, copies snapshot, releases it)
    argus_authority_snapshot_t snap;
    esp_err_t err = argus_authority_mgr_get_snapshot(&snap);
    if (err != ESP_OK) {
        xSemaphoreGive(s_dispatch_mutex);
        return err;
    }

    // Step 3: Verify authority generation
    if (env->authority_generation != snap.generation) {
        ESP_LOGW("argus_cmd_router", "Dispatch rejected: Envelope generation (%lu) != active generation (%lu)",
                 (unsigned long)env->authority_generation, (unsigned long)snap.generation);
        xSemaphoreGive(s_dispatch_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // Step 4: Validate permission
    if (!argus_authority_validate_permission(&snap, env->source, env->command_type)) {
        ESP_LOGW("argus_cmd_router", "Dispatch rejected: Source %d command %d not permitted in authority mode %d owner %d",
                 (int)env->source, (int)env->command_type, (int)snap.mode, (int)snap.owner);
        xSemaphoreGive(s_dispatch_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // Step 5: Execute state manager action while holding s_dispatch_mutex
    switch (env->command_type) {
        case ARGUS_CMD_TYPE_SET_TARGET:
            err = argus_state_mgr_set_target(env->target_rpm_milli, env->forward);
            break;
        case ARGUS_CMD_TYPE_START:
            err = argus_state_mgr_start();
            break;
        case ARGUS_CMD_TYPE_STOP_NORMAL:
            err = argus_state_mgr_stop_normal();
            break;
        case ARGUS_CMD_TYPE_UNLOCK:
            err = argus_state_mgr_unlock();
            break;
        case ARGUS_CMD_TYPE_RESET_ESTOP:
            err = argus_state_mgr_reset_estop();
            break;
        case ARGUS_CMD_TYPE_RECOVER:
            err = argus_state_mgr_recover();
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
    }

    // Step 6: Release s_dispatch_mutex
    xSemaphoreGive(s_dispatch_mutex);
    return err;
}
