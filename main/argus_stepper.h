#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    gpio_num_t step_gpio;
    gpio_num_t dir_gpio;
    gpio_num_t en_gpio;
    bool enable_pin_active_high;
    bool has_enable_pin;
    uint32_t ledc_freq_hz;
    uint32_t max_step_freq_hz;
    uint32_t enable_delay_ms;
    uint32_t motor_full_steps_per_rev;
    uint32_t microsteps;
    uint32_t gearbox_ratio_num;
    uint32_t gearbox_ratio_den;
    uint32_t max_motor_rpm;
    uint32_t ramp_interval_ms;
    uint32_t ramp_rpm_per_sec_milli;
} argus_stepper_hw_config_t;

typedef struct {
    uint32_t position_steps;
    int32_t position_output_mrev;
    int32_t commanded_speed_hz;
    int32_t applied_speed_hz;
    int32_t commanded_rpm_milli;
    int32_t applied_rpm_milli;
    bool run_commanded;
    bool motion_active;
    bool direction_forward;
    bool enabled;
    bool driver_locked;
    bool emergency_stopped;
    uint8_t fault_code;
} argus_stepper_status_t;

esp_err_t argus_stepper_init(const argus_stepper_hw_config_t *config);
esp_err_t argus_stepper_set_speed(int32_t speed_hz);
esp_err_t argus_stepper_set_speed_rpm_milli(int32_t rpm_milli);
esp_err_t argus_stepper_start(void);
esp_err_t argus_stepper_stop(void);
esp_err_t argus_stepper_emergency_stop(void);
esp_err_t argus_stepper_unlock(void);
esp_err_t argus_stepper_get_status(argus_stepper_status_t *status);
