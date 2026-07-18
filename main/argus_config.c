#include "argus_config.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"

// Include sdkconfig for macros defined in Kconfig.projbuild
#include "sdkconfig.h"

static const char *TAG = "argus_config";

// Active config instance
static argus_config_t s_config;

const argus_config_t *argus_config_get(void)
{
    return &s_config;
}

esp_err_t argus_config_init(void)
{
    // Default hardware configuration values (GUI configured microstepping, etc.)
    s_config.motor_full_steps_per_rev = 200U;
    s_config.microsteps = 4U;            // 1/4 microstepping
    s_config.gearbox_ratio_num = 10U;
    s_config.gearbox_ratio_den = 1U;
    
    // Limits
    s_config.min_output_milli_rpm = 500;       // 0.5 RPM
    s_config.max_output_milli_rpm = 200000;    // 200.0 RPM
    
    // Minimum STEP pulse width from UIM344 manual (duration should > 4us)
    s_config.min_step_pulse_width_us = 4U;
    
    s_config.accel_milli_rpm_per_sec = 10000;  // 10 RPM/sec
    s_config.decel_milli_rpm_per_sec = 10000;  // 10 RPM/sec

    // GPIO pins matching legacy mapping
    s_config.step_gpio = GPIO_NUM_3;
    s_config.dir_gpio = GPIO_NUM_4;
    s_config.en_gpio = GPIO_NUM_5;
    s_config.enable_active_low = true;  // Physically verified on production motor hardware
    s_config.step_active_low = true;    // Common-anode active-low configuration
    s_config.dir_inverted = true;       // Direction inversion due to common-anode DIR

    // Initialize with Kconfig defaults
#ifdef CONFIG_ARGUS_CLIENT_NAME
    strlcpy(s_config.client_name, CONFIG_ARGUS_CLIENT_NAME, sizeof(s_config.client_name));
#else
    strlcpy(s_config.client_name, "dev_client", sizeof(s_config.client_name));
#endif

#ifdef CONFIG_ARGUS_UNIT_ID
    strlcpy(s_config.unit_id, CONFIG_ARGUS_UNIT_ID, sizeof(s_config.unit_id));
#else
    strlcpy(s_config.unit_id, "pump_v2_001", sizeof(s_config.unit_id));
#endif

    // Try loading dynamic settings from NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t len = sizeof(s_config.client_name);
        nvs_get_str(nvs_handle, "client_name", s_config.client_name, &len);
        len = sizeof(s_config.unit_id);
        nvs_get_str(nvs_handle, "unit_id", s_config.unit_id, &len);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Loaded client identity from NVS: client=%s unit=%s", s_config.client_name, s_config.unit_id);
    } else {
        ESP_LOGI(TAG, "Legacy motor-configuration namespace empty; using motor Kconfig defaults. (Phase 4A commissioning configuration remains valid)");
    }

    return ESP_OK;
}
