#ifndef ARGUS_MQTT_SECURITY_H
#define ARGUS_MQTT_SECURITY_H

#include <stdbool.h>

#include "argus_machine_service.h"
#include "argus_mqtt_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

bool argus_mqtt_security_subscription_allowed(
    const argus_mqtt_topics_t *topics,
    const argus_machine_principal_t *principal,
    const char *filter);
bool argus_mqtt_security_publish_allowed(
    const argus_mqtt_topics_t *topics,
    const argus_machine_principal_t *principal,
    const char *topic);
argus_permission_set_t argus_mqtt_security_required_permission(
    const argus_mqtt_topics_t *topics, const char *topic);

#ifdef __cplusplus
}
#endif

#endif
