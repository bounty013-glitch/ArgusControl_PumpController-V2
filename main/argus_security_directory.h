#ifndef ARGUS_SECURITY_DIRECTORY_H
#define ARGUS_SECURITY_DIRECTORY_H

#include <stddef.h>
#include <stdint.h>

#include "argus_security_store.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_SECURITY_DIRECTORY_SCHEMA_VERSION 1U
#define ARGUS_SECURITY_CUSTOM_ROLE_CAPACITY \
    (ARGUS_SECURITY_MAX_ROLES - ARGUS_SECURITY_BUILTIN_ROLE_COUNT)

typedef struct {
    uint16_t schema_version;
    uint16_t flags;
    uint8_t custom_role_count;
    uint8_t human_count;
    uint16_t reserved;
    argus_security_role_record_t
        custom_roles[ARGUS_SECURITY_CUSTOM_ROLE_CAPACITY];
    argus_security_human_record_t humans[ARGUS_SECURITY_MAX_HUMANS];
} argus_security_directory_payload_t;

typedef struct {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t payload_length;
    uint32_t generation;
    uint32_t crc32;
    uint32_t valid_marker;
    argus_security_directory_payload_t payload;
} argus_security_directory_slot_t;

typedef struct {
    argus_security_directory_payload_t payload;
    uint32_t generation;
} argus_security_directory_snapshot_t;

bool argus_security_directory_payload_valid(
    const argus_security_directory_payload_t *payload);
uint32_t argus_security_directory_crc32(
    const argus_security_directory_payload_t *payload);

esp_err_t argus_security_directory_init(void);
esp_err_t argus_security_directory_get_snapshot(
    argus_security_directory_snapshot_t *out);
esp_err_t argus_security_directory_commit(
    const argus_security_directory_payload_t *payload,
    uint32_t expected_generation);
esp_err_t argus_security_directory_find_login(
    const char *canonical_login,
    argus_security_human_record_t *out);
esp_err_t argus_security_directory_find_id(
    const char *identifier,
    argus_security_human_record_t *out,
    size_t *out_index);

#ifdef __cplusplus
}
#endif

#endif
