#include "argus_conversions.h"
#include <stdlib.h>

uint64_t argus_conversions_steps_per_rev(const argus_config_t *cfg)
{
    if (cfg == NULL || cfg->gearbox_ratio_den == 0) {
        return 0ULL;
    }
    uint64_t base = (uint64_t)cfg->motor_full_steps_per_rev * (uint64_t)cfg->microsteps;
    return (base * (uint64_t)cfg->gearbox_ratio_num) / (uint64_t)cfg->gearbox_ratio_den;
}

uint64_t argus_conversions_rpm_to_mhz(int32_t milli_rpm, const argus_config_t *cfg)
{
    uint64_t spr = argus_conversions_steps_per_rev(cfg);
    if (spr == 0ULL) {
        return 0ULL;
    }
    // Calculate millihertz: mHz = (milli_rpm * spr) / 60
    uint64_t abs_milli_rpm = (milli_rpm < 0) ? (uint64_t)(-milli_rpm) : (uint64_t)milli_rpm;
    return (abs_milli_rpm * spr) / 60ULL;
}

int32_t argus_conversions_mhz_to_rpm(uint64_t freq_mhz, bool forward, const argus_config_t *cfg)
{
    uint64_t spr = argus_conversions_steps_per_rev(cfg);
    if (spr == 0ULL) {
        return 0;
    }
    // Calculate milli-RPM: milli_rpm = (freq_mhz * 60) / spr
    uint64_t milli_rpm = (freq_mhz * 60ULL) / spr;
    if (milli_rpm > INT32_MAX) {
        milli_rpm = INT32_MAX;
    }
    return forward ? (int32_t)milli_rpm : -(int32_t)milli_rpm;
}

int32_t argus_conversions_flow_to_rpm(int32_t flow_microliters_per_min, int32_t displacement_microliters_per_rev)
{
    if (displacement_microliters_per_rev <= 0) {
        return 0;
    }
    int64_t flow = (int64_t)flow_microliters_per_min;
    int64_t milli_rpm = (flow * 1000LL) / (int64_t)displacement_microliters_per_rev;
    if (milli_rpm > INT32_MAX) {
        return INT32_MAX;
    }
    if (milli_rpm < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)milli_rpm;
}
