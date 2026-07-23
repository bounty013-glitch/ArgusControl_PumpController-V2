#include "argus_security_http.h"

#include <stdlib.h>
#include <string.h>

#include "argus_auth_service.h"
#include "argus_http_security.h"
#include "argus_net_mgr.h"
#include "argus_password_verifier.h"
#include "argus_security_admin.h"
#include "argus_security_audit.h"
#include "argus_security_directory.h"
#include "argus_security_store.h"
#include "argus_session_manager.h"
#include "cJSON.h"
#include "esp_timer.h"

#define SECURITY_BODY_MAX 1024U
#define SECURITY_RESPONSE_MAX 4096U

static esp_timer_handle_t s_ap_apply_timer;
static esp_timer_handle_t s_recovery_timer;

static argus_security_directory_snapshot_t *directory_snapshot_alloc(void)
{
    argus_security_directory_snapshot_t *directory =
        calloc(1U, sizeof(*directory));
    if (directory == NULL) return NULL;
    if (argus_security_directory_get_snapshot(directory) != ESP_OK) {
        argus_password_zeroize(directory, sizeof(*directory));
        free(directory);
        return NULL;
    }
    return directory;
}

static void directory_snapshot_free(
    argus_security_directory_snapshot_t *directory)
{
    if (directory == NULL) return;
    argus_password_zeroize(directory, sizeof(*directory));
    free(directory);
}

static void set_headers(httpd_req_t *req)
{
    argus_http_security_headers(req, false);
}

static esp_err_t send_json(
    httpd_req_t *req, const char *status, const char *body)
{
    set_headers(req);
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, body);
}

static bool require_access(
    httpd_req_t *req, argus_permission_set_t permissions,
    bool mutation, argus_http_security_context_t *out)
{
    argus_http_access_result_t result = argus_http_security_require(
        req, permissions, mutation, out);
    if (result == ARGUS_HTTP_ACCESS_OK) return true;
    (void)argus_http_security_send_error(req, result);
    return false;
}

static cJSON *receive_object(httpd_req_t *req)
{
    if (req == NULL || req->content_len == 0U ||
        req->content_len >= SECURITY_BODY_MAX) {
        if (req != NULL && req->content_len >= SECURITY_BODY_MAX) {
            httpd_resp_set_hdr(req, "Connection", "close");
        }
        return NULL;
    }
    char body[SECURITY_BODY_MAX];
    size_t received = 0U;
    while (received < req->content_len) {
        int result = httpd_req_recv(
            req, body + received, req->content_len - received);
        if (result <= 0 ||
            (size_t)result > req->content_len - received) {
            memset(body, 0, sizeof(body));
            httpd_resp_set_hdr(req, "Connection", "close");
            return NULL;
        }
        received += (size_t)result;
    }
    body[received] = '\0';
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(
        body, received, &end, false);
    bool complete = end == body + received;
    memset(body, 0, sizeof(body));
    if (root == NULL || !cJSON_IsObject(root) ||
        !complete) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool object_keys(
    const cJSON *root, const char *const *keys,
    size_t key_count)
{
    size_t count = 0U;
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (item->string == NULL) return false;
        bool known = false;
        for (size_t i = 0U; i < key_count; ++i) {
            if (strcmp(item->string, keys[i]) == 0) {
                if (cJSON_GetObjectItemCaseSensitive(
                        root, keys[i]) != item) {
                    return false;
                }
                known = true;
                break;
            }
        }
        if (!known) return false;
        count++;
    }
    return count == key_count;
}

static void zeroize_json_string(cJSON *root, const char *name)
{
    cJSON *item = root == NULL
        ? NULL
        : cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        argus_password_zeroize(
            item->valuestring, strlen(item->valuestring));
    }
}

