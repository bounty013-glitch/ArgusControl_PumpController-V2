/**
 * @file argus_config_overlay.c
 * @brief Pure Configuration Overlay Implementation
 *
 * This is the production-used overlay logic extracted from the HTTP handler.
 * It is a pure function with no I/O — all inputs are caller-provided.
 */

#include "argus_config_overlay.h"
#include "argus_identity.h"
#include <string.h>
#include <stdio.h>

/* ── Scope parser ──────────────────────────────────────────────────── */

argus_config_scope_t argus_config_overlay_parse_scope(const char *scope_str)
{
    if (!scope_str) return ARGUS_CONFIG_SCOPE_INVALID;
    if (strcmp(scope_str, "identity") == 0) return ARGUS_CONFIG_SCOPE_IDENTITY;
    if (strcmp(scope_str, "wifi") == 0) return ARGUS_CONFIG_SCOPE_WIFI;
    return ARGUS_CONFIG_SCOPE_INVALID;
}

/* ── Identity overlay ──────────────────────────────────────────────── */

static argus_config_overlay_result_t apply_identity(
    const argus_config_payload_t *current,
    const argus_config_fields_t *fields,
    argus_config_payload_t *out_cfg)
{
    argus_config_overlay_result_t r = { .success = false };

    /* Check provisioning lock */
    if (current->provisioned_flags & ARGUS_CFG_PROVISIONED_IDENTITY) {
        r.error_code = "identity_locked";
        r.error_message = "Identity is locked after initial provisioning";
        return r;
    }

    /* All three fields required */
    if (!fields->has_client_id || !fields->has_unit_id || !fields->has_device_name) {
        r.error_code = "all_identity_fields_required";
        r.error_message = "client_id, unit_id, and device_name are all required";
        return r;
    }

    /* Validate each field */
    if (!argus_identity_validate_client_id(fields->client_id)) {
        r.error_code = "invalid_client_id";
        r.error_message = "client_id: 1-32 alphanumeric, hyphens, or underscores";
        return r;
    }
    if (!argus_identity_validate_unit_id(fields->unit_id)) {
        r.error_code = "invalid_unit_id";
        r.error_message = "unit_id: 1-32 alphanumeric, hyphens, or underscores";
        return r;
    }
    if (!argus_identity_validate_device_name(fields->device_name)) {
        r.error_code = "invalid_device_name";
        r.error_message = "device_name: 1-64 printable ASCII characters";
        return r;
    }

    /* Start from current config (preserves WiFi, other fields) */
    memcpy(out_cfg, current, sizeof(argus_config_payload_t));

    /* Overlay identity fields */
    snprintf(out_cfg->client_id, sizeof(out_cfg->client_id), "%s", fields->client_id);
    snprintf(out_cfg->unit_id, sizeof(out_cfg->unit_id), "%s", fields->unit_id);
    snprintf(out_cfg->device_name, sizeof(out_cfg->device_name), "%s", fields->device_name);

    /* Set provisioned flag atomically */
    out_cfg->provisioned_flags |= ARGUS_CFG_PROVISIONED_IDENTITY;

    r.success = true;
    return r;
}

/* ── WiFi overlay ──────────────────────────────────────────────────── */

static argus_config_overlay_result_t apply_wifi(
    const argus_config_payload_t *current,
    const argus_config_fields_t *fields,
    argus_config_payload_t *out_cfg)
{
    argus_config_overlay_result_t r = { .success = false };

    /* SSID field must be present */
    if (!fields->has_sta_ssid) {
        r.error_code = "sta_ssid_required";
        r.error_message = "sta_ssid field is required for WiFi scope";
        return r;
    }

    /* Start from current config (preserves identity, provisioned_flags) */
    memcpy(out_cfg, current, sizeof(argus_config_payload_t));

    if (strlen(fields->sta_ssid) == 0) {
        /* Explicit WiFi clear — zero both SSID and password */
        memset(out_cfg->sta_ssid, 0, sizeof(out_cfg->sta_ssid));
        memset(out_cfg->sta_pass, 0, sizeof(out_cfg->sta_pass));
        r.success = true;
        return r;
    }

    /* Non-empty SSID — handle password logic */

    /* Reject mask string */
    if (fields->has_sta_pass && strcmp(fields->sta_pass, ARGUS_CONFIG_MASK_STRING) == 0) {
        r.error_code = "mask_string_not_accepted";
        r.error_message = "Cannot use the mask string as a password";
        return r;
    }

    if (fields->has_sta_pass && strlen(fields->sta_pass) > 0) {
        /* New password supplied — replace */
        snprintf(out_cfg->sta_pass, sizeof(out_cfg->sta_pass), "%s", fields->sta_pass);
    } else if (strcmp(fields->sta_ssid, current->sta_ssid) == 0) {
        /* Same SSID, no new password — preserve existing password */
        /* (out_cfg->sta_pass already has the current value from memcpy) */
    } else {
        /* New SSID but no password — error */
        r.error_code = "password_required_for_new_ssid";
        r.error_message = "Password is required when changing the WiFi network";
        return r;
    }

    snprintf(out_cfg->sta_ssid, sizeof(out_cfg->sta_ssid), "%s", fields->sta_ssid);

    r.success = true;
    return r;
}

/* ── Public overlay dispatcher ─────────────────────────────────────── */

argus_config_overlay_result_t argus_config_overlay_apply(
    const argus_config_payload_t *current,
    argus_config_scope_t scope,
    const argus_config_fields_t *fields,
    argus_config_payload_t *out_cfg)
{
    argus_config_overlay_result_t r = { .success = false };

    if (!current || !fields || !out_cfg) {
        r.error_code = "internal_error";
        r.error_message = "NULL argument to overlay";
        return r;
    }

    switch (scope) {
        case ARGUS_CONFIG_SCOPE_IDENTITY:
            return apply_identity(current, fields, out_cfg);
        case ARGUS_CONFIG_SCOPE_WIFI:
            return apply_wifi(current, fields, out_cfg);
        default:
            r.error_code = "invalid_scope";
            r.error_message = "scope must be identity or wifi";
            return r;
    }
}
