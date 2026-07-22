/**
 * @file argus_browser_command.h
 * @brief Pure decoder for the Phase 4B.4 browser command request contract.
 */

#ifndef ARGUS_BROWSER_COMMAND_H
#define ARGUS_BROWSER_COMMAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "argus_cmd_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_BROWSER_COMMAND_MAX_BODY_LEN 192U
#define ARGUS_BROWSER_COMMAND_MAX_TARGET_RPM_MILLI 200000

typedef enum {
    ARGUS_BROWSER_CMD_DECODE_OK = 0,
    ARGUS_BROWSER_CMD_DECODE_INVALID_ARGUMENT,
    ARGUS_BROWSER_CMD_DECODE_EMPTY_BODY,
    ARGUS_BROWSER_CMD_DECODE_BODY_TOO_LARGE,
    ARGUS_BROWSER_CMD_DECODE_TOP_LEVEL_NOT_OBJECT,
    ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON,
    ARGUS_BROWSER_CMD_DECODE_DUPLICATE_FIELD,
    ARGUS_BROWSER_CMD_DECODE_UNKNOWN_FIELD,
    ARGUS_BROWSER_CMD_DECODE_MISSING_FIELD,
    ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE,
    ARGUS_BROWSER_CMD_DECODE_UNSUPPORTED_COMMAND,
    ARGUS_BROWSER_CMD_DECODE_VALUE_OUT_OF_RANGE,
    ARGUS_BROWSER_CMD_DECODE_UNEXPECTED_FIELD,
} argus_browser_command_decode_result_t;

typedef struct {
    bool is_valid;
    argus_cmd_type_t command_type;
    int32_t target_rpm_milli;
    bool forward;
} argus_browser_command_request_t;

/**
 * @brief Decode one complete bounded JSON browser-command request.
 *
 * The input is length-delimited and need not be NUL-terminated. On every
 * failure, out_request is reset to a non-dispatchable state. This pure decoder
 * does not attach command source or authority generation.
 */
argus_browser_command_decode_result_t argus_browser_command_decode(
    const uint8_t *body,
    size_t body_len,
    argus_browser_command_request_t *out_request);

#ifdef __cplusplus
}
#endif

#endif /* ARGUS_BROWSER_COMMAND_H */
