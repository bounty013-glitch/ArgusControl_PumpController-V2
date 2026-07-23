#ifndef ARGUS_SECURITY_AUDIT_H
#define ARGUS_SECURITY_AUDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_AUDIT_CAPACITY 255U
#define ARGUS_AUDIT_TEXT_MAX 36U
#define ARGUS_AUDIT_PAGE_MAX 16U
#define ARGUS_AUDIT_ACTION_MAX 20U

typedef enum {
    ARGUS_AUDIT_LOGIN_SUCCESS = 1,
    ARGUS_AUDIT_LOGIN_FAILURE,
    ARGUS_AUDIT_LOGIN_THROTTLED,
    ARGUS_AUDIT_LOGOUT,
    ARGUS_AUDIT_SESSION_REVOKED,
    ARGUS_AUDIT_ACCOUNT_CHANGED,
    ARGUS_AUDIT_PASSWORD_CHANGED,
    ARGUS_AUDIT_ROLE_CHANGED,
    ARGUS_AUDIT_ADMIN_DENIED,
    ARGUS_AUDIT_AP_SECRET_CHANGED,
    ARGUS_AUDIT_RECOVERY_EXIT,
    ARGUS_AUDIT_STA_ROUTE_REJECTED,
    ARGUS_AUDIT_STORAGE_FAILURE,
} argus_audit_event_type_t;

typedef enum {
    ARGUS_AUDIT_OUTCOME_SUCCESS = 1,
    ARGUS_AUDIT_OUTCOME_REJECTED,
    ARGUS_AUDIT_OUTCOME_FAILED,
    ARGUS_AUDIT_OUTCOME_PREPARED,
} argus_audit_outcome_t;

typedef struct {
    uint16_t schema_version;
    uint16_t event_type;
    uint8_t outcome;
    uint8_t principal_type;
    uint16_t lifecycle_id;
    uint64_t sequence;
    uint64_t boot_id;
    uint64_t uptime_us;
    uint32_t security_epoch;
    char actor[ARGUS_AUDIT_TEXT_MAX + 1U];
    char target[ARGUS_AUDIT_TEXT_MAX + 1U];
    char source[16];
    char reason[ARGUS_AUDIT_TEXT_MAX + 1U];
    uint32_t crc32;
} argus_security_audit_record_t;

typedef struct {
    uint64_t next_sequence;
    uint32_t count;
    uint32_t overwritten;
    bool available;
    bool finalization_degraded;
} argus_security_audit_status_t;

typedef struct {
    argus_audit_event_type_t event_type;
    uint8_t principal_type;
    uint16_t lifecycle_id;
    uint32_t security_epoch;
    char actor[ARGUS_AUDIT_TEXT_MAX + 1U];
    char target[ARGUS_AUDIT_TEXT_MAX + 1U];
    char source[16];
    char action[ARGUS_AUDIT_ACTION_MAX + 1U];
} argus_security_audit_mutation_t;

typedef struct {
    argus_security_audit_record_t records[ARGUS_AUDIT_PAGE_MAX];
    uint64_t next_before;
    uint32_t count;
    bool has_more;
    bool corruption_gap;
} argus_security_audit_page_t;

esp_err_t argus_security_audit_init(void);
esp_err_t argus_security_audit_append(
    argus_audit_event_type_t type,
    argus_audit_outcome_t outcome,
    uint8_t principal_type,
    const char *actor,
    const char *target,
    const char *source,
    const char *reason,
    uint32_t security_epoch,
    bool required);
esp_err_t argus_security_audit_mutation_begin(
    argus_audit_event_type_t type,
    uint8_t principal_type,
    const char *actor,
    const char *target,
    const char *source,
    const char *action,
    uint32_t security_epoch,
    argus_security_audit_mutation_t *out);
esp_err_t argus_security_audit_mutation_finish(
    const argus_security_audit_mutation_t *mutation,
    bool succeeded);
esp_err_t argus_security_audit_get_status(
    argus_security_audit_status_t *out);
esp_err_t argus_security_audit_read(
    uint32_t offset_from_oldest,
    argus_security_audit_record_t *out);
esp_err_t argus_security_audit_read_page(
    uint64_t before_sequence,
    uint32_t limit,
    argus_security_audit_page_t *out);

#ifdef CONFIG_ARGUS_DIAGNOSTIC_MODE
void argus_security_audit_test_fail_next_required(void);
void argus_security_audit_test_clear_finalization_degraded(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