static argus_permission_set_t permission_from_name(const char *name)
{
    static const struct {
        const char *name;
        argus_permission_set_t value;
    } MAP[] = {
        {"view_status", ARGUS_PERMISSION_VIEW_STATUS},
        {"request_authority", ARGUS_PERMISSION_REQUEST_AUTHORITY},
        {"motion", ARGUS_PERMISSION_MOTION},
        {"software_estop", ARGUS_PERMISSION_SOFTWARE_ESTOP},
        {"reset_software_estop", ARGUS_PERMISSION_RESET_SOFTWARE_ESTOP},
        {"ack_alarms", ARGUS_PERMISSION_ACK_ALARMS},
        {"manage_users", ARGUS_PERMISSION_MANAGE_USERS},
        {"manage_roles", ARGUS_PERMISSION_MANAGE_ROLES},
        {"manage_client_admins", ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS},
        {"enroll_machines", ARGUS_PERMISSION_ENROLL_MACHINES},
        {"revoke_machines", ARGUS_PERMISSION_REVOKE_MACHINES},
        {"view_audit", ARGUS_PERMISSION_VIEW_AUDIT},
        {"manage_network", ARGUS_PERMISSION_MANAGE_NETWORK},
        {"change_ap_secret", ARGUS_PERMISSION_CHANGE_AP_SECRET},
        {"manage_client_network", ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK},
        {"manage_mqtt", ARGUS_PERMISSION_MANAGE_MQTT},
        {"modify_identity", ARGUS_PERMISSION_MODIFY_IDENTITY},
        {"modify_protected_config",
         ARGUS_PERMISSION_MODIFY_PROTECTED_CONFIG},
        {"commission", ARGUS_PERMISSION_COMMISSION},
        {"calibrate", ARGUS_PERMISSION_CALIBRATE},
        {"manage_firmware", ARGUS_PERMISSION_MANAGE_FIRMWARE},
        {"invoke_recovery", ARGUS_PERMISSION_INVOKE_RECOVERY},
        {"full_security_reset", ARGUS_PERMISSION_FULL_SECURITY_RESET},
    };
    if (name == NULL) return 0U;
    for (size_t i = 0U; i < sizeof(MAP) / sizeof(MAP[0]); ++i) {
        if (strcmp(name, MAP[i].name) == 0) return MAP[i].value;
    }
    return 0U;
}

static bool permission_array(
    const cJSON *array, argus_permission_set_t *out)
{
    if (!cJSON_IsArray(array) || out == NULL ||
        cJSON_GetArraySize(array) > 23) {
        return false;
    }
    argus_permission_set_t result = 0U;
    const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
        argus_permission_set_t permission =
            permission_from_name(item->valuestring);
        if (permission == 0U || (result & permission) != 0U) return false;
        result |= permission;
    }
    *out = result;
    return true;
}

static bool custom_role_mask_field(
    const cJSON *root, const char *name, uint16_t *out)
{
    cJSON *array = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsArray(array) || out == NULL) return false;
    argus_security_directory_snapshot_t *directory =
        directory_snapshot_alloc();
    if (directory == NULL) return false;
    uint16_t result = 0U;
    bool valid = true;
    const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsString(item) || item->valuestring == NULL) {
            valid = false;
            break;
        }
        size_t index = directory->payload.custom_role_count;
        for (size_t i = 0U;
             i < directory->payload.custom_role_count; ++i) {
            if (strcmp(directory->payload.custom_roles[i].identifier,
                       item->valuestring) == 0) {
                index = i;
                break;
            }
        }
        if (index == directory->payload.custom_role_count ||
            (result & (UINT16_C(1) << index)) != 0U) {
            valid = false;
            break;
        }
        result |= (uint16_t)(UINT16_C(1) << index);
    }
    directory_snapshot_free(directory);
    if (valid) *out = result;
    return valid;
}

static esp_err_t reserve_admin_audit(
    const argus_http_security_context_t *security,
    argus_audit_event_type_t type,
    const char *target,
    const char *reason)
{
    return argus_security_audit_append(
        type, ARGUS_AUDIT_OUTCOME_SUCCESS,
        (uint8_t)security->principal.type,
        security->principal.identifier, target, "SOFTAP",
        reason, security->principal.security_epoch, true);
}

static bool string_field(
    const cJSON *root, const char *name,
    const char **out, size_t maximum, bool allow_empty)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
    size_t length = strnlen(item->valuestring, maximum + 1U);
    if (length > maximum || (!allow_empty && length == 0U)) return false;
    *out = item->valuestring;
    return true;
}

