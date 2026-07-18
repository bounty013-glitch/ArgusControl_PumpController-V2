#include "argus_tests.h"
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "argus_config.h"
#include "argus_conversions.h"
#include "argus_step_gen.h"
#include "argus_trajectory.h"
#include "argus_feedback.h"
#include "driver/gpio.h"

static const char *TAG = "argus_tests";

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            ESP_LOGE(TAG, "Test failed: %s at line %d", msg, __LINE__); \
            return ESP_FAIL; \
        } \
    } while (0)

// Simulated Bresenham scheduler helper
typedef struct {
    uint64_t integer;
    uint64_t remainder;
    uint64_t denominator;
} test_timing_t;

static test_timing_t calculate_test_timing(int32_t rpm_milli, uint32_t steps_per_rev)
{
    test_timing_t t = {0};
    if (rpm_milli == 0) return t;
    uint64_t num = 600000000000ULL;
    uint64_t den = (uint64_t)rpm_milli * (uint64_t)steps_per_rev;
    t.integer = num / den;
    t.remainder = num % den;
    t.denominator = den;
    return t;
}

static uint64_t simulate_pulses(int32_t rpm_milli, uint32_t steps_per_rev, int steps)
{
    test_timing_t t = calculate_test_timing(rpm_milli, steps_per_rev);
    uint64_t acc = 0;
    uint64_t total_ticks = 0;
    for (int i = 0; i < steps; i++) {
        acc += t.remainder;
        uint64_t step_ticks = t.integer;
        if (acc >= t.denominator) {
            step_ticks += 1;
            acc -= t.denominator;
        }
        total_ticks += step_ticks;
    }
    return total_ticks;
}

