#ifndef ARGUS_LOCAL_RECOVERY_H
#define ARGUS_LOCAL_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_RECOVERY_GPIO 0
#define ARGUS_RECOVERY_SAMPLE_MS 20U
#define ARGUS_RECOVERY_DEBOUNCE_MS 100U
#define ARGUS_RECOVERY_HOLD_MS 10000U

typedef enum {
    ARGUS_RECOVERY_DETECT_NONE = 0,
    ARGUS_RECOVERY_DETECT_QUALIFIED_RELEASE,
} argus_recovery_detector_result_t;

typedef struct {
    bool raw_high;
    bool stable_high;
    bool stable_initialized;
    bool startup_release_seen;
    bool hold_qualified;
    bool triggered;
    uint32_t raw_stable_ms;
    uint32_t held_ms;
} argus_recovery_detector_t;

typedef struct {
    esp_err_t (*persist_request)(void *ctx);
    esp_err_t (*post_network_request)(void *ctx);
    void *ctx;
} argus_local_recovery_ops_t;

typedef struct {
    bool accepted;
    bool persisted;
    bool network_posted;
    esp_err_t error;
} argus_local_recovery_commit_result_t;

typedef struct {
    bool initialized;
    bool startup_release_seen;
    bool hold_qualified;
    bool triggered;
    uint32_t held_ms;
    uint32_t trigger_count;
} argus_local_recovery_status_t;

void argus_recovery_detector_init(argus_recovery_detector_t *detector,
                                  bool initial_high);
argus_recovery_detector_result_t argus_recovery_detector_update(
    argus_recovery_detector_t *detector, bool level_high,
    uint32_t elapsed_ms);

argus_local_recovery_commit_result_t argus_local_recovery_commit(
    const argus_local_recovery_ops_t *ops);

esp_err_t argus_local_recovery_init(void);
esp_err_t argus_local_recovery_get_status(
    argus_local_recovery_status_t *out_status);

#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
esp_err_t argus_local_recovery_clear_for_test(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