static bool level_field(
    const cJSON *root, argus_security_level_t *out)
{
    const char *value;
    if (!string_field(root, "level", &value, 20U, false)) return false;
    if (strcmp(value, "argus_personnel") == 0) {
        *out = ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL;
    } else if (strcmp(value, "client_admin") == 0) {
        *out = ARGUS_SECURITY_LEVEL_CLIENT_ADMIN;
    } else if (strcmp(value, "supervisor") == 0) {
        *out = ARGUS_SECURITY_LEVEL_SUPERVISOR;
    } else if (strcmp(value, "operator") == 0) {
        *out = ARGUS_SECURITY_LEVEL_OPERATOR;
    } else if (strcmp(value, "viewer") == 0) {
        *out = ARGUS_SECURITY_LEVEL_VIEWER;
    } else {
        return false;
    }
    return true;
}

static esp_err_t accounts_get(httpd_req_t *req)
{
    argus_http_security_context_t security;
    if (!require_access(
            req, ARGUS_PERMISSION_MANAGE_USERS, false, &security)) {
        return ESP_OK;
    }
    argus_security_directory_snapshot_t *directory =
        directory_snapshot_alloc();
    if (directory == NULL) {
        memset(&security, 0, sizeof(security));
        return send_json(
            req, "503 Service Unavailable",
            "{\"ok\":false,\"error\":\"security_store_unavailable\"}");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *accounts = cJSON_AddArrayToObject(root, "accounts");
    for (size_t i = 0U; i < directory->payload.human_count; ++i) {
        const argus_security_human_record_t *human =
            &directory->payload.humans[i];
        if (argus_authorization_manage_target(
                &security.principal,
                (argus_security_level_t)human->level,
                human->scope, human->protected_identity != 0U) !=
            ARGUS_AUTHZ_ALLOW) {
            continue;
        }
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", human->identifier);
        cJSON_AddStringToObject(entry, "username", human->login);
        cJSON_AddStringToObject(entry, "display_name", human->display_name);
        cJSON_AddStringToObject(entry, "scope", human->scope);
        cJSON_AddNumberToObject(entry, "level", human->level);
        cJSON_AddBoolToObject(entry, "enabled", human->enabled != 0U);
        cJSON_AddBoolToObject(
            entry, "protected", human->protected_identity != 0U);
        cJSON_AddItemToArray(accounts, entry);
    }
    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    directory_snapshot_free(directory);
    memset(&security, 0, sizeof(security));
    if (rendered == NULL ||
        strnlen(rendered, SECURITY_RESPONSE_MAX) >= SECURITY_RESPONSE_MAX) {
        free(rendered);
        return send_json(
            req, "500 Internal Server Error",
            "{\"ok\":false,\"error\":\"response_overflow\"}");
    }
    set_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, rendered);
    memset(rendered, 0, strlen(rendered));
    free(rendered);
    return err;
}

static esp_err_t accounts_post(httpd_req_t *req)
{
    argus_http_security_context_t security;
    if (!require_access(
            req, ARGUS_PERMISSION_MANAGE_USERS, true, &security)) {
        return ESP_OK;
    }
    cJSON *root = receive_object(req);
    static const char *const KEYS[] = {
        "id", "username", "display_name", "scope",
        "level", "permissions", "password",
    };
    const char *id;
    const char *username;
    const char *display;
    const char *scope;
    const char *password;
    argus_security_level_t level;
    argus_permission_set_t permissions;
    bool valid = root != NULL &&
        object_keys(root, KEYS, sizeof(KEYS) / sizeof(KEYS[0])) &&
        string_field(root, "id", &id, ARGUS_SECURITY_ID_MAX, false) &&
        string_field(
            root, "username", &username, ARGUS_SECURITY_LOGIN_MAX, false) &&
        string_field(
            root, "display_name", &display,
            ARGUS_SECURITY_DISPLAY_MAX, false) &&
        string_field(root, "scope", &scope, ARGUS_SECURITY_SCOPE_MAX, false) &&
        string_field(
            root, "password", &password, ARGUS_PASSWORD_INPUT_MAX, false) &&
        level_field(root, &level) &&
        permission_array(
            cJSON_GetObjectItemCaseSensitive(root, "permissions"),
            &permissions);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (valid) {
        esp_err_t audit_err = argus_security_audit_append(
                ARGUS_AUDIT_ACCOUNT_CHANGED,
                ARGUS_AUDIT_OUTCOME_SUCCESS,
                (uint8_t)security.principal.type,
                security.principal.identifier, id, "SOFTAP",
                "create_reserved", security.principal.security_epoch,
                true);
        if (audit_err == ESP_OK) {
            err = argus_security_admin_create_human(
                &security.principal, id, username, display, scope,
                level, permissions, (const uint8_t *)password,
                strlen(password));
        } else {
            err = ESP_FAIL;
        }
    }
    zeroize_json_string(root, "password");
    cJSON_Delete(root);
    memset(&security, 0, sizeof(security));
    if (err == ESP_OK) {
        return send_json(req, "201 Created", "{\"ok\":true}");
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        return send_json(
            req, "403 Forbidden",
            "{\"ok\":false,\"error\":\"delegation_denied\"}");
    }
    if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NO_MEM) {
        return send_json(
            req, "409 Conflict",
            "{\"ok\":false,\"error\":\"account_conflict\"}");
    }
    if (err == ESP_FAIL) {
        return send_json(
            req, "503 Service Unavailable",
            "{\"ok\":false,\"error\":\"audit_unavailable\"}");
    }
    return send_json(
        req, "400 Bad Request",
        "{\"ok\":false,\"error\":\"invalid_request\"}");
}

