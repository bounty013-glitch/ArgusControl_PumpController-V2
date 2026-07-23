#ifndef ARGUS_SECURITY_STORE_H
#define ARGUS_SECURITY_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "argus_password_verifier.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_SECURITY_SCHEMA_VERSION 1U
#define ARGUS_SECURITY_RECORD_VERSION 1U
#define ARGUS_SECURITY_SLOT_MAGIC 0x41524753U
#define ARGUS_SECURITY_VALID_MARKER 0x53454355U
#define ARGUS_SECURITY_MAX_ROLES 16U
#define ARGUS_SECURITY_BUILTIN_ROLE_COUNT 6U
#define ARGUS_SECURITY_MAX_HUMANS 16U
#define ARGUS_SECURITY_MAX_MACHINES 16U
#define ARGUS_SECURITY_ID_MAX 36U
#define ARGUS_SECURITY_LOGIN_MAX 32U
#define ARGUS_SECURITY_DISPLAY_MAX 48U
#define ARGUS_SECURITY_SCOPE_MAX 36U
#define ARGUS_SECURITY_TOPIC_SCOPE_MAX 96U
#define ARGUS_SECURITY_API_SCOPE_MAX 64U
#define ARGUS_SECURITY_AP_SECRET_MIN 8U
#define ARGUS_SECURITY_AP_SECRET_MAX 63U
#define ARGUS_SECURITY_PARTITION "sec_store"
#define ARGUS_SECURITY_KEYS_PARTITION "sec_keys"

typedef uint64_t argus_permission_set_t;

enum {
    ARGUS_PERMISSION_VIEW_STATUS = UINT64_C(1) << 0,
    ARGUS_PERMISSION_REQUEST_AUTHORITY = UINT64_C(1) << 1,
    ARGUS_PERMISSION_MOTION = UINT64_C(1) << 2,
    ARGUS_PERMISSION_SOFTWARE_ESTOP = UINT64_C(1) << 3,
    ARGUS_PERMISSION_RESET_SOFTWARE_ESTOP = UINT64_C(1) << 4,
    ARGUS_PERMISSION_ACK_ALARMS = UINT64_C(1) << 5,
    ARGUS_PERMISSION_MANAGE_USERS = UINT64_C(1) << 6,
    ARGUS_PERMISSION_MANAGE_ROLES = UINT64_C(1) << 7,
    ARGUS_PERMISSION_MANAGE_CLIENT_ADMINS = UINT64_C(1) << 8,
    ARGUS_PERMISSION_ENROLL_MACHINES = UINT64_C(1) << 9,
    ARGUS_PERMISSION_REVOKE_MACHINES = UINT64_C(1) << 10,
    ARGUS_PERMISSION_VIEW_AUDIT = UINT64_C(1) << 11,
    ARGUS_PERMISSION_MANAGE_NETWORK = UINT64_C(1) << 12,
    ARGUS_PERMISSION_CHANGE_AP_SECRET = UINT64_C(1) << 13,
    ARGUS_PERMISSION_MANAGE_CLIENT_NETWORK = UINT64_C(1) << 14,
    ARGUS_PERMISSION_MANAGE_MQTT = UINT64_C(1) << 15,
    ARGUS_PERMISSION_MODIFY_IDENTITY = UINT64_C(1) << 16,
    ARGUS_PERMISSION_MODIFY_PROTECTED_CONFIG = UINT64_C(1) << 17,
    ARGUS_PERMISSION_COMMISSION = UINT64_C(1) << 18,
    ARGUS_PERMISSION_CALIBRATE = UINT64_C(1) << 19,
    ARGUS_PERMISSION_MANAGE_FIRMWARE = UINT64_C(1) << 20,
    ARGUS_PERMISSION_INVOKE_RECOVERY = UINT64_C(1) << 21,
    ARGUS_PERMISSION_FULL_SECURITY_RESET = UINT64_C(1) << 22,
};

#define ARGUS_PERMISSION_DEFINED_MASK \
    ((UINT64_C(1) << 23) - UINT64_C(1))

typedef enum {
    ARGUS_SECURITY_LEVEL_ARGUS_PERSONNEL = 0,
    ARGUS_SECURITY_LEVEL_CLIENT_ADMIN,
    ARGUS_SECURITY_LEVEL_SUPERVISOR,
    ARGUS_SECURITY_LEVEL_OPERATOR,
    ARGUS_SECURITY_LEVEL_VIEWER,
    ARGUS_SECURITY_LEVEL_MACHINE,
    ARGUS_SECURITY_LEVEL_COUNT,
} argus_security_level_t;

