#ifndef ARGUS_MACHINE_DIRECTORY_H
#define ARGUS_MACHINE_DIRECTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "argus_security_store.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_MACHINE_DIRECTORY_SCHEMA_VERSION 1U
#define ARGUS_MACHINE_DIRECTORY_MAGIC UINT32_C(0x41524d34)
#define ARGUS_MACHINE_DIRECTORY_VALID UINT32_C(0x4d434c54)

typedef struct {
    uint16_t schema_version;
    uint8_t machine_count;
    uint8_t reserved;
    argus_security_machine_record_t machines[ARGUS_SECURITY_MAX_MACHINES];
} argus_machine_directory_payload_t;

typedef struct {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t payload_length;
    uint32_t generation;
    uint32_t crc32;
    uint32_t valid_marker;
    argus_machine_directory_payload_t payload;
} argus_machine_directory_slot_t;

typedef struct {
    argus_machine_directory_payload_t payload;
    uint32_t generation;
} argus_machine_directory_snapshot_t;

typedef enum {
    ARGUS_MACHINE_DIRECTORY_MISSING = 0,
    ARGUS_MACHINE_DIRECTORY_READY,
    ARGUS_MACHINE_DIRECTORY_CORRUPT,
    ARGUS_MACHINE_DIRECTORY_UNSUPPORTED,
    ARGUS_MACHINE_DIRECTORY_UNAVAILABLE,
} argus_machine_directory_state_t;

typedef struct {
    argus_machine_directory_state_t state;
    uint16_t schema_version;
    uint8_t machine_count;
    uint32_t generation;
    bool redundancy_degraded;
} argus_machine_directory_status_t;

uint32_t argus_machine_directory_crc32(
    const argus_machine_directory_payload_t *payload);
bool argus_machine_directory_payload_valid(
    const argus_machine_directory_payload_t *payload);
bool argus_machine_directory_slot_valid(
    const argus_machine_directory_slot_t *slot);
bool argus_machine_directory_commit_precondition(
    const argus_machine_directory_payload_t *payload,
    uint32_t expected_generation, uint32_t current_generation,
    bool directory_ready);

esp_err_t argus_machine_directory_select_for_test(
    const argus_machine_directory_slot_t *slot_a, esp_err_t slot_a_status,
    const argus_machine_directory_slot_t *slot_b, esp_err_t slot_b_status,
    uint8_t selector, esp_err_t selector_status,
    argus_machine_directory_snapshot_t *out, uint8_t *out_active_slot,
    bool *out_selector_repair);

esp_err_t argus_machine_directory_init(void);
esp_err_t argus_machine_directory_get_snapshot(
    argus_machine_directory_snapshot_t *out);
esp_err_t argus_machine_directory_get_status(
    argus_machine_directory_status_t *out);
esp_err_t argus_machine_directory_commit(
    const argus_machine_directory_payload_t *payload,
    uint32_t expected_generation);
esp_err_t argus_machine_directory_find(
    const char *identifier, argus_security_machine_record_t *out,
    size_t *out_index, uint32_t *out_directory_generation);

#ifdef __cplusplus
}
#endif

#endif