static esp_err_t account_action_post(httpd_req_t *req)
{
    argus_http_security_context_t security;
    if (!require_access(
            req, ARGUS_PERMISSION_MANAGE_USERS, true, &security)) {
        return ESP_OK;
    }
    cJSON *root = receive_object(req);
    const char *action = NULL;
    const char *id = NULL;
    const char *password = NULL;
    cJSON *action_item =
        root == NULL ? NULL :
        cJSON_GetObjectItemCaseSensitive(root, "action");
    cJSON *id_item =
        root == NULL ? NULL :
        cJSON_GetObjectItemCaseSensitive(root, "id");
    bool valid = cJSON_IsString(action_item) &&
                 cJSON_IsString(id_item) &&
                 action_item->valuestring != NULL &&
                 id_item->valuestring != NULL;
    if (valid) {
        action = action_item->valuestring;
        id = id_item->valuestring;
        if (strcmp(id, security.principal.identifier) == 0 &&
            (strcmp(action, "disable") == 0 ||
             strcmp(action, "delete") == 0)) {
            valid = false;
        }
    }
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (valid && strcmp(action, "enable") == 0) {
        static const char *const KEYS[] = {"action", "id"};
        valid = object_keys(root, KEYS, 2U);
        if (valid) {
            err = reserve_admin_audit(
                &security, ARGUS_AUDIT_ACCOUNT_CHANGED,
                id, "enable_reserved");
            if (err == ESP_OK) {
                err = argus_security_admin_set_enabled(
                    &security.principal, id, true);
            } else {
                err = ESP_FAIL;
            }
        }
    } else if (valid && strcmp(action, "disable") == 0) {
        static const char *const KEYS[] = {"action", "id"};
        valid = object_keys(root, KEYS, 2U);
        if (valid) {
            err = reserve_admin_audit(
                &security, ARGUS_AUDIT_ACCOUNT_CHANGED,
                id, "disable_reserved");
            if (err == ESP_OK) {
                err = argus_security_admin_set_enabled(
                    &security.principal, id, false);
            } else {
                err = ESP_FAIL;
            }
        }
    } else if (valid && strcmp(action, "delete") == 0) {
        static const char *const KEYS[] = {"action", "id"};
        valid = object_keys(root, KEYS, 2U);
        if (valid) {
            err = reserve_admin_audit(
                &security, ARGUS_AUDIT_ACCOUNT_CHANGED,
                id, "delete_reserved");
            if (err == ESP_OK) {
                err = argus_security_admin_delete_human(
                    &security.principal, id);
            } else {
                err = ESP_FAIL;
            }
        }
    } else if (valid && strcmp(action, "revoke_sessions") == 0) {
        static const char *const KEYS[] = {"action", "id"};
        valid = object_keys(root, KEYS, 2U);
        if (valid) {
            err = reserve_admin_audit(
                &security, ARGUS_AUDIT_SESSION_REVOKED,
                id, "revoke_reserved");
            if (err == ESP_OK) {
                (void)argus_session_manager_revoke_principal(id);
            } else {
                err = ESP_FAIL;
            }
        }
    } else if (valid && strcmp(action, "reset_password") == 0) {
        static const char *const KEYS[] = {"action", "id", "password"};
        valid = object_keys(root, KEYS, 3U) &&
                string_field(
                    root, "password", &password,
                    ARGUS_PASSWORD_INPUT_MAX, false);
        if (valid) {
            err = reserve_admin_audit(
                &security, ARGUS_AUDIT_PASSWORD_CHANGED,
                id, "reset_reserved");
            if (err == ESP_OK) {
                err = argus_security_admin_replace_password(
                    &security.principal, id, (const uint8_t *)password,
                    strlen(password));
            } else {
                err = ESP_FAIL;
            }
        }
    } else if (valid && strcmp(action, "update_display") == 0) {
        static const char *const KEYS[] = {
            "action", "id", "display_name",
        };
        const char *display_name = NULL;
        valid = object_keys(root, KEYS, 3U) &&
                string_field(
                    root, "display_name", &display_name,
                    ARGUS_SECURITY_DISPLAY_MAX, false);
        if (valid) {
            err = reserve_admin_audit(
                &security, ARGUS_AUDIT_ACCOUNT_CHANGED,
                id, "display_reserved");
            if (err == ESP_OK) {
                err = argus_security_admin_update_display_name(
                    &security.principal, id, display_name);
            } else {
                err = ESP_FAIL;
            }
        }
    } else if (valid && strcmp(action, "assign_roles") == 0) {
        static const char *const KEYS[] = {"action", "id", "roles"};
        uint16_t role_mask = 0U;
        valid = object_keys(root, KEYS, 3U) &&
                custom_role_mask_field(root, "roles", &role_mask);
        if (valid) {
            err = reserve_admin_audit(
                &security, ARGUS_AUDIT_ACCOUNT_CHANGED,
                id, "roles_reserved");
            if (err == ESP_OK) {
                err = argus_security_admin_assign_custom_roles(
                    &security.principal, id, role_mask);
            } else {
                err = ESP_FAIL;
            }
        }
    } else {
        valid = false;
    }
    zeroize_json_string(root, "password");
    cJSON_Delete(root);
    memset(&security, 0, sizeof(security));
    if (!valid) {
        return send_json(
            req, "400 Bad Request",
            "{\"ok\":false,\"error\":\"invalid_request\"}");
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        return send_json(
            req, "403 Forbidden",
            "{\"ok\":false,\"error\":\"target_denied\"}");
    }
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            return send_json(
                req, "503 Service Unavailable",
                "{\"ok\":false,\"error\":\"audit_unavailable\"}");
        }
        return send_json(
            req, "409 Conflict",
            "{\"ok\":false,\"error\":\"account_conflict\"}");
    }
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t roles_get(httpd_req_t *req)
{
    argus_http_security_context_t security;
    if (!require_access(
            req, ARGUS_PERMISSION_MANAGE_ROLES, false, &security)) {
        return ESP_OK;
    }
    argus_security_directory_snapshot_t *directory =
        directory_snapshot_alloc();
    if (directory == NULL) {
        return send_json(
            req, "503 Service Unavailable",
            "{\"ok\":false,\"error\":\"security_store_unavailable\"}");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *roles = cJSON_AddArrayToObject(root, "custom_roles");
    for (size_t i = 0U; i < directory->payload.custom_role_count; ++i) {
        const argus_security_role_record_t *role =
            &directory->payload.custom_roles[i];
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", role->identifier);
        cJSON_AddNumberToObject(entry, "level", role->level);
        cJSON_AddItemToArray(roles, entry);
    }
    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    directory_snapshot_free(directory);
    memset(&security, 0, sizeof(security));
    if (rendered == NULL) {
        return send_json(
            req, "500 Internal Server Error",
            "{\"ok\":false,\"error\":\"internal_error\"}");
    }
    set_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, rendered);
    free(rendered);
    return err;
}

