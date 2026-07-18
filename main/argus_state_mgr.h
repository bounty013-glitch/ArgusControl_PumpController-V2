#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "argus_config.h"

typedef enum {
    ARGUS_STATE_BOOTING = 0,
    ARGUS_STATE_UNLOCKED,
    ARGUS_STATE_STARTING,
    ARGUS_STATE_RUNNING,
    ARGUS_STATE_DECELERATING,
    ARGUS_STATE_HOLDING,
    ARGUS_STATE_EMERGENCY_STOPPED,
    ARGUS_STATE_RECOVERING,
    ARGUS_STATE_FAULTED
} argus_machine_state_t;

// Lower-layer operations table interface
typedef struct {
    esp_err_t (*enable_driver)(void);
    esp_err_t (*disable_driver)(void);
    esp_err_t (*set_direction)(bool forward);
    esp_err_t (*set_target_rate)(int32_t rpm_milli, bool forward);
    esp_err_t (*stop_immediate)(void);
    esp_err_t (*stop_normal)(void);
    esp_err_t (*recover)(void);
    uint32_t (*get_error)(void);
} argus_motion_ops_t;

// Pure, reentrant state transition core structure
typedef struct {
    argus_machine_state_t machine_state;
    int32_t configured_target_rpm_milli;
    int32_t trajectory_target_rpm_milli;
    int32_t applied_rpm_milli;
    int32_t generated_rpm_milli;
    int64_t generated_step_count;
    bool requested_forward;
    bool applied_forward;
    bool driver_enabled;
    bool ramp_active;
    bool estop_latched;
    argus_machine_state_t estop_reset_destination;
    uint32_t fault_code;
    uint32_t command_generation;
    char last_rejection_reason[64];
    const argus_motion_ops_t *ops;
} argus_state_core_t;

typedef struct {
    argus_machine_state_t machine_state;
    int32_t configured_target_rpm_milli;
    int32_t trajectory_target_rpm_milli;
    int32_t applied_rpm_milli;
    int32_t generated_rpm_milli;
    int64_t generated_step_count;
    bool requested_forward;
    bool applied_forward;
    bool driver_enabled;
    bool ramp_active;
    bool estop_latched;
    uint32_t fault_code;
    uint32_t command_generation;
    bool feedback_available;
    char last_rejection_reason[64];
} argus_state_snapshot_t;

// Pure core transition functions (isolated, zero FreeRTOS tasks/mutexes)
void argus_state_core_init(argus_state_core_t *core, const argus_config_t *cfg, const argus_motion_ops_t *ops);
esp_err_t argus_state_core_set_target(argus_state_core_t *core, int32_t rpm_milli, bool forward);
esp_err_t argus_state_core_start(argus_state_core_t *core);
esp_err_t argus_state_core_stop_normal(argus_state_core_t *core);
esp_err_t argus_state_core_unlock(argus_state_core_t *core);
esp_err_t argus_state_core_estop(argus_state_core_t *core);
esp_err_t argus_state_core_reset_estop(argus_state_core_t *core);
esp_err_t argus_state_core_recover(argus_state_core_t *core);
void argus_state_core_evaluate_periodic(argus_state_core_t *core, int32_t applied_rpm, int32_t generated_rpm, bool ramp_active, uint32_t latched_error);
void argus_state_core_get_snapshot(const argus_state_core_t *core, argus_state_snapshot_t *snapshot);

// Live Production State Manager API (wraps s_prod_core with synchronization)
const char *argus_state_mgr_get_state_name(argus_machine_state_t state);
esp_err_t argus_state_mgr_init(const argus_config_t *cfg, const argus_motion_ops_t *ops);
esp_err_t argus_state_mgr_set_target(int32_t rpm_milli, bool forward);
esp_err_t argus_state_mgr_start(void);
esp_err_t argus_state_mgr_stop_normal(void);
esp_err_t argus_state_mgr_unlock(void);
esp_err_t argus_state_mgr_estop(void);
esp_err_t argus_state_mgr_reset_estop(void);
esp_err_t argus_state_mgr_recover(void);
void argus_state_mgr_get_snapshot(argus_state_snapshot_t *snapshot);
