#ifndef ARGUS_PASSWORD_VERIFIER_H
#define ARGUS_PASSWORD_VERIFIER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARGUS_PASSWORD_FORMAT_VERSION 1U
#define ARGUS_PASSWORD_ALGORITHM_PBKDF2_HMAC_SHA256 1U
#define ARGUS_PASSWORD_SALT_SIZE 16U
#define ARGUS_PASSWORD_VERIFIER_SIZE 32U
#define ARGUS_PASSWORD_INPUT_MAX 64U
#define ARGUS_PASSWORD_ITERATIONS_MIN 10000U
#define ARGUS_PASSWORD_ITERATIONS_MAX 500000U
#define ARGUS_PASSWORD_ITERATIONS_DEFAULT 25000U

typedef struct __attribute__((packed)) {
    uint8_t format_version;
    uint8_t algorithm;
    uint8_t salt_length;
    uint8_t verifier_length;
    uint32_t iterations;
    uint8_t salt[ARGUS_PASSWORD_SALT_SIZE];
    uint8_t verifier[ARGUS_PASSWORD_VERIFIER_SIZE];
} argus_password_verifier_t;

typedef struct {
    uint32_t iterations;
    uint32_t elapsed_ms;
    size_t free_heap_before;
    size_t free_heap_after;
    uint32_t worker_stack_high_water_bytes;
} argus_password_benchmark_t;

bool argus_password_verifier_record_valid(
    const argus_password_verifier_t *record);

esp_err_t argus_password_pbkdf2_for_test(
    const uint8_t *password, size_t password_len,
    const uint8_t *salt, size_t salt_len,
    uint32_t iterations, uint8_t out[ARGUS_PASSWORD_VERIFIER_SIZE]);

esp_err_t argus_password_verifier_init(void);

esp_err_t argus_password_verifier_create(
    const uint8_t *password, size_t password_len, uint32_t iterations,
    argus_password_verifier_t *out_record);

esp_err_t argus_password_verifier_verify(
    const uint8_t *password, size_t password_len,
    const argus_password_verifier_t *record, bool *out_match);

esp_err_t argus_password_verifier_benchmark(
    uint32_t iterations, argus_password_benchmark_t *out_result);

void argus_password_zeroize(void *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif
