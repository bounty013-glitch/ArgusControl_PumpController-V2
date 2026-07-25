#ifndef ARGUS_MQTT_INVALIDATION_H
#define ARGUS_MQTT_INVALIDATION_H

#include <stdbool.h>
#include <stdint.h>

#include "argus_security_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_MQTT_INVALIDATION_HISTORY_CAPACITY 32U

typedef struct {
    uint64_t generation;
    char identifier[ARGUS_SECURITY_ID_MAX + 1U];
} argus_mqtt_invalidation_event_t;

typedef struct {
    uint64_t generation;
    argus_mqtt_invalidation_event_t
        events[ARGUS_MQTT_INVALIDATION_HISTORY_CAPACITY];
} argus_mqtt_invalidation_journal_t;

void argus_mqtt_invalidation_init(
    argus_mqtt_invalidation_journal_t *journal);
uint64_t argus_mqtt_invalidation_capture(
    const argus_mqtt_invalidation_journal_t *journal);
bool argus_mqtt_invalidation_record(
    argus_mqtt_invalidation_journal_t *journal,
    const char *identifier);
bool argus_mqtt_invalidation_since(
    const argus_mqtt_invalidation_journal_t *journal,
    const char *identifier,
    uint64_t captured_generation);

#ifdef __cplusplus
}
#endif

#endif
