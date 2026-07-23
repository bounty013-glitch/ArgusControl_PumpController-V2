#ifndef ARGUS_HTTP_ROUTE_INVENTORY_H
#define ARGUS_HTTP_ROUTE_INVENTORY_H

#include <stdbool.h>
#include <stddef.h>

#include "argus_authorization.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARGUS_HTTP_ROUTE_PUBLIC = 1,
    ARGUS_HTTP_ROUTE_AUTHENTICATED,
    ARGUS_HTTP_ROUTE_ADMINISTRATIVE,
    ARGUS_HTTP_ROUTE_BROWSER_COMMAND,
    ARGUS_HTTP_ROUTE_RETIRED,
} argus_http_route_class_t;

typedef struct {
    const char *path;
    httpd_method_t method;
    argus_http_route_class_t route_class;
    argus_permission_set_t capabilities;
    bool capability_any;
    bool registered;
    bool ap_only;
    bool authentication;
    bool csrf;
    bool strict_json;
    bool security_mutation;
    bool browser_command_adapter;
    bool no_store;
} argus_http_route_inventory_entry_t;

const argus_http_route_inventory_entry_t *
argus_http_route_inventory(size_t *count);
bool argus_http_route_inventory_validate(void);

#ifdef __cplusplus
}
#endif

#endif
