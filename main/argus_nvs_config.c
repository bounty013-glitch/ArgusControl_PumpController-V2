/**
 * @file argus_nvs_config.c
 * @brief Power-Loss-Safe Dual-Slot NVS Configuration Manager Implementation
 */

#include "argus_nvs_config.h"
#include "argus_identity.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "argus_nvs_cfg";

#define ARGUS_NVS_NS_CFG        "argus_cfg"
#define ARGUS_NVS_NS_SYS        "argus_sys"
#define ARGUS_NVS_NS_RST        "argus_rst"  /* Dedicated namespace for reset-pending marker.
                                              * Factory reset erases CFG and SYS but never RST,
                                              * ensuring the marker survives power-loss during
                                              * reset and can trigger recovery on next boot. */
#define ARGUS_NVS_KEY_SLOT_0    "slot_a"
#define ARGUS_NVS_KEY_SLOT_1    "slot_b"
#define ARGUS_NVS_KEY_SELECTOR  "active_slot"
#define ARGUS_NVS_KEY_RESET_PEND "rst_pend"
#define ARGUS_NVS_KEY_PROV_HWM   "prov_hwm"   /* Monotonic provisioning high-water marker */

static argus_nvs_core_t s_prod_core;
static bool s_initialized = false;

static const argus_nvs_driver_t *s_custom_driver = NULL;

// Standard IEEE 802.3 CRC32 — delegates to internal raw helper after it's defined.
// Forward declaration of calc_crc32_raw is not needed because argus_nvs_config_calc_crc32
// is called externally and calc_crc32_raw is static. The public function is defined after
// calc_crc32_raw below. We keep this position as a stub for the declaration order.
// (Actual implementation moved below calc_crc32_raw.)

bool argus_nvs_config_gen_is_newer(uint32_t gen_a, uint32_t gen_b)
{
    if (gen_a == 0) return false;
    if (gen_b == 0) return true;
    return ((int32_t)(gen_a - gen_b)) > 0;
}

/**
 * @brief Compute CRC32 over raw bytes (used for V1 migration and V2 payloads).
 */
static uint32_t calc_crc32_raw(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320U;
            else         crc = (crc >> 1);
        }
    }
    return ~crc;
}

uint32_t argus_nvs_config_calc_crc32(const argus_config_payload_t *payload)
{
    if (!payload) return 0;
    return calc_crc32_raw((const uint8_t *)payload, sizeof(argus_config_payload_t));
}

/**
 * @brief Validate a dual-slot record. Supports V1→V2 transparent migration.
 *
 * V1 slots have schema_version=1, payload_length=228 (no provisioned_flags).
 * On valid V1 read, provisioned_flags is set to 0 (unprovisioned, eligible
 * for one portal provisioning). This is a documented development migration
 * rule, not an inferred universal production policy.
 *
 * V2 slots have schema_version=2, payload_length=229 (with provisioned_flags).
 */
static bool is_slot_valid(argus_cfg_slot_t *slot)
{
    if (!slot) return false;
    if (slot->valid_marker != ARGUS_CONFIG_VALID_MARKER) return false;
    if (slot->config_generation == 0) return false;

    if (slot->schema_version == ARGUS_CONFIG_SCHEMA_VERSION &&
        slot->payload_length == sizeof(argus_config_payload_t)) {
        /* Current schema — verify CRC over full payload */
        uint32_t computed = calc_crc32_raw((const uint8_t *)&slot->payload,
                                           sizeof(argus_config_payload_t));
        if (computed != slot->crc32) return false;
        return (argus_nvs_config_validate(&slot->payload) == ESP_OK);
    }

    if (slot->schema_version == ARGUS_CONFIG_SCHEMA_V1 &&
        slot->payload_length == ARGUS_CONFIG_PAYLOAD_V1_SIZE) {
        /* V1 migration: CRC was over the old 228-byte payload (no flags field) */
        uint32_t computed = calc_crc32_raw((const uint8_t *)&slot->payload,
                                           ARGUS_CONFIG_PAYLOAD_V1_SIZE);
        if (computed != slot->crc32) return false;

        /* Migrate in-place: set provisioned_flags to 0 (identity editable) */
        slot->payload.provisioned_flags = 0;

        /* Upgrade slot metadata so commit writes V2 */
        slot->schema_version = ARGUS_CONFIG_SCHEMA_VERSION;
        slot->payload_length = sizeof(argus_config_payload_t);
        slot->crc32 = calc_crc32_raw((const uint8_t *)&slot->payload,
                                      sizeof(argus_config_payload_t));

        return (argus_nvs_config_validate(&slot->payload) == ESP_OK);
    }

    return false;  /* Unknown schema — reject */
}

