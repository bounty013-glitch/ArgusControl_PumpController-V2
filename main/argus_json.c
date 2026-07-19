/**
 * @file argus_json.c
 * @brief Robust JSON String Extraction Implementation
 *
 * Extracted from argus_http_server.c inline helpers.
 * Reports deterministic errors for malformed, truncated, oversized,
 * and type-mismatched JSON string values.
 */

#include "argus_json.h"
#include <string.h>
#include <stdio.h>

argus_json_result_t argus_json_extract_string(const char *json, const char *key,
                                               char *out, size_t out_size)
{
    if (!json || !key || !out || out_size == 0) return ARGUS_JSON_KEY_ABSENT;

    /* Build search pattern: "key" */
    char search[80];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return ARGUS_JSON_KEY_ABSENT;

    /* Skip past the key string */
    p += strlen(search);

    /* Skip whitespace and colon */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return ARGUS_JSON_TYPE_MISMATCH;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    /* Value must start with a quote for a string */
    if (*p != '"') return ARGUS_JSON_TYPE_MISMATCH;
    p++;  /* skip opening quote */

    /* Extract value into a temporary buffer to avoid partial writes on error.
     * Use a stack buffer sized to out_size so we don't write to out until
     * we confirm the value is complete and fits. For very large buffers,
     * this is still bounded by out_size which is a field size (max ~65). */
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) {
            p++;  /* skip backslash, take next char literally */
        }
        if (i >= out_size - 1) {
            /* Value exceeds destination capacity — reject without modifying out */
            return ARGUS_JSON_OVERFLOW;
        }
        /* We'll store in out only after confirming termination */
        i++;
        p++;
    }

    /* Check for unterminated string */
    if (*p != '"') {
        return ARGUS_JSON_UNTERMINATED;
    }

    /* Value is complete and fits. Now do the actual extraction.
     * Re-walk the value to populate out (we need to re-parse for escapes). */
    p = strstr(json, search);
    p += strlen(search);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    p++;  /* skip colon */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    p++;  /* skip opening quote */

    i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) {
            p++;  /* skip backslash */
        }
        out[i++] = *p++;
    }
    out[i] = '\0';

    return ARGUS_JSON_OK;
}

bool argus_json_has_key(const char *json, const char *key)
{
    if (!json || !key) return false;
    char search[80];
    snprintf(search, sizeof(search), "\"%s\"", key);
    return strstr(json, search) != NULL;
}