static esp_err_t roles_post(httpd_req_t *req)
{
    argus_http_security_context_t security;
    if (!require_access(
            req, ARGUS_PERMISSION_MANAGE_ROLES, true, &security)) {
        return ESP_OK;
    }
    cJSON *root = receive_object(req);
    cJSON *action =
        root == NULL ? NULL :
        cJSON_GetObjectItemCaseSensitive(root, "action");
    if (cJSON_IsString(action) && action->valuestring != NULL) {
        static const char *const DELETE_KEYS[] = {"action", "id"};
        const char *id = NULL;
        bool valid = strcmp(action->valuestring, "delete") == 0 &&
            object_keys(root, DELETE_KEYS, 2U) &&
            string_field(root, "id", &id, ARGUS_SECURITY_ID_MAX, false);
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (valid) {
            err = reserve_admin_audit(
                &security, ARGUS_AUDIT_ROLE_CHANGED,
                id, "delete_reserved");
            if (err == ESP_OK) {
                err = argus_security_admin_delete_role(
                    &security.principal, id);
            } else {
                err = ESP_FAIL;
            }
        }
        cJSON_Delete(root);
        memset(&security, 0, sizeof(security));
        if (err == ESP_OK) {
            return send_json(req, "200 OK", "{\"ok\":true}");
        }
        if (err == ESP_ERR_INVALID_STATE) {
            return send_json(
                req, "409 Conflict",
                "{\"ok\":false,\"error\":\"role_in_use\"}");
        }
        if (err == ESP_ERR_NOT_ALLOWED) {
            return send_json(
                req, "403 Forbidden",
                "{\"ok\":false,\"error\":\"delegation_denied\"}");
        }
        if (err == ESP_FAIL) {
            return send_json(
                req, "503 Service Unavailable",
                "{\"ok\":false,\"error\":\"audit_unavailable\"}");
        }
        return send_json(
            req, "400 Bad Request",
            "{\"ok\":false,\"error\":\"invalid_request\"}");
    }
    static const char *const KEYS[] = {
        "id", "level", "permissions", "delegable_permissions",
    };
    const char *id;
    argus_security_level_t level;
    argus_permission_set_t permissions;
    argus_permission_set_t delegable;
    bool valid = root != NULL && object_keys(root, KEYS, 4U) &&
        string_field(root, "id", &id, ARGUS_SECURITY_ID_MAX, false) &&
        level_field(root, &level) &&
        permission_array(
            cJSON_GetObjectItemCaseSensitive(root, "permissions"),
            &permissions) &&
        permission_array(
            cJSON_GetObjectItemCaseSensitive(
                root, "delegable_permissions"), &delegable);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (valid) {
        err = reserve_admin_audit(
            &security, ARGUS_AUDIT_ROLE_CHANGED,
            id, "create_reserved");
        if (err == ESP_OK) {
            err = argus_security_admin_create_role(
                &security.principal, id, level, permissions, delegable);
        } else {
            err = ESP_FAIL;
        }
    }
    cJSON_Delete(root);
    memset(&security, 0, sizeof(security));
    if (err == ESP_OK) return send_json(req, "201 Created", "{\"ok\":true}");
    if (err == ESP_ERR_NOT_ALLOWED) {
        return send_json(
            req, "403 Forbidden",
            "{\"ok\":false,\"error\":\"delegation_denied\"}");
    }
    if (err == ESP_FAIL) {
        return send_json(
            req, "503 Service Unavailable",
            "{\"ok\":false,\"error\":\"audit_unavailable\"}");
    }
    return send_json(
        req, "400 Bad Request",
        "{\"ok\":false,\"error\":\"invalid_request\"}");
}

