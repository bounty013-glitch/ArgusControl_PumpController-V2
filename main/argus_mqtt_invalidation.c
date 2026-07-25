#include "argus_mqtt_invalidation.h"

#include <string.h>

static bool identifier_valid(const char *identifier)
{
    if (identifier == NULL) return false;
    size_t length = strnlen(identifier, ARGUS_SECURITY_ID_MAX + 1U);
    return length > 0U && length <= ARGUS_SECURITY_ID_MAX;
}

void argus_mqtt_invalidation_init(
    argus_mqtt_invalidation_journal_t *journal)
{
    if (journal != NULL) memset(journal, 0, sizeof(*journal));
}

uint64_t argus_mqtt_invalidation_capture(
    const argus_mqtt_invalidation_journal_t *journal)
{
    return journal != NULL ? journal->generation : 0U;
}

bool argus_mqtt_invalidation_record(
    argus_mqtt_invalidation_journal_t *journal,
    const char *identifier)
{
    if (journal == NULL || !identifier_valid(identifier)) return false;
    journal->generation++;
    if (journal->generation == 0U) {
        memset(journal->events, 0, sizeof(journal->events));
        journal->generation = 1U;
    }
    size_t index = (size_t)(
        (journal->generation - 1U) %
        ARGUS_MQTT_INVALIDATION_HISTORY_CAPACITY);
    journal->events[index].generation = journal->generation;
    strlcpy(journal->events[index].identifier, identifier,
            sizeof(journal->events[index].identifier));
    return true;
}

bool argus_mqtt_invalidation_since(
    const argus_mqtt_invalidation_journal_t *journal,
    const char *identifier,
    uint64_t captured_generation)
{
    if (journal == NULL || !identifier_valid(identifier)) return true;
    uint64_t current = journal->generation;
    if (current == captured_generation) return false;
    if (current < captured_generation ||
        current - captured_generation >
            ARGUS_MQTT_INVALIDATION_HISTORY_CAPACITY) {
        return true;
    }
    for (uint64_t generation = captured_generation + 1U;
         generation <= current; ++generation) {
        size_t index = (size_t)(
            (generation - 1U) %
            ARGUS_MQTT_INVALIDATION_HISTORY_CAPACITY);
        const argus_mqtt_invalidation_event_t *event =
            &journal->events[index];
        if (event->generation != generation) return true;
        if (strcmp(event->identifier, identifier) == 0) return true;
    }
    return false;
}
