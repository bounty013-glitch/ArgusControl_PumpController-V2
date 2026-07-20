/**
 * @file argus_json.h
 * @brief Robust JSON String Extraction for Argus Configuration Payloads
 *
 * Extracted from argus_http_server.c to enable pure unit testing.
 * Provides deterministic error reporting for malformed, truncated,
 * oversized, and type-mismatched JSON string values.
 */

#ifndef ARGUS_JSON_H
#define ARGUS_JSON_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result codes for JSON string extraction.
 */
typedef enum {
    ARGUS_JSON_OK = 0,           /**< Valid string extracted and NUL-terminated */
    ARGUS_JSON_KEY_ABSENT,       /**< Key not found in JSON body */
    ARGUS_JSON_TYPE_MISMATCH,    /**< Key found but value is not a quoted string */
    ARGUS_JSON_UNTERMINATED,     /**< Opening quote found but no closing quote before NUL */
    ARGUS_JSON_OVERFLOW,         /**< Valid string exceeds destination buffer capacity */
} argus_json_result_t;

/**
 * @brief Extract a JSON string value by key from a flat JSON body.
 *
 * Lightweight parser sufficient for single-level configuration payloads.
 * Handles backslash-escaped characters within the value string.
 *
 * Guarantees:
 * - On OK: out is NUL-terminated, contains the unescaped value.
 * - On KEY_ABSENT: out is not modified.
 * - On TYPE_MISMATCH: out is not modified.
 * - On UNTERMINATED: out is not modified.
 * - On OVERFLOW: out is not modified.
 * - Valid empty string ("") returns OK with out[0] = '\0'.
 * - Maximum-length values that fit in out_size (including NUL) are accepted.
 *
 * @param json     NUL-terminated JSON body to search.
 * @param key      Key name to find (without quotes).
 * @param out      Destination buffer for the extracted string.
 * @param out_size Size of destination buffer (must include room for NUL).
 * @return Result code indicating success or specific failure mode.
 */
argus_json_result_t argus_json_extract_string(const char *json, const char *key,
                                               char *out, size_t out_size);

/**
 * @brief Check whether a key exists in a flat JSON body.
 *
 * @param json NUL-terminated JSON body.
 * @param key  Key name to find (without quotes).
 * @return true if the key is present (regardless of value type).
 */
bool argus_json_has_key(const char *json, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* ARGUS_JSON_H */