static esp_err_t audit_get(httpd_req_t *req)
{
    argus_http_security_context_t security;
    if (!require_access(
            req, ARGUS_PERMISSION_VIEW_AUDIT, false, &security)) {
        return ESP_OK;
    }
    argus_security_audit_status_t status;
    if (argus_security_audit_get_status(&status) != ESP_OK) {
        return send_json(
            req, "503 Service Unavailable",
            "{\"ok\":false,\"error\":\"audit_unavailable\"}");
    }
    uint32_t start = status.count > 16U ? status.count - 16U : 0U;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", status.count);
    cJSON_AddNumberToObject(root, "overwritten", status.overwritten);
    cJSON *events = cJSON_AddArrayToObject(root, "events");
    for (uint32_t i = start; i < status.count; ++i) {
        argus_security_audit_record_t record;
        if (argus_security_audit_read(i, &record) != ESP_OK) break;
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "sequence", (double)record.sequence);
        cJSON_AddNumberToObject(entry, "event_type", record.event_type);
        cJSON_AddNumberToObject(entry, "outcome", record.outcome);
        cJSON_AddStringToObject(entry, "actor", record.actor);
        cJSON_AddStringToObject(entry, "target", record.target);
        cJSON_AddStringToObject(entry, "source", record.source);
        cJSON_AddStringToObject(entry, "reason", record.reason);
        cJSON_AddNumberToObject(
            entry, "uptime_ms", (double)(record.uptime_us / 1000U));
        cJSON_AddItemToArray(events, entry);
        memset(&record, 0, sizeof(record));
    }
    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    memset(&security, 0, sizeof(security));
    if (rendered == NULL ||
        strnlen(rendered, SECURITY_RESPONSE_MAX) >= SECURITY_RESPONSE_MAX) {
        free(rendered);
        return send_json(
            req, "500 Internal Server Error",
            "{\"ok\":false,\"error\":\"response_overflow\"}");
    }
    set_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, rendered);
    free(rendered);
    return err;
}

