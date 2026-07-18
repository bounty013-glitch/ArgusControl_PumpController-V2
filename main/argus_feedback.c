#include "argus_feedback.h"

// Default disabled implementations
static bool default_is_available(void)
{
    return false; // Disabled by default
}

static esp_err_t default_get_actual_rpm(int32_t *out_rpm)
{
    if (out_rpm) {
        *out_rpm = 0; // Return exactly 0, do not fabricate values
    }
    return ESP_ERR_NOT_SUPPORTED; // Explicitly state unavailable
}

static esp_err_t default_get_actual_position(int64_t *out_steps)
{
    if (out_steps) {
        *out_steps = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

// Global interface instance
static const argus_feedback_interface_t s_feedback_interface = {
    .is_available = default_is_available,
    .get_actual_rpm = default_get_actual_rpm,
    .get_actual_position = default_get_actual_position,
};

const argus_feedback_interface_t *argus_feedback_get_interface(void)
{
    return &s_feedback_interface;
}