// Default production NVS storage driver implementations
// Default production NVS storage driver implementations
static esp_err_t prod_read_slot(void *ctx, uint8_t slot_index, argus_cfg_slot_t *out_slot)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_CFG, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    const char *key = (slot_index == 0) ? ARGUS_NVS_KEY_SLOT_0 : ARGUS_NVS_KEY_SLOT_1;
    size_t len = sizeof(argus_cfg_slot_t);
    err = nvs_get_blob(handle, key, out_slot, &len);
    nvs_close(handle);
    return err;
}

static esp_err_t prod_write_slot(void *ctx, uint8_t slot_index, const argus_cfg_slot_t *in_slot)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_CFG, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    const char *key = (slot_index == 0) ? ARGUS_NVS_KEY_SLOT_0 : ARGUS_NVS_KEY_SLOT_1;
    err = nvs_set_blob(handle, key, in_slot, sizeof(argus_cfg_slot_t));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t prod_read_selector(void *ctx, uint8_t *out_selector)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_CFG, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_u8(handle, ARGUS_NVS_KEY_SELECTOR, out_selector);
    nvs_close(handle);
    return err;
}

static esp_err_t prod_write_selector(void *ctx, uint8_t selector)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_CFG, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, ARGUS_NVS_KEY_SELECTOR, selector);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t prod_read_reset_pending(void *ctx, bool *out_pending)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_RST, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace doesn't exist → fresh device, no reset pending */
        *out_pending = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        /* Real storage error → propagate, do not hide */
        return err;
    }

    uint8_t val = 0;
    err = nvs_get_u8(handle, ARGUS_NVS_KEY_RESET_PEND, &val);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Key doesn't exist → not pending */
        *out_pending = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        /* Real read error → propagate */
        return err;
    }
    *out_pending = (val == 1);
    return ESP_OK;
}

static esp_err_t prod_write_reset_pending(void *ctx, bool pending)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_RST, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, ARGUS_NVS_KEY_RESET_PEND, pending ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t prod_read_provisioned_hwm(void *ctx, uint8_t *out_flags)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_SYS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace doesn't exist → fresh device, no HWM yet */
        *out_flags = 0;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (err != ESP_OK) return err;  /* Real error → propagate */
    err = nvs_get_u8(handle, ARGUS_NVS_KEY_PROV_HWM, out_flags);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out_flags = 0;
    }
    return err;
}

static esp_err_t prod_write_provisioned_hwm(void *ctx, uint8_t flags)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ARGUS_NVS_NS_SYS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, ARGUS_NVS_KEY_PROV_HWM, flags);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t prod_erase_all(void *ctx)
{
    (void)ctx;
    esp_err_t first_err = ESP_OK;
    nvs_handle_t handle;

    esp_err_t open_err = nvs_open(ARGUS_NVS_NS_CFG, NVS_READWRITE, &handle);
    if (open_err == ESP_OK) {
        esp_err_t erase_err = nvs_erase_all(handle);
        if (erase_err != ESP_OK && first_err == ESP_OK) first_err = erase_err;
        esp_err_t commit_err = nvs_commit(handle);
        if (commit_err != ESP_OK && first_err == ESP_OK) first_err = commit_err;
        nvs_close(handle);
    } else if (open_err != ESP_ERR_NVS_NOT_FOUND && first_err == ESP_OK) {
        first_err = open_err;
    }

    open_err = nvs_open(ARGUS_NVS_NS_SYS, NVS_READWRITE, &handle);
    if (open_err == ESP_OK) {
        esp_err_t erase_err = nvs_erase_all(handle);
        if (erase_err != ESP_OK && first_err == ESP_OK) first_err = erase_err;
        esp_err_t commit_err = nvs_commit(handle);
        if (commit_err != ESP_OK && first_err == ESP_OK) first_err = commit_err;
        nvs_close(handle);
    } else if (open_err != ESP_ERR_NVS_NOT_FOUND && first_err == ESP_OK) {
        first_err = open_err;
    }

    return first_err;
}