typedef enum {
    ARGUS_SECURITY_STORE_MISSING = 0,
    ARGUS_SECURITY_STORE_UNPROVISIONED,
    ARGUS_SECURITY_STORE_READY,
    ARGUS_SECURITY_STORE_CORRUPT,
    ARGUS_SECURITY_STORE_UNSUPPORTED_VERSION,
    ARGUS_SECURITY_STORE_UNAVAILABLE,
} argus_security_store_state_t;

typedef enum {
    ARGUS_SECURITY_MIGRATION_NOT_STARTED = 0,
    ARGUS_SECURITY_MIGRATION_LEGACY_PENDING,
    ARGUS_SECURITY_MIGRATION_COMPLETE,
    ARGUS_SECURITY_MIGRATION_BUILD_DEFAULT_DEFERRED,
    ARGUS_SECURITY_MIGRATION_FAILED,
} argus_security_migration_state_t;

typedef enum {
    ARGUS_SECURITY_RECOVERY_INACTIVE = 0,
    ARGUS_SECURITY_RECOVERY_REQUESTED,
} argus_security_recovery_state_t;

typedef struct __attribute__((packed)) {
    uint8_t record_version;
    uint8_t level;
    uint8_t builtin;
    uint8_t protected_role;
    char identifier[ARGUS_SECURITY_ID_MAX + 1U];
    argus_permission_set_t permissions;
    argus_permission_set_t delegable_permissions;
} argus_security_role_record_t;

typedef struct __attribute__((packed)) {
    uint8_t record_version;
    uint8_t level;
    uint8_t enabled;
    uint8_t protected_identity;
    char identifier[ARGUS_SECURITY_ID_MAX + 1U];
    char login[ARGUS_SECURITY_LOGIN_MAX + 1U];
    char display_name[ARGUS_SECURITY_DISPLAY_MAX + 1U];
    char scope[ARGUS_SECURITY_SCOPE_MAX + 1U];
    uint16_t role_mask;
    argus_permission_set_t direct_permissions;
    uint32_t credential_version;
    uint32_t record_security_epoch;
    uint8_t revoked;
    uint8_t reserved[3];
    argus_password_verifier_t verifier;
} argus_security_human_record_t;

typedef enum {
    ARGUS_MACHINE_CLIENT_HMI = 1,
    ARGUS_MACHINE_CLIENT_NODE_RED,
    ARGUS_MACHINE_CLIENT_SERVICE_TOOL,
    ARGUS_MACHINE_CLIENT_BACKUP_INTERFACE,
    ARGUS_MACHINE_CLIENT_ARGUS_COMMAND,
    ARGUS_MACHINE_CLIENT_TYPE_MAX = ARGUS_MACHINE_CLIENT_ARGUS_COMMAND,
} argus_machine_client_type_t;

enum {
    ARGUS_MACHINE_TRANSPORT_HTTP = 1U << 0,
    ARGUS_MACHINE_TRANSPORT_MQTT = 1U << 1,
    ARGUS_MACHINE_TRANSPORT_LOCAL_SERVICE = 1U << 2,
    ARGUS_MACHINE_TRANSPORT_DEFINED_MASK =
        ARGUS_MACHINE_TRANSPORT_HTTP | ARGUS_MACHINE_TRANSPORT_MQTT |
        ARGUS_MACHINE_TRANSPORT_LOCAL_SERVICE,
};

typedef struct __attribute__((packed)) {
    uint8_t record_version;
    uint8_t enabled;
    uint8_t client_type;
    uint8_t allowed_transports;
    uint8_t revoked;
    uint8_t reserved;
    uint16_t role_mask;
    char identifier[ARGUS_SECURITY_ID_MAX + 1U];
    char display_name[ARGUS_SECURITY_DISPLAY_MAX + 1U];
    char scope[ARGUS_SECURITY_SCOPE_MAX + 1U];
    char topic_scope[ARGUS_SECURITY_TOPIC_SCOPE_MAX + 1U];
    char api_scope[ARGUS_SECURITY_API_SCOPE_MAX + 1U];
    char enrollment_actor[ARGUS_SECURITY_ID_MAX + 1U];
    argus_permission_set_t permissions;
    uint32_t credential_version;
    uint32_t record_security_epoch;
    argus_password_verifier_t verifier;
} argus_security_machine_record_t;

