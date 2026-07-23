#ifndef ARGUS_SECURITY_HTTP_H
#define ARGUS_SECURITY_HTTP_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t argus_security_http_init(void);
esp_err_t argus_security_http_register(httpd_handle_t server);

#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
size_t argus_security_http_test_route_count(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