static void ap_apply_callback(void *ctx)
{
    (void)ctx;
    (void)argus_net_mgr_request_active_ap_secret_apply();
}

static void recovery_callback(void *ctx)
{
    (void)ctx;
    (void)argus_net_mgr_request_restart();
}

static esp_err_t ap_password_post(httpd_req_t *req)
{
    argus_http_security_context_t security;
    argus_permission_set_t required =
        ARGUS_PERMISSION_MANAGE_NETWORK |
        ARGUS_PERMISSION_CHANGE_AP_SECRET;
    if (!require_access(req, required, true, &security)) return ESP_OK;
    if (!argus_session_manager_recently_reauthenticated(
            security.session_index)) {
        memset(&security, 0, sizeof(security));
        return send_json(
            req, "403 Forbidden",
            "{\"ok\":false,\"error\":\"recent_reauthentication_required\"}");
    }
    cJSON *root = receive_object(req);
    static const char *const KEYS[] = {
        "current_password", "new_password", "confirm_password",
    };
    const char *current = NULL;
    const char *replacement = NULL;
    const char *confirmation = NULL;
    bool valid = root != NULL && object_keys(root, KEYS, 3U) &&
        string_field(
            root, "current_password", &current,
            ARGUS_SECURITY_AP_SECRET_MAX, false) &&
        string_field(
            root, "new_password", &replacement,
            ARGUS_SECURITY_AP_SECRET_MAX, false) &&
        string_field(
            root, "confirm_password", &confirmation,
            ARGUS_SECURITY_AP_SECRET_MAX, false) &&
        strcmp(replacement, confirmation) == 0;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (valid) {
        esp_err_t audit_err = argus_security_audit_append(
            ARGUS_AUDIT_AP_SECRET_CHANGED,
            ARGUS_AUDIT_OUTCOME_SUCCESS,
            (uint8_t)security.principal.type,
            security.principal.identifier, "active_ap", "SOFTAP",
            "change_reserved", security.principal.security_epoch,
            true);
        err = audit_err == ESP_OK
            ? argus_security_store_replace_active_ap_secret(
                  (const uint8_t *)current, strlen(current),
                  (const uint8_t *)replacement, strlen(replacement))
            : ESP_FAIL;
    }
    zeroize_json_string(root, "current_password");
    zeroize_json_string(root, "new_password");
    zeroize_json_string(root, "confirm_password");
    cJSON_Delete(root);
    memset(&security, 0, sizeof(security));
    if (err != ESP_OK) {
        return send_json(
            req, err == ESP_FAIL
                     ? "503 Service Unavailable"
                     : err == ESP_ERR_INVALID_STATE
                           ? "403 Forbidden" : "400 Bad Request",
            "{\"ok\":false,\"error\":\"ap_password_change_rejected\"}");
    }
    (void)argus_session_manager_revoke_all();
    if (esp_timer_start_once(s_ap_apply_timer, 300000U) != ESP_OK) {
        return send_json(
            req, "202 Accepted",
            "{\"ok\":true,\"status\":\"ap_password_changed\","
            "\"reconnect_required\":true,\"manual_restart_required\":true}");
    }
    return send_json(
        req, "202 Accepted",
        "{\"ok\":true,\"status\":\"ap_password_change_accepted\","
        "\"reconnect_required\":true}");
}

