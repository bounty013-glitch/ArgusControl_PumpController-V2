/**
 * @file argus_cmd_router.h
 * @brief Command Router & Dispatch Serialization Gate for Argus Pump Controller V2
 */

#ifndef ARGUS_CMD_ROUTER_H
#define ARGUS_CMD_ROUTER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "argus_cmd_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize command router and dispatch mutex.
 * @return ESP_OK on success.
 */
esp_err_t argus_cmd_router_init(void);

/**
 * @brief Dispatch a normalized command envelope through authority gate and dispatch mutex.
 * @param env Pointer to normalized command envelope.
 * @return ESP_OK if command was authorized and executed;
 *         ESP_ERR_INVALID_STATE if rejected due to authority mode or stale generation;
 *         ESP_ERR_INVALID_ARG if env is invalid.
 */
esp_err_t argus_cmd_router_dispatch(const argus_command_envelope_t *env);

/**
 * @brief Acquire dispatch mutex for authority transition.
 *        Ensures in-flight normal commands complete before authority mode changes.
 */
void argus_cmd_router_lock_dispatch(void);

/**
 * @brief Release dispatch mutex after authority transition.
 */
void argus_cmd_router_unlock_dispatch(void);

/**
 * @brief Non-mutating authority probe. Validates whether an envelope would be authorized
 *        under current authority state without modifying motion state, setpoints, or authority.
 * @param env Pointer to command envelope to probe.
 * @return ESP_OK if envelope is authorized;
 *         ESP_ERR_INVALID_STATE if rejected by authority rules or generation mismatch;
 *         ESP_ERR_INVALID_ARG if env is NULL.
 */
esp_err_t argus_cmd_router_check_authority(const argus_command_envelope_t *env);

#ifdef __cplusplus
}
#endif

#endif /* ARGUS_CMD_ROUTER_H */
