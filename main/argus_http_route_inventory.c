#include "argus_http_route_inventory.h"

#include <string.h>

#define ROUTE(path_, method_, class_, caps_, any_, registered_, auth_, csrf_, \
              json_, security_mutation_, command_)                         \
    {                                                                      \
        .path = (path_), .method = (method_), .route_class = (class_),     \
        .capabilities = (caps_), .capability_any = (any_),                 \
        .registered = (registered_), .ap_only = true,                      \
        .authentication = (auth_), .csrf = (csrf_),                        \
        .strict_json = (json_),                                            \
        .security_mutation = (security_mutation_),                         \
        .browser_command_adapter = (command_), .no_store = true,           \
    }

static const argus_permission_set_t ADMIN_PAGE_CAPABILITIES =
    ARGUS_PERMISSION_MANAGE_USERS |
    ARGUS_PERMISSION_MANAGE_ROLES |
    ARGUS_PERMISSION_ENROLL_MACHINES |
    ARGUS_PERMISSION_REVOKE_MACHINES |
    ARGUS_PERMISSION_VIEW_AUDIT |
    ARGUS_PERMISSION_MANAGE_NETWORK |
    ARGUS_PERMISSION_CHANGE_AP_SECRET |
    ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK |
    ARGUS_PERMISSION_MODIFY_IDENTITY |
    ARGUS_PERMISSION_MODIFY_PROTECTED_CONFIG |
    ARGUS_PERMISSION_COMMISSION |
    ARGUS_PERMISSION_CALIBRATE |
    ARGUS_PERMISSION_MANAGE_FIRMWARE |
    ARGUS_PERMISSION_INVOKE_RECOVERY |
    ARGUS_PERMISSION_FULL_SECURITY_RESET;

static const argus_permission_set_t BROWSER_COMMAND_CAPABILITIES =
    ARGUS_PERMISSION_MOTION |
    ARGUS_PERMISSION_SOFTWARE_ESTOP |
    ARGUS_PERMISSION_RESET_SOFTWARE_ESTOP;

