#pragma once

#include <stdint.h>

#define ARGUS_CAN_ID_SET_SPEED_CMD   0x100
#define ARGUS_CAN_ID_START_CMD       0x101
#define ARGUS_CAN_ID_STOP_CMD        0x102
#define ARGUS_CAN_ID_SPEED_STATUS    0x180
#define ARGUS_CAN_ID_STATE_STATUS    0x181

#define ARGUS_STATE_FLAG_DRIVER_ENABLED  (1U << 0)
#define ARGUS_STATE_FLAG_MOTION_ACTIVE   (1U << 1)
#define ARGUS_STATE_FLAG_DIRECTION_FWD   (1U << 2)
#define ARGUS_STATE_FLAG_RUN_COMMAND     (1U << 3)

typedef struct {
    int32_t rpm_milli;
} argus_speed_cmd_t;

typedef struct {
    int32_t commanded_rpm_milli;
    int32_t applied_rpm_milli;
} argus_speed_status_frame_t;

typedef struct {
    uint32_t position_steps;
    uint8_t state_flags;
    uint8_t fault_code;
    uint8_t reserved0;
    uint8_t reserved1;
} argus_state_status_frame_t;