static const argus_nvs_driver_t s_prod_driver = {
    .read_slot = prod_read_slot,
    .write_slot = prod_write_slot,
    .read_selector = prod_read_selector,
    .write_selector = prod_write_selector,
    .read_reset_pending = prod_read_reset_pending,
    .write_reset_pending = prod_write_reset_pending,
    .read_provisioned_hwm = prod_read_provisioned_hwm,
    .write_provisioned_hwm = prod_write_provisioned_hwm,
    .erase_all = prod_erase_all,
    .ctx = NULL
};

esp_err_t argus_nvs_core_init(argus_nvs_core_t *core, const argus_nvs_driver_t *driver)
{
    if (!core || !driver) return ESP_ERR_INVALID_ARG;
    if (!driver->read_slot || !driver->read_selector) return ESP_ERR_INVALID_ARG;
    memset(core, 0, sizeof(argus_nvs_core_t));
    core->driver = driver;

    argus_cfg_slot_t slot_a = {0}, slot_b = {0};
    esp_err_t err_a = driver->read_slot(driver->ctx, 0, &slot_a);
    bool valid_a = (err_a == ESP_OK) && is_slot_valid(&slot_a);

    esp_err_t err_b = driver->read_slot(driver->ctx, 1, &slot_b);
    bool valid_b = (err_b == ESP_OK) && is_slot_valid(&slot_b);

    uint8_t selector = 0xFF;
    driver->read_selector(driver->ctx, &selector);

    /* ── Phase 1: Selector-based slot selection ─────────────────────── */
    if (valid_a && valid_b) {
        if (selector == 0) {
            core->active_slot_index = 0;
            memcpy(&core->active_config, &slot_a.payload, sizeof(argus_config_payload_t));
            core->active_generation = slot_a.config_generation;
            core->has_valid_config = true;
        } else if (selector == 1) {
            core->active_slot_index = 1;
            memcpy(&core->active_config, &slot_b.payload, sizeof(argus_config_payload_t));
            core->active_generation = slot_b.config_generation;
            core->has_valid_config = true;
        } else {
            if (argus_nvs_config_gen_is_newer(slot_b.config_generation, slot_a.config_generation)) {
                core->active_slot_index = 1;
                memcpy(&core->active_config, &slot_b.payload, sizeof(argus_config_payload_t));
                core->active_generation = slot_b.config_generation;
            } else {
                core->active_slot_index = 0;
                memcpy(&core->active_config, &slot_a.payload, sizeof(argus_config_payload_t));
                core->active_generation = slot_a.config_generation;
            }
            core->has_valid_config = true;
        }
    } else if (valid_a) {
        core->active_slot_index = 0;
        memcpy(&core->active_config, &slot_a.payload, sizeof(argus_config_payload_t));
        core->active_generation = slot_a.config_generation;
        core->has_valid_config = true;
    } else if (valid_b) {
        core->active_slot_index = 1;
        memcpy(&core->active_config, &slot_b.payload, sizeof(argus_config_payload_t));
        core->active_generation = slot_b.config_generation;
        core->has_valid_config = true;
    } else {
        memset(&core->active_config, 0, sizeof(argus_config_payload_t));
        core->active_generation = 0;
        core->has_valid_config = false;
    }

    /* ── Phase 2: Monotonic provisioning enforcement ────────────────── */
    /* Read durable high-water marker.
     * ESP_ERR_NVS_NOT_FOUND → fresh device, hwm_flags = 0.
     * Any other error → fail closed: treat as all bits locked (0xFF).
     * This prevents a transient NVS read failure from reopening provisioning. */
    uint8_t hwm_flags = 0;
    if (driver->read_provisioned_hwm) {
        esp_err_t hwm_err = driver->read_provisioned_hwm(driver->ctx, &hwm_flags);
        if (hwm_err != ESP_OK && hwm_err != ESP_ERR_NVS_NOT_FOUND) {
            hwm_flags = 0xFF;  /* Fail closed: lock all provisioning bits */
        }
    }

    if (core->has_valid_config) {
        /* Merge slot-level flags with durable HWM */
        uint8_t monotonic_flags = hwm_flags;
        if (valid_a) monotonic_flags |= slot_a.payload.provisioned_flags;
        if (valid_b) monotonic_flags |= slot_b.payload.provisioned_flags;

        core->active_config.provisioned_flags |= monotonic_flags;
    }

    /* ── Phase 3: Selector-failure recovery ─────────────────────────── *
     * If HWM proves provisioning occurred but the selected config is NOT
     * provisioned, the selector was likely not activated after a successful
     * HWM write (power loss between HWM write and selector activation).
     *
     * Recovery: find the valid provisioned candidate among the two slots,
     * switch to it, and attempt a best-effort selector repair.
     *
     * Selector repair is best-effort: if write_selector fails, the
     * in-memory configuration is still correct for this boot. The next
     * successful commit will establish a new durable selector.
     *
     * If neither slot is provisioned but HWM says it should be, the
     * provisioning data is unrecoverable (both slots corrupted). The
     * fail-closed monotonic enforcement above already locked provisioning
     * via the HWM flags merge, preventing identity reopening.            */
    if (core->has_valid_config && valid_a && valid_b &&
        (hwm_flags & ARGUS_CFG_PROVISIONED_IDENTITY)) {
        bool sel_a_prov = (slot_a.payload.provisioned_flags & ARGUS_CFG_PROVISIONED_IDENTITY);
        bool sel_b_prov = (slot_b.payload.provisioned_flags & ARGUS_CFG_PROVISIONED_IDENTITY);

        /* If the active slot is unprovisioned but the other is provisioned,
         * this is a selector-failure: switch to the provisioned slot. */
        if (core->active_slot_index == 0 && !sel_a_prov && sel_b_prov) {
            core->active_slot_index = 1;
            memcpy(&core->active_config, &slot_b.payload, sizeof(argus_config_payload_t));
            core->active_generation = slot_b.config_generation;
            core->active_config.provisioned_flags |= hwm_flags;
            /* Best-effort selector repair: failure is non-fatal.
             * In-memory state is correct; next commit repairs durably. */
            if (driver->write_selector) {
                (void)driver->write_selector(driver->ctx, 1);
            }
        } else if (core->active_slot_index == 1 && !sel_b_prov && sel_a_prov) {
            core->active_slot_index = 0;
            memcpy(&core->active_config, &slot_a.payload, sizeof(argus_config_payload_t));
            core->active_generation = slot_a.config_generation;
            core->active_config.provisioned_flags |= hwm_flags;
            /* Best-effort selector repair */
            if (driver->write_selector) {
                (void)driver->write_selector(driver->ctx, 0);
            }
        }
    }

    core->initialized = true;
    return ESP_OK;
}