static const argus_http_route_inventory_entry_t ROUTES[] = {
    ROUTE("/login", HTTP_GET, ARGUS_HTTP_ROUTE_PUBLIC, 0U, false,
          true, false, false, false, false, false),
    ROUTE("/api/auth/login", HTTP_POST, ARGUS_HTTP_ROUTE_PUBLIC, 0U, false,
          true, false, false, true, false, false),
    ROUTE("/api/auth/session", HTTP_GET, ARGUS_HTTP_ROUTE_AUTHENTICATED,
          0U, false, true, true, false, false, false, false),
    ROUTE("/api/auth/logout", HTTP_POST, ARGUS_HTTP_ROUTE_AUTHENTICATED,
          0U, false, true, true, true, true, true, false),
    ROUTE("/api/auth/reauth", HTTP_POST, ARGUS_HTTP_ROUTE_AUTHENTICATED,
          0U, false, true, true, true, true, true, false),
    ROUTE("/api/auth/change-password", HTTP_POST,
          ARGUS_HTTP_ROUTE_AUTHENTICATED, 0U, false, true, true, true,
          true, true, false),
    ROUTE("/operate", HTTP_GET, ARGUS_HTTP_ROUTE_AUTHENTICATED,
          ARGUS_PERMISSION_VIEW_STATUS, false, true, true, false,
          false, false, false),
    ROUTE("/commission", HTTP_GET, ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ADMIN_PAGE_CAPABILITIES, true, true, true, false,
          false, false, false),
    ROUTE("/api/status", HTTP_GET, ARGUS_HTTP_ROUTE_AUTHENTICATED,
          ARGUS_PERMISSION_VIEW_STATUS, false, true, true, false,
          false, false, false),
    ROUTE("/api/identity", HTTP_GET, ARGUS_HTTP_ROUTE_AUTHENTICATED,
          ARGUS_PERMISSION_VIEW_STATUS, false, true, true, false,
          false, false, false),
    ROUTE("/", HTTP_GET, ARGUS_HTTP_ROUTE_PUBLIC, 0U, false,
          true, false, false, false, false, false),
    ROUTE("/controls", HTTP_GET, ARGUS_HTTP_ROUTE_AUTHENTICATED,
          ARGUS_PERMISSION_VIEW_STATUS, false, true, true, false,
          false, false, false),
    ROUTE("/change-password", HTTP_GET, ARGUS_HTTP_ROUTE_AUTHENTICATED,
          0U, false, true, true, false, false, false, false),
    ROUTE("/api/config", HTTP_GET, ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_MODIFY_IDENTITY |
              ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK |
              ARGUS_PERMISSION_MODIFY_PROTECTED_CONFIG,
          true, true, true, false, false, false, false),
    ROUTE("/api/config/save", HTTP_POST, ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_MODIFY_IDENTITY |
              ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK,
          true, true, true, true, true, true, false),
    ROUTE("/config/identity", HTTP_GET, ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_MODIFY_IDENTITY, false, true, true, false,
          false, false, false),
    ROUTE("/config/wifi", HTTP_GET, ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK, false, true, true,
          false, false, false, false),
    ROUTE("/api/network/reconnect", HTTP_POST,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK, false, true, true,
          true, true, false, false),
    ROUTE("/api/restart", HTTP_POST, ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_MANAGE_FIRMWARE, false, true, true, true,
          true, false, false),
    ROUTE("/api/service/enter", HTTP_POST,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_REQUEST_AUTHORITY, false, true, true, true,
          true, false, false),
    ROUTE("/api/service/exit", HTTP_POST,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_REQUEST_AUTHORITY, false, true, true, true,
          true, false, false),
    ROUTE("/api/command", HTTP_POST, ARGUS_HTTP_ROUTE_BROWSER_COMMAND,
          BROWSER_COMMAND_CAPABILITIES, true, true, true, true, true,
          false, true),
    ROUTE("/api/factory-reset", HTTP_POST,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE, ARGUS_PERMISSION_COMMISSION,
          false, true, true, true, true, true, false),
    ROUTE("/api/security/accounts", HTTP_GET,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_MANAGE_USERS, false, true, true, false,
          false, false, false),
    ROUTE("/api/security/accounts", HTTP_POST,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_MANAGE_USERS, false, true, true, true,
          true, true, false),
    ROUTE("/api/security/accounts/action", HTTP_POST,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_MANAGE_USERS, false, true, true, true,
          true, true, false),
    ROUTE("/api/security/roles", HTTP_GET,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_MANAGE_ROLES, false, true, true, false,
          false, false, false),
    ROUTE("/api/security/roles", HTTP_POST,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_MANAGE_ROLES, false, true, true, true,
          true, true, false),
    ROUTE("/api/security/audit", HTTP_GET,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_VIEW_AUDIT, false, true, true, false,
          false, false, false),
    ROUTE("/api/security/ap-password", HTTP_POST,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_MANAGE_NETWORK |
              ARGUS_PERMISSION_CHANGE_AP_SECRET,
          false, true, true, true, true, true, false),
    ROUTE("/api/security/recovery/exit", HTTP_POST,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_INVOKE_RECOVERY, false, true, true, true,
          true, true, false),
    ROUTE("/api/security/machines", HTTP_GET,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_ENROLL_MACHINES |
              ARGUS_PERMISSION_REVOKE_MACHINES,
          true, true, true, false,
          false, false, false),
    ROUTE("/api/security/machines", HTTP_POST,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_ENROLL_MACHINES, false, true, true, true,
          true, true, false),
    ROUTE("/api/security/machines/action", HTTP_POST,
          ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
          ARGUS_PERMISSION_ENROLL_MACHINES |
              ARGUS_PERMISSION_REVOKE_MACHINES,
          true, true, true, true, true, true, false),
    ROUTE("/api/logout", HTTP_GET, ARGUS_HTTP_ROUTE_RETIRED, 0U, false,
          false, false, false, false, false, false),
};

const argus_http_route_inventory_entry_t *
argus_http_route_inventory(size_t *count)
{
    if (count != NULL) *count = sizeof(ROUTES) / sizeof(ROUTES[0]);
    return ROUTES;
}

bool argus_http_route_inventory_validate(void)
{
    const size_t count = sizeof(ROUTES) / sizeof(ROUTES[0]);
    for (size_t i = 0U; i < count; ++i) {
        const argus_http_route_inventory_entry_t *route = &ROUTES[i];
        if (route->path == NULL || route->path[0] != '/' ||
            !route->ap_only || !route->no_store) {
            return false;
        }
        if (route->registered ==
            (route->route_class == ARGUS_HTTP_ROUTE_RETIRED)) {
            return false;
        }
        if (route->security_mutation &&
            (route->method == HTTP_GET || route->method == HTTP_HEAD ||
             !route->authentication || !route->csrf ||
             !route->strict_json)) {
            return false;
        }
        if ((route->route_class == ARGUS_HTTP_ROUTE_ADMINISTRATIVE ||
             route->route_class == ARGUS_HTTP_ROUTE_BROWSER_COMMAND) &&
            route->capabilities == 0U) {
            return false;
        }
        if (route->browser_command_adapter !=
            (route->route_class == ARGUS_HTTP_ROUTE_BROWSER_COMMAND)) {
            return false;
        }
        for (size_t j = i + 1U; j < count; ++j) {
            if (route->method == ROUTES[j].method &&
                strcmp(route->path, ROUTES[j].path) == 0) {
                return false;
            }
        }
    }
    return true;
}
