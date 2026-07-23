#ifndef ARGUS_TESTS_4D2_H
#define ARGUS_TESTS_4D2_H

#include "esp_err.h"

esp_err_t test_4d2_permission_ceiling_metadata(void);
esp_err_t test_4d2_record_schema_validation(void);
esp_err_t test_4d2_manifest_validation(void);
esp_err_t test_4d2_store_missing_initialization(void);
esp_err_t test_4d2_store_atomic_commit_readback(void);
esp_err_t test_4d2_store_interrupted_write(void);
esp_err_t test_4d2_store_selector_failure(void);
esp_err_t test_4d2_store_initial_selector_failure(void);
esp_err_t test_4d2_store_corrupt_fallback(void);
esp_err_t test_4d2_store_unsupported_version(void);
esp_err_t test_4d2_pbkdf2_known_answer(void);
esp_err_t test_4d2_verifier_create_verify(void);
esp_err_t test_4d2_verifier_salt_uniqueness(void);
esp_err_t test_4d2_verifier_malformed_records(void);
esp_err_t test_4d2_recovery_startup_low_and_short_press(void);
esp_err_t test_4d2_recovery_bounce_rejection(void);
esp_err_t test_4d2_recovery_long_hold_release_once(void);
esp_err_t test_4d2_recovery_commit_order_and_failures(void);
esp_err_t test_4d2_capacity_and_domain_bounds(void);
esp_err_t test_4d2_provisioning_synthetic_success(void);
esp_err_t test_4d2_provisioning_rejections(void);
esp_err_t test_4d2_migration_power_loss_idempotence(void);
esp_err_t test_4d2_migration_deferred_and_malformed(void);

#endif