esp_err_t argus_nvs_core_get(const argus_nvs_core_t *core, argus_config_payload_t *out_cfg)
{
    if (!core || !out_cfg) return ESP_ERR_INVALID_ARG;
    if (!core->has_valid_config) return ESP_ERR_NOT_FOUND;
    memcpy(out_cfg, &core->active_config, sizeof(argus_config_payload_t));
    return ESP_OK;
}

esp_err_t argus_nvs_core_commit(argus_nvs_core_t *core, const argus_config_payload_t *in_cfg)
{
    if (!core || !in_cfg || !core->driver) return ESP_ERR_INVALID_ARG;
    if (!core->driver->read_selector || !core->driver->write_slot || !core->driver->read_slot || !core->driver->write_selector) return ESP_ERR_INVALID_ARG;

    if (strcmp(in_cfg->sta_pass, ARGUS_CONFIG_MASK_STRING) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t val_err = argus_nvs_config_validate(in_cfg);
    if (val_err != ESP_OK) return val_err;

    uint8_t selector = 0xFF;
    esp_err_t sel_read_err = core->driver->read_selector(core->driver->ctx, &selector);

    uint8_t inactive_slot;
    if (sel_read_err == ESP_OK && selector <= 1) {
        /* The selector is the source of truth for which slot the system
         * boots from.  Always write to the OTHER slot to guarantee the
         * selector-pointed LKG is never overwritten.                    */
        inactive_slot = (selector == 0) ? 1 : 0;
    } else {
        /* Selector unreadable (first commit or corrupted).  Fall back to
         * the core's active_slot_index.                                  */
        inactive_slot = (core->active_slot_index == 0) ? 1 : 0;
    }
    uint32_t next_gen = core->active_generation + 1;

    argus_cfg_slot_t slot = {
        .schema_version = ARGUS_CONFIG_SCHEMA_VERSION,
        .config_generation = next_gen,
        .payload_length = sizeof(argus_config_payload_t),
        .crc32 = argus_nvs_config_calc_crc32(in_cfg),
        .valid_marker = ARGUS_CONFIG_VALID_MARKER
    };
    memcpy(&slot.payload, in_cfg, sizeof(argus_config_payload_t));

    esp_err_t write_err = core->driver->write_slot(core->driver->ctx, inactive_slot, &slot);
    if (write_err != ESP_OK) return write_err;

    argus_cfg_slot_t verify_slot = {0};
    esp_err_t readback_err = core->driver->read_slot(core->driver->ctx, inactive_slot, &verify_slot);
    if (readback_err != ESP_OK || !is_slot_valid(&verify_slot) ||
        memcmp(&verify_slot.payload, in_cfg, sizeof(argus_config_payload_t)) != 0) {
        /* Readback verification failed.  Do NOT update the selector.
         * The selector still points to the LKG, which is in the other
         * slot and was never overwritten.                                */
        return ESP_ERR_INVALID_STATE;
    }

    /* Write monotonic provisioning high-water marker BEFORE selector activation.
     * If this fails, the selector is NOT updated — the old slot remains active.
     * The new slot is written but orphaned; it will be overwritten on the next
     * successful commit. This guarantees commit does not return ESP_OK unless
     * the durable provisioning invariant is established.
     *
     * Power-loss recovery:
     * - After slot write, before HWM: Old selector active. Orphaned new slot.
     * - After HWM, before selector: Old slot active. HWM set → identity locked.
     * - After selector: New slot active. HWM set. Fully committed. */
    if ((in_cfg->provisioned_flags & ARGUS_CFG_PROVISIONED_IDENTITY) &&
        core->driver->write_provisioned_hwm) {
        esp_err_t hwm_err = core->driver->write_provisioned_hwm(
            core->driver->ctx, in_cfg->provisioned_flags);
        if (hwm_err != ESP_OK) {
            return hwm_err;  /* Do NOT activate selector */
        }
    }

    esp_err_t sel_err = core->driver->write_selector(core->driver->ctx, inactive_slot);
    if (sel_err != ESP_OK) return sel_err;

    argus_cfg_slot_t final_slot = {0};
    esp_err_t final_read_err = core->driver->read_slot(core->driver->ctx, inactive_slot, &final_slot);
    if (final_read_err != ESP_OK || !is_slot_valid(&final_slot)) {
        return ESP_ERR_INVALID_STATE;
    }

    core->active_slot_index = inactive_slot;
    core->active_generation = next_gen;
    memcpy(&core->active_config, &final_slot.payload, sizeof(argus_config_payload_t));
    core->has_valid_config = true;

    return ESP_OK;
}

/**
 * @brief Pure boot-recovery transaction operating on injected driver.
 *
 * Checks for a pending factory-reset marker. If pending is true,
 * erases reset-scope data and clears the marker. Returns the exact
 * originating error on failure without translating it.
 *
 * Missing marker/namespace is normal and treated as not pending.
 * Real storage errors are propagated.
 *
 * @param drv  Injected storage driver (production or mock).
 * @return ESP_OK if no recovery needed or recovery completed.
 *         Exact error if marker read, erase, or clear failed.
 */
esp_err_t argus_nvs_core_recovery_check(const argus_nvs_driver_t *drv)
{
    if (!drv) return ESP_ERR_INVALID_ARG;
    if (!drv->read_reset_pending || !drv->erase_all || !drv->write_reset_pending) return ESP_ERR_INVALID_ARG;

    bool reset_pending = false;
    esp_err_t pend_err = drv->read_reset_pending(drv->ctx, &reset_pending);
    if (pend_err != ESP_OK) {
        return pend_err;
    }
    if (!reset_pending) {
        return ESP_OK;
    }

    /* Pending is true — erase reset-scope data */
    esp_err_t erase_err = drv->erase_all(drv->ctx);
    if (erase_err != ESP_OK) {
        /* Do not clear the marker — recovery will retry on next boot */
        return erase_err;
    }

    /* Erase succeeded — clear the marker */
    esp_err_t clear_err = drv->write_reset_pending(drv->ctx, false);
    if (clear_err != ESP_OK) {
        /* Marker remains true — next boot will re-erase (idempotent) */
        return clear_err;
    }

    return ESP_OK;
}

/**
 * @brief Pure factory-reset transaction operating on caller-owned core and injected driver.
 *
 * Orchestrates the durable reset transaction:
 *   1. Write pending=true (fail-closed: returns immediately on failure)
 *   2. Erase reset-scope data (returns immediately on failure; marker preserved)
 *   3. Clear pending (non-fatal: erase succeeded, so core reinit proceeds)
 *   4. Reinitialize caller-owned core to match the erased state
 *
 * Production policy: if clearing the pending marker fails after a successful
 * erase, the core is still reinitialized to match the erased storage. The
 * marker remains true and will trigger an idempotent recovery erase on next
 * boot. The clear error is returned to the caller.
 *
 * @param core  Caller-owned core state to reinitialize.
 * @param drv   Injected storage driver (production or mock).
 * @return ESP_OK on full success.
 *         Exact error from the first failing operation.
 */
esp_err_t argus_nvs_core_factory_reset(argus_nvs_core_t *core, const argus_nvs_driver_t *drv)
{
    if (!core || !drv) return ESP_ERR_INVALID_ARG;
    if (!drv->write_reset_pending || !drv->erase_all) return ESP_ERR_INVALID_ARG;

    /* Step 1: Mark pending before destructive operations */
    esp_err_t pend_err = drv->write_reset_pending(drv->ctx, true);
    if (pend_err != ESP_OK) {
        return pend_err;
    }

    /* Step 2: Erase reset-scope data (CFG + SYS namespaces) */
    esp_err_t erase_err = drv->erase_all(drv->ctx);
    if (erase_err != ESP_OK) {
        /* Do NOT clear pending — recovery will re-erase on next boot */
        return erase_err;
    }

    /* Step 3: Clear pending marker */
    esp_err_t clear_err = drv->write_reset_pending(drv->ctx, false);
    if (clear_err != ESP_OK) {
        /* Erase succeeded but marker stuck — next boot will re-erase
         * (idempotent). Continue to reinit core to match erased state.
         * This is the documented production policy. */
    }

    /* Step 4: Reinitialize core to match erased/uncommissioned state */
    esp_err_t core_err = argus_nvs_core_init(core, drv);
    if (core_err != ESP_OK) {
        return core_err;
    }

    /* Return clear_err if the pending-clear failed but erase and core
     * succeeded — caller knows the marker is stuck. */
    return clear_err;
}

static const argus_nvs_driver_t *get_driver(void)
{
    return s_custom_driver ? s_custom_driver : &s_prod_driver;
}

esp_err_t argus_nvs_config_init(const argus_nvs_driver_t *driver)
{
    s_custom_driver = driver;
    s_initialized = false;

    esp_err_t flash_err = ESP_OK;
    if (!s_custom_driver) {
        flash_err = nvs_flash_init();
        if (flash_err == ESP_ERR_NVS_NO_FREE_PAGES || flash_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            esp_err_t erase_err = nvs_flash_erase();
            if (erase_err != ESP_OK) {
                ESP_LOGE(TAG, "nvs_flash_erase() failed: %s", esp_err_to_name(erase_err));
                return erase_err;
            }
            flash_err = nvs_flash_init();
        }
        if (flash_err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_init() failed: %s", esp_err_to_name(flash_err));
            return flash_err;
        }
        ESP_LOGI(TAG, "nvs_flash_init() result: %s (%d)", esp_err_to_name(flash_err), flash_err);
    }

    const argus_nvs_driver_t *drv = get_driver();

    /* Delegate boot-recovery to the same pure helper exercised by tests.
     * The reset-pending marker lives in ARGUS_NVS_NS_RST, which
     * factory-reset erasure does not touch, ensuring durability. */
    esp_err_t recovery_err = argus_nvs_core_recovery_check(drv);
    if (recovery_err != ESP_OK) {
        ESP_LOGE(TAG, "Boot recovery failed: %s", esp_err_to_name(recovery_err));
        return recovery_err;
    }

    /* Delegate to the single core algorithm for slot selection,
     * HWM enforcement, monotonic provisioning, and selector recovery. */
    esp_err_t core_err = argus_nvs_core_init(&s_prod_core, drv);
    if (core_err != ESP_OK) {
        ESP_LOGE(TAG, "Core init failed: %s", esp_err_to_name(core_err));
        return core_err;
    }

    /* If no valid config, set uncommissioned defaults */
    if (!s_prod_core.has_valid_config) {
        snprintf(s_prod_core.active_config.client_id,
                 sizeof(s_prod_core.active_config.client_id), "default_client");
        snprintf(s_prod_core.active_config.unit_id,
                 sizeof(s_prod_core.active_config.unit_id), "unit_01");
        snprintf(s_prod_core.active_config.device_name,
                 sizeof(s_prod_core.active_config.device_name), "Argus Peristaltic Pump V2");
    }

    bool is_comm = argus_nvs_config_is_commissioned(&s_prod_core.active_config);
    ESP_LOGI(TAG, "Selected LKG Slot: %u (gen %lu), Commissioned: %s (%s)",
             s_prod_core.active_slot_index, (unsigned long)s_prod_core.active_generation,
             is_comm ? "YES" : "NO",
             argus_nvs_config_get_uncommissioned_reason(&s_prod_core.active_config));

    s_initialized = true;
    return ESP_OK;
}

esp_err_t argus_nvs_config_get_effective(argus_config_payload_t *out_cfg, bool *out_has_persisted_config)
{
    if (!out_cfg || !out_has_persisted_config) return ESP_ERR_INVALID_ARG;
    
    if (!s_initialized) {
        esp_err_t err = argus_nvs_config_init(NULL);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            return err;
        }
    }
    
    memcpy(out_cfg, &s_prod_core.active_config, sizeof(argus_config_payload_t));
    *out_has_persisted_config = s_prod_core.has_valid_config;
    
    return ESP_OK;
}