esp_err_t argus_tests_run_all(void)
{
    ESP_LOGI(TAG, "Starting V2 Pump Controller Bresenham and Phase 3A Trajectory tests...");

    // Create test configuration
    argus_config_t cfg = {
        .motor_full_steps_per_rev = 200,
        .microsteps = 4,
        .gearbox_ratio_num = 10,
        .gearbox_ratio_den = 1,
        .min_output_milli_rpm = 500,
        .max_output_milli_rpm = 200000,
        .min_step_pulse_width_us = 6, // 6 us pulse width
        .accel_milli_rpm_per_sec = 10000, // 10.0 RPM/sec (200 mRPM / 20ms)
        .decel_milli_rpm_per_sec = 10000, // 10.0 RPM/sec (200 mRPM / 20ms)
        .step_gpio = GPIO_NUM_3,
        .dir_gpio = GPIO_NUM_4,
        .en_gpio = GPIO_NUM_5,
        .enable_active_low = true,
        .step_active_low = true,
        .dir_inverted = true,
    };

    uint64_t spr = argus_conversions_steps_per_rev(&cfg);
    TEST_ASSERT(spr == 8000ULL, "Default steps/rev should be 8000");

    // =================================================================
    // 1. Safe disabled level & Glitch-free initialization state check
    // =================================================================
    int en_level = gpio_get_level(cfg.en_gpio);
    TEST_ASSERT(en_level == 1, "Glitch-free ENA initialization should default to HIGH (disabled)");
    TEST_ASSERT(argus_step_gen_is_driver_enabled() == false, "Driver enablement state should report false initially");

    int step_level = gpio_get_level(cfg.step_gpio);
    TEST_ASSERT(step_level == 1, "STEP initialization should default to inactive-high (1)");

    // Initialize trajectory test instance
    TEST_ASSERT(argus_trajectory_init(&cfg) == ESP_OK, "Trajectory init");

    // =================================================================
    // 2. Exact rational timing / Bresenham checks
    // =================================================================
    uint64_t ticks_0_5 = simulate_pulses(500, spr, 8000);
    TEST_ASSERT(ticks_0_5 == 1200000000ULL, "0.5 RPM timing test failed");

    uint64_t ticks_0_7 = simulate_pulses(700, spr, 280);
    TEST_ASSERT(ticks_0_7 == 30000000ULL, "0.7 RPM timing test failed");

    uint64_t ticks_3s_0_5 = simulate_pulses(500, spr, 200);
    uint64_t ticks_3s_0_6 = simulate_pulses(600, spr, 240);
    TEST_ASSERT(ticks_3s_0_5 == 30000000ULL, "0.5 RPM 3s check");
    TEST_ASSERT(ticks_3s_0_6 == 30000000ULL, "0.6 RPM 3s check");

    // =================================================================
    // 3. Phase 3A Deterministic Trajectory Ramp Unit Tests
    // =================================================================

    // A. Zero to 0.5 RPM (500 mRPM) ramp (200 mRPM/tick -> 200, 400, 500 clamped)
    argus_trajectory_stop_immediate();
    argus_step_gen_start();
    argus_trajectory_set_target_rpm_milli(500, true);
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 0, "Initial applied speed must start at 0");

    argus_trajectory_step_tick_20ms();
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 200, "Tick 1 applied = 200");

    argus_trajectory_step_tick_20ms();
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 400, "Tick 2 applied = 400");

    argus_trajectory_step_tick_20ms();
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 500, "Tick 3 applied = 500 (exact clamp)");
    TEST_ASSERT(argus_trajectory_is_ramp_active() == false, "Ramp settled at target");

    // B. Non-multiple clamping check: Target 550 mRPM (clamped from 400 -> 550 without overshoot)
    argus_trajectory_stop_immediate();
    argus_trajectory_set_target_rpm_milli(550, true);
    argus_trajectory_step_tick_20ms(); // 200
    argus_trajectory_step_tick_20ms(); // 400
    argus_trajectory_step_tick_20ms(); // 550
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 550, "Clamped to 550 without overshoot");

    // C. Zero to 20.0 RPM (20000 mRPM) ramp (requires 100 ticks = 2.0 seconds)
    argus_trajectory_stop_immediate();
    argus_trajectory_set_target_rpm_milli(20000, true);
    for (int i = 0; i < 99; i++) {
        argus_trajectory_step_tick_20ms();
        TEST_ASSERT(argus_trajectory_is_ramp_active() == true, "Ramp active during acceleration");
    }
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 19800, "Applied at tick 99 = 19800");
    argus_trajectory_step_tick_20ms();
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 20000, "Applied at tick 100 = 20000 (settled)");
    TEST_ASSERT(argus_trajectory_is_ramp_active() == false, "Ramp finished");

    // D. 20 to 72 RPM ramp (20000 -> 72000 mRPM)
    argus_trajectory_set_target_rpm_milli(72000, true);
    for (int i = 0; i < 260; i++) {
        argus_trajectory_step_tick_20ms();
    }
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 72000, "Applied reached 72 RPM");

    // E. 72 to 20 RPM deceleration (72000 -> 20000 mRPM)
    argus_trajectory_set_target_rpm_milli(20000, true);
    for (int i = 0; i < 260; i++) {
        argus_trajectory_step_tick_20ms();
    }
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 20000, "Decelerated back to 20 RPM");

    // F. 20 RPM to Normal Stop (20000 -> 0 mRPM)
    argus_trajectory_stop_normal();
    for (int i = 0; i < 100; i++) {
        argus_trajectory_step_tick_20ms();
    }
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 0, "Normal stop reached zero applied speed");
    TEST_ASSERT(gpio_get_level(cfg.en_gpio) == 0, "Normal stop must keep ENA LOW to retain holding torque");

    // G. Direction Reversal Through Zero (Forward -> Reverse)
    argus_trajectory_set_target_rpm_milli(2000, true); // Forward 2.0 RPM
    for (int i = 0; i < 10; i++) argus_trajectory_step_tick_20ms();
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 2000, "Forward 2 RPM settled");

    // Command Reverse 2.0 RPM
    argus_trajectory_set_target_rpm_milli(2000, false);
    TEST_ASSERT(argus_trajectory_is_reversing() == true, "Reversal active");

    // Must decelerate forward speed to 0 first
    for (int i = 0; i < 10; i++) {
        argus_trajectory_step_tick_20ms();
    }
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 0, "Decelerated to zero before DIR toggle");

    // Next tick toggles DIR and begins ramp in reverse direction
    argus_trajectory_step_tick_20ms();
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 200, "Ramping up in reverse direction");
    for (int i = 0; i < 9; i++) argus_trajectory_step_tick_20ms();
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 2000, "Reverse target reached");

    // H. Immediate Stop
    argus_trajectory_stop_immediate();
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 0, "Immediate stop sets applied to 0");
    TEST_ASSERT(argus_trajectory_get_target_rpm_milli() == 0, "Immediate stop sets target to 0");
    TEST_ASSERT(gpio_get_level(cfg.en_gpio) == 0, "Immediate stop keeps ENA LOW for holding torque");

    // I. Unlock Driver
    argus_trajectory_unlock();
    TEST_ASSERT(gpio_get_level(cfg.en_gpio) == 1, "Unlock drives ENA HIGH to release shaft");

    // J. Explicit Diagnostic Recovery ('r')
    argus_trajectory_set_target_rpm_milli(1000, true);
    for (int i = 0; i < 5; i++) argus_trajectory_step_tick_20ms();
    
    esp_err_t rec_err = argus_trajectory_recover();
    TEST_ASSERT(rec_err == ESP_OK, "Recovery execution");
    TEST_ASSERT(argus_trajectory_get_target_rpm_milli() == 0, "Recovery resets target to 0");
    TEST_ASSERT(argus_trajectory_get_applied_rpm_milli() == 0, "Recovery resets applied to 0");
    TEST_ASSERT(gpio_get_level(cfg.en_gpio) == 1, "Recovery leaves driver unlocked (ENA HIGH)");

    // Clean stop & unlock for final checks
    argus_trajectory_unlock();

    // =================================================================
    // 4. Feedback seam audit check
    // =================================================================
    const argus_feedback_interface_t *fb = argus_feedback_get_interface();
    TEST_ASSERT(fb != NULL, "Feedback interface should not be null");
    TEST_ASSERT(fb->is_available() == false, "Feedback must be explicitly unavailable by default");
    int32_t fb_rpm = 999;
    esp_err_t fb_rpm_err = fb->get_actual_rpm(&fb_rpm);
    TEST_ASSERT(fb_rpm_err == ESP_ERR_NOT_SUPPORTED, "get_actual_rpm must return ESP_ERR_NOT_SUPPORTED");
    TEST_ASSERT(fb_rpm == 0, "get_actual_rpm must not return fabricated/nonzero values when unsupported");

    ESP_LOGI(TAG, "All automated V2 Pump Controller Bresenham and Phase 3A Trajectory tests PASSED successfully.");
    return ESP_OK;
}