static esp_err_t recovery_exit_post(httpd_req_t *req)
{
    argus_http_security_context_t security;
    if (!require_access(
            req, ARGUS_PERMISSION_INVOKE_RECOVERY, true, &security)) {
        return ESP_OK;
    }
    if (!argus_session_manager_recently_reauthenticated(
            security.session_index)) {
        memset(&security, 0, sizeof(security));
        return send_json(
            req, "403 Forbidden",
            "{\"ok\":false,\"error\":\"recent_reauthentication_required\"}");
    }
    argus_security_store_status_t status;
    if (argus_security_store_get_status(&status) != ESP_OK) {
        memset(&security, 0, sizeof(security));
        return send_json(
            req, "503 Service Unavailable",
            "{\"ok\":false,\"error\":\"security_store_unavailable\"}");
    }
    if (status.recovery_state != ARGUS_SECURITY_RECOVERY_REQUESTED) {
        memset(&security, 0, sizeof(security));
        return send_json(
            req, "409 Conflict",
            "{\"ok\":false,\"error\":\"recovery_not_active\"}");
    }
    esp_err_t err = argus_security_audit_append(
        ARGUS_AUDIT_RECOVERY_EXIT, ARGUS_AUDIT_OUTCOME_SUCCESS,
        (uint8_t)security.principal.type,
        security.principal.identifier, "network_recovery", "SOFTAP",
        "exit_reserved", security.principal.security_epoch, true);
    memset(&security, 0, sizeof(security));
    if (err == ESP_OK) {
        err = argus_security_store_set_recovery_state(
            ARGUS_SECURITY_RECOVERY_INACTIVE);
    }
    if (err != ESP_OK) {
        return send_json(
            req, "503 Service Unavailable",
            "{\"ok\":false,\"error\":\"recovery_exit_failed\"}");
    }
    (void)argus_session_manager_revoke_all();
    if (esp_timer_start_once(s_recovery_timer, 300000U) != ESP_OK) {
        return send_json(
            req, "202 Accepted",
            "{\"ok\":true,\"status\":\"recovery_exit_committed\","
            "\"manual_restart_required\":true}");
    }
    return send_json(
        req, "202 Accepted",
        "{\"ok\":true,\"status\":\"recovery_exit_accepted\"}");
}

static const httpd_uri_t URI_ACCOUNTS_GET = {
    .uri = "/api/security/accounts", .method = HTTP_GET,
    .handler = accounts_get,
};
static const httpd_uri_t URI_ACCOUNTS_POST = {
    .uri = "/api/security/accounts", .method = HTTP_POST,
    .handler = accounts_post,
};
static const httpd_uri_t URI_ACCOUNT_ACTION = {
    .uri = "/api/security/accounts/action", .method = HTTP_POST,
    .handler = account_action_post,
};
static const httpd_uri_t URI_ROLES_GET = {
    .uri = "/api/security/roles", .method = HTTP_GET,
    .handler = roles_get,
};
static const httpd_uri_t URI_ROLES_POST = {
    .uri = "/api/security/roles", .method = HTTP_POST,
    .handler = roles_post,
};
static const httpd_uri_t URI_AUDIT_GET = {
    .uri = "/api/security/audit", .method = HTTP_GET,
    .handler = audit_get,
};
static const httpd_uri_t URI_AP_PASSWORD = {
    .uri = "/api/security/ap-password", .method = HTTP_POST,
    .handler = ap_password_post,
};
static const httpd_uri_t URI_RECOVERY_EXIT = {
    .uri = "/api/security/recovery/exit", .method = HTTP_POST,
    .handler = recovery_exit_post,
};

esp_err_t argus_security_http_init(void)
{
    if (s_ap_apply_timer != NULL || s_recovery_timer != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_timer_create_args_t ap_args = {
        .callback = ap_apply_callback,
        .name = "argus_ap_apply",
    };
    esp_timer_create_args_t recovery_args = {
        .callback = recovery_callback,
        .name = "argus_recovery_exit",
    };
    esp_err_t err = esp_timer_create(&ap_args, &s_ap_apply_timer);
    if (err == ESP_OK) {
        err = esp_timer_create(&recovery_args, &s_recovery_timer);
    }
    return err;
}

esp_err_t argus_security_http_register(httpd_handle_t server)
{
    if (server == NULL || s_ap_apply_timer == NULL ||
        s_recovery_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const httpd_uri_t *routes[] = {
        &URI_ACCOUNTS_GET, &URI_ACCOUNTS_POST, &URI_ACCOUNT_ACTION,
        &URI_ROLES_GET, &URI_ROLES_POST, &URI_AUDIT_GET,
        &URI_AP_PASSWORD, &URI_RECOVERY_EXIT,
    };
    for (size_t i = 0U; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
size_t argus_security_http_test_route_count(void)
{
    return 8U;
}
#endif
