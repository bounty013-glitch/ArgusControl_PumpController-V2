#include "argus_mqtt_security.h"

#include <string.h>

static bool approved_read_filter(
    const argus_mqtt_topics_t *topics, const char *filter)
{
    if (topics == NULL || filter == NULL) return false;
    size_t root_length = strlen(topics->root);
    if (strncmp(filter, topics->root, root_length) != 0 ||
        filter[root_length] != '/') {
        return false;
    }
    const char *leaf = filter + root_length + 1U;
    static const char *const categories[] = {
        "metadata", "state", "status", "telemetry",
    };
    for (size_t i = 0U; i < sizeof(categories) / sizeof(categories[0]); ++i) {
        size_t length = strlen(categories[i]);
        if (strncmp(leaf, categories[i], length) == 0 &&
            leaf[length] == '/') {
            const char *remainder = leaf + length + 1U;
            if (strcmp(remainder, "#") == 0) return true;
            return strchr(remainder, '#') == NULL &&
                   strchr(remainder, '+') == NULL &&
                   remainder[0] != '\0';
        }
    }
    return strcmp(filter, topics->command_result) == 0;
}

bool argus_mqtt_security_subscription_allowed(
    const argus_mqtt_topics_t *topics,
    const argus_machine_principal_t *principal,
    const char *filter)
{
    return topics != NULL && principal != NULL && filter != NULL &&
           (principal->permissions & ARGUS_PERMISSION_VIEW_STATUS) != 0U &&
           approved_read_filter(topics, filter) &&
           argus_machine_service_topic_scope_contains(
               principal->topic_scope, filter);
}

argus_permission_set_t argus_mqtt_security_required_permission(
    const argus_mqtt_topics_t *topics, const char *topic)
{
    if (topics == NULL || topic == NULL) return 0U;
    switch (argus_mqtt_topics_classify(topics, topic)) {
        case ARGUS_MQTT_ACTION_HEARTBEAT:
            return ARGUS_PERMISSION_REQUEST_AUTHORITY;
        case ARGUS_MQTT_ACTION_E_STOP:
            return ARGUS_PERMISSION_SOFTWARE_ESTOP;
        case ARGUS_MQTT_ACTION_RESET_E_STOP:
            return ARGUS_PERMISSION_RESET_SOFTWARE_ESTOP;
        case ARGUS_MQTT_ACTION_SET_TARGET:
        case ARGUS_MQTT_ACTION_START:
        case ARGUS_MQTT_ACTION_STOP:
        case ARGUS_MQTT_ACTION_UNLOCK:
        case ARGUS_MQTT_ACTION_RECOVER:
            return ARGUS_PERMISSION_MOTION;
        default:
            return 0U;
    }
}

bool argus_mqtt_security_publish_allowed(
    const argus_mqtt_topics_t *topics,
    const argus_machine_principal_t *principal,
    const char *topic)
{
    if (topics == NULL || principal == NULL || topic == NULL ||
        !argus_machine_service_topic_scope_contains(
            principal->topic_scope, topic)) {
        return false;
    }
    argus_permission_set_t required =
        argus_mqtt_security_required_permission(topics, topic);
    return required != 0U &&
           (principal->permissions & required) == required;
}
