#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

// System config structure
typedef struct {
    // Motor configurations
    uint32_t motor_full_steps_per_rev;  // e.g. 200
    uint32_t microsteps;                 // e.g. 4 (1/4 microstepping)
    uint32_t gearbox_ratio_num;          // e.g. 10
    uint32_t gearbox_ratio_den;          // e.g. 1
    
    // Speed boundaries in milli-RPM (1.2 RPM = 1200)
    int32_t min_output_milli_rpm;       // e.g. 500 (0.5 RPM)
    int32_t max_output_milli_rpm;       // e.g. 200000 (200.0 RPM)
    
    // Hardware constraints
    uint32_t min_step_pulse_width_us;   // e.g. 4 us (UIM spec)
    
    // Trajectory defaults
    int32_t accel_milli_rpm_per_sec;    // default acceleration rate
    int32_t decel_milli_rpm_per_sec;    // default deceleration rate
 
    // Pin definitions
    gpio_num_t step_gpio;
    gpio_num_t dir_gpio;
    gpio_num_t en_gpio;
    bool enable_active_low;             // Physically verified on production motor hardware
    bool step_active_low;               // Common-anode active-low step configuration
    bool dir_inverted;                  // Direction inversion status (common-anode)

    // Commissioning parameters
    char client_name[32];
    char unit_id[32];
} argus_config_t;

/**
 * @brief Retrieve the active system hardware configuration.
 * @return Const pointer to configuration structure.
 */
const argus_config_t *argus_config_get(void);

/**
 * @brief Initialize configuration, loading overrides from NVS if present.
 * @return ESP_OK on success.
 */
esp_err_t argus_config_init(void);