typedef struct __attribute__((packed)) {
    uint8_t record_version;
    uint8_t provisioned;
    uint8_t length;
    uint8_t reserved;
    uint32_t credential_version;
    uint8_t value[ARGUS_SECURITY_AP_SECRET_MAX + 1U];
} argus_security_ap_secret_record_t;

typedef struct __attribute__((packed)) {
    uint16_t schema_version;
    uint16_t flags;
    uint32_t security_epoch;
    uint8_t role_count;
    uint8_t human_count;
    uint8_t machine_count;
    uint8_t console_verifier_provisioned;
    uint32_t console_credential_version;
    uint8_t migration_state;
    uint8_t recovery_state;
    uint16_t reserved;
    argus_security_role_record_t builtin_roles[ARGUS_SECURITY_BUILTIN_ROLE_COUNT];
    argus_security_ap_secret_record_t factory_ap;
    argus_security_ap_secret_record_t active_ap;
    argus_password_verifier_t console_verifier;
} argus_security_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t payload_length;
    uint32_t generation;
    uint32_t crc32;
    uint32_t valid_marker;
    argus_security_payload_t payload;
} argus_security_slot_t;

typedef struct {
    esp_err_t (*read_slot)(void *ctx, uint8_t index,
                           argus_security_slot_t *out);
    esp_err_t (*write_slot)(void *ctx, uint8_t index,
                            const argus_security_slot_t *slot);
    esp_err_t (*read_selector)(void *ctx, uint8_t *out_selector);
    esp_err_t (*write_selector)(void *ctx, uint8_t selector);
    void *ctx;
} argus_security_store_driver_t;

typedef struct {
    argus_security_payload_t active;
    uint32_t generation;
    uint8_t active_slot;
    argus_security_store_state_t state;
    bool redundancy_degraded;
    const argus_security_store_driver_t *driver;
} argus_security_store_core_t;

typedef struct {
    argus_security_store_state_t state;
    uint16_t schema_version;
    uint32_t generation;
    uint32_t security_epoch;
    uint8_t role_count;
    uint8_t human_count;
    uint8_t machine_count;
    bool factory_ap_provisioned;
    bool active_ap_provisioned;
    bool console_verifier_provisioned;
    argus_security_migration_state_t migration_state;
    argus_security_recovery_state_t recovery_state;
    bool encryption_enabled;
    bool key_physically_extractable;
    bool redundancy_degraded;
} argus_security_store_status_t;

void argus_security_store_default_payload(argus_security_payload_t *out);
bool argus_security_payload_valid(const argus_security_payload_t *payload);
bool argus_security_role_record_valid(
    const argus_security_role_record_t *record);
bool argus_security_human_record_valid(
    const argus_security_human_record_t *record);
bool argus_security_machine_record_valid(
    const argus_security_machine_record_t *record);
uint32_t argus_security_payload_crc32(
    const argus_security_payload_t *payload);

esp_err_t argus_security_store_core_init(
    argus_security_store_core_t *core,
    const argus_security_store_driver_t *driver);
esp_err_t argus_security_store_core_commit(
    argus_security_store_core_t *core,
    const argus_security_payload_t *payload);

esp_err_t argus_security_store_init(void);
esp_err_t argus_security_store_get_status(
    argus_security_store_status_t *out_status);

esp_err_t argus_security_store_bootstrap_ap_secrets(
    const uint8_t *secret, size_t secret_len);
esp_err_t argus_security_store_provision_initial(
    const uint8_t *factory_ap, size_t factory_ap_len,
    const uint8_t *active_ap, size_t active_ap_len,
    const argus_password_verifier_t *console_verifier);
esp_err_t argus_security_store_get_factory_ap_secret(
    uint8_t *out, size_t out_size, size_t *out_len);
esp_err_t argus_security_store_get_active_ap_secret(
    uint8_t *out, size_t out_size, size_t *out_len);

esp_err_t argus_security_store_set_console_verifier(
    const argus_password_verifier_t *record, bool allow_replace);
esp_err_t argus_security_store_get_console_verifier(
    argus_password_verifier_t *out_record, uint32_t *out_version);
esp_err_t argus_security_store_set_migration_state(
    argus_security_migration_state_t state);
esp_err_t argus_security_store_set_recovery_state(
    argus_security_recovery_state_t state);
argus_security_recovery_state_t argus_security_store_get_recovery_state(void);

#ifdef __cplusplus
}
#endif

#endif
