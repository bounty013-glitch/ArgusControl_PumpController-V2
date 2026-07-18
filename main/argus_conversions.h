#pragma once

#include <stdint.h>
#include "argus_config.h"

/**
 * @brief Calculate the total output steps per one revolution of the pump shaft.
 * @param cfg Pointer to active configuration.
 * @return Steps per output shaft revolution.
 */
uint64_t argus_conversions_steps_per_rev(const argus_config_t *cfg);

/**
 * @brief Convert output milli-RPM to STEP generation frequency in millihertz (mHz).
 *        1 Hz = 1000 mHz.
 * @param milli_rpm Target speed in milli-RPM.
 * @param cfg Pointer to active configuration.
 * @return Frequency in millihertz (mHz).
 */
uint64_t argus_conversions_rpm_to_mhz(int32_t milli_rpm, const argus_config_t *cfg);

/**
 * @brief Convert STEP generation frequency in millihertz (mHz) to equivalent milli-RPM.
 * @param freq_mhz Step generation frequency in millihertz.
 * @param cfg Pointer to active configuration.
 * @return Speed in milli-RPM.
 */
int32_t argus_conversions_mhz_to_rpm(uint64_t freq_mhz, bool forward, const argus_config_t *cfg);

/**
 * @brief Convert volumetric flow rate to output shaft target RPM.
 * @param flow_microliters_per_min Volumetric flow rate.
 * @param displacement_microliters_per_rev Pump displacement constant.
 * @return Target output speed in milli-RPM.
 */
int32_t argus_conversions_flow_to_rpm(int32_t flow_microliters_per_min, int32_t displacement_microliters_per_rev);