esp_err_t argus_nvs_config_validate(const argus_config_payload_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    if (!argus_identity_validate_client_id(cfg->client_id)) return ESP_ERR_INVALID_ARG;
    if (!argus_identity_validate_unit_id(cfg->unit_id)) return ESP_ERR_INVALID_ARG;
    if (!argus_identity_validate_device_name(cfg->device_name)) return ESP_ERR_INVALID_ARG;

    size_t ssid_len = strlen(cfg->sta_ssid);
    if (ssid_len > ARGUS_CFG_STA_SSID_MAX) return ESP_ERR_INVALID_ARG;

    size_t pass_len = strlen(cfg->sta_pass);
    if (ssid_len > 0) {
        if (pass_len < ARGUS_CFG_STA_PASS_MIN || pass_len > ARGUS_CFG_STA_PASS_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

const char *argus_nvs_config_get_uncommissioned_reason(const argus_config_payload_t *cfg)
{
    if (!cfg) return "Null payload pointer";
    if (!argus_identity_validate_client_id(cfg->client_id)) return "Invalid Client ID";
    if (!argus_identity_validate_unit_id(cfg->unit_id)) return "Invalid Unit ID";
    if (!argus_identity_validate_device_name(cfg->device_name)) return "Invalid Device Name";
    if (strlen(cfg->sta_ssid) == 0) return "STA SSID is empty";
    size_t pass_len = strlen(cfg->sta_pass);
    if (pass_len < ARGUS_CFG_STA_PASS_MIN) return "STA Pass length < 8 chars";
    if (pass_len > ARGUS_CFG_STA_PASS_MAX) return "STA Pass length > 63 chars";
    return "Valid / Commissioned";
}

bool argus_nvs_config_is_commissioned(const argus_config_payload_t *cfg)
{
    if (argus_nvs_config_validate(cfg) != ESP_OK) return false;
    if (strlen(cfg->sta_ssid) == 0) return false;
    if (strlen(cfg->sta_pass) < ARGUS_CFG_STA_PASS_MIN) return false;
    return true;
}

esp_err_t argus_nvs_config_commit(const argus_config_payload_t *in_cfg)
{
    if (!in_cfg) return ESP_ERR_INVALID_ARG;

    if (strlen(in_cfg->sta_pass) > 0 && strcmp(in_cfg->sta_pass, ARGUS_CONFIG_MASK_STRING) == 0) {
        ESP_LOGE(TAG, "Commit rejected: Mask string submitted as password");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = argus_nvs_config_validate(in_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Commit rejected: Staged payload failed validation (%s)", esp_err_to_name(err));
        return err;
    }

    /* Delegate to the single core commit algorithm.
     * This handles slot write, readback verify, HWM write (before selector),
     * selector activation, and final verify — the same path tests exercise. */
    err = argus_nvs_core_commit(&s_prod_core, in_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Core commit failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Log commissioning status for diagnostics */
    if (argus_nvs_config_is_commissioned(in_cfg)) {
        ESP_LOGI(TAG, "Commit complete: Payload is fully commissioned (STA credentials present)");
    } else {
        ESP_LOGW(TAG, "Commit complete: Payload is identity-only (no STA credentials). "
                 "Reason: %s", argus_nvs_config_get_uncommissioned_reason(in_cfg));
    }

    ESP_LOGI(TAG, "NVS commit & core verification PASSED cleanly.");
    return ESP_OK;
}

esp_err_t argus_nvs_config_factory_reset(void)
{
    const argus_nvs_driver_t *drv = get_driver();

    /* Delegate to the same pure helper exercised by tests */
    esp_err_t err = argus_nvs_core_factory_reset(&s_prod_core, drv);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Factory reset transaction failed: %s", esp_err_to_name(err));
        /* On pending-clear failure (erase succeeded), the core has already
         * been reinitialized by the helper. Set defaults regardless. */
    }

    /* Set uncommissioned defaults if core was reinitialized */
    if (!s_prod_core.has_valid_config) {
        snprintf(s_prod_core.active_config.client_id,
                 sizeof(s_prod_core.active_config.client_id), "default_client");
        snprintf(s_prod_core.active_config.unit_id,
                 sizeof(s_prod_core.active_config.unit_id), "unit_01");
        snprintf(s_prod_core.active_config.device_name,
                 sizeof(s_prod_core.active_config.device_name), "Argus Peristaltic Pump V2");
    }

    return err;
}

void argus_nvs_config_mask(const argus_config_payload_t *in_cfg, argus_config_payload_t *out_masked)
{
    if (!in_cfg || !out_masked) return;
    memcpy(out_masked, in_cfg, sizeof(argus_config_payload_t));
    if (strlen(out_masked->sta_pass) > 0) {
        snprintf(out_masked->sta_pass, sizeof(out_masked->sta_pass), "%s", ARGUS_CONFIG_MASK_STRING);
    }
}

esp_err_t argus_nvs_core_get_observation_snapshot(const argus_nvs_driver_t *drv, argus_nvs_observation_t *out_obs)
{
    if (!drv || !out_obs) return ESP_ERR_INVALID_ARG;

    esp_err_t first_unexpected = ESP_OK;
    memset(out_obs, 0, sizeof(*out_obs));

    // Read selector
    out_obs->selector_status = drv->read_selector(drv->ctx, &out_obs->selector);
    if (out_obs->selector_status == ESP_OK) {
        out_obs->selector_present = true;
    } else if (out_obs->selector_status == ESP_ERR_NOT_FOUND || out_obs->selector_status == ESP_ERR_NVS_NOT_FOUND) {
        out_obs->selector_present = false;
    } else {
        out_obs->selector_present = false;
        if (first_unexpected == ESP_OK) first_unexpected = out_obs->selector_status;
    }

    // Read slot A
    out_obs->slot_a_status = drv->read_slot(drv->ctx, 0, &out_obs->slot_a);
    if (out_obs->slot_a_status == ESP_OK) {
        out_obs->slot_a_present = true;
        out_obs->slot_a_valid = is_slot_valid(&out_obs->slot_a);
    } else if (out_obs->slot_a_status == ESP_ERR_NOT_FOUND || out_obs->slot_a_status == ESP_ERR_NVS_NOT_FOUND) {
        out_obs->slot_a_present = false;
        out_obs->slot_a_valid = false;
    } else {
        out_obs->slot_a_present = false;
        out_obs->slot_a_valid = false;
        if (first_unexpected == ESP_OK) first_unexpected = out_obs->slot_a_status;
    }

    // Read slot B
    out_obs->slot_b_status = drv->read_slot(drv->ctx, 1, &out_obs->slot_b);
    if (out_obs->slot_b_status == ESP_OK) {
        out_obs->slot_b_present = true;
        out_obs->slot_b_valid = is_slot_valid(&out_obs->slot_b);
    } else if (out_obs->slot_b_status == ESP_ERR_NOT_FOUND || out_obs->slot_b_status == ESP_ERR_NVS_NOT_FOUND) {
        out_obs->slot_b_present = false;
        out_obs->slot_b_valid = false;
    } else {
        out_obs->slot_b_present = false;
        out_obs->slot_b_valid = false;
        if (first_unexpected == ESP_OK) first_unexpected = out_obs->slot_b_status;
    }

    return first_unexpected;
}

esp_err_t argus_nvs_config_get_observation_snapshot(argus_nvs_observation_t *out_obs)
{
    if (!out_obs) return ESP_ERR_INVALID_ARG;
    return argus_nvs_core_get_observation_snapshot(get_driver(), out_obs);
}
