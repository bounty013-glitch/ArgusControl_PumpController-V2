// argus_factory_credential - the per-device default portal password.
//
// WHY THIS EXISTS (CER-6, raised from the console project 2026-08-01):
//
// A factory-fresh controller could not be logged into at all. No account is
// seeded on first boot, the console verifier takes the
// BUILD_DEFAULT_DEFERRED branch and is never provisioned, and the only
// writer of that verifier - change_own_password_post_handler() - begins with
// require_access(). You could not log in without a verifier and could not
// set a verifier without logging in. Every controller on the bench worked
// only because it carried a MIGRATED legacy portal password from a build
// that no longer exists; no unit had ever taken the blank-board path.
//
// DHR-004 anticipated this and deferred it to "offline/manufacturing
// provisioning", with a reconsideration trigger of "before production field
// release". That trigger fired: there is a real trailer being commissioned.
//
// THE SHAPE (decided by Shawn, 2026-08-01):
//
//   1. PER DEVICE, DERIVED FROM THE MAC, AUTOMATIC ON BOOT. No per-unit
//      code, no per-unit build, no manufacturing tooling that does not
//      exist. A blank board provisions its own default the first time it
//      runs, and five identical flashes produce five different passwords.
//   2. THE FORCED CHANGE ONLY NAGS. A controller that still carries its
//      factory credential says so, loudly and repeatedly, but does not
//      refuse to work. Blocking commissioning on a password change would
//      trade a small risk for a large one.
//
// This mirrors what the AP secret already does
// (argus_security_store_bootstrap_ap_secrets), so the console verifier now
// follows the SAME rule as the AP secret rather than a second, different
// one.
//
// SECRECY, HONESTLY STATED. The derivation is
//
//     SHA-256( salt || MAC ) -> 12 characters
//
// The MAC is not secret: it is visible in the service SSID
// (Argus-Service-<last 3 MAC bytes>) to anyone in radio range. The salt is
// therefore the whole of the secret. It lives in gitignored sdkconfig,
// never in tracked source, and it is what stops a stranger deriving the
// password from a beacon frame. With no salt configured the password is
// derivable by anyone who can see the SSID - that build logs a warning at
// every boot and should never leave a bench.
//
// The plaintext is never logged and never returned by any API. Operators
// obtain it with tools/factory_credential.py, which takes the MAC and the
// same salt. That satisfies PHASE_4D_1's requirement that provisioning
// place no plaintext in source, logs, evidence, or shell history.

#ifndef ARGUS_FACTORY_CREDENTIAL_H
#define ARGUS_FACTORY_CREDENTIAL_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Characters in a derived password. 12 characters from a 32-symbol alphabet
// is ~60 bits - far beyond what a local portal behind a WPA2 AP needs, and
// still short enough to read off a screen and type on a phone.
#define ARGUS_FACTORY_CREDENTIAL_LEN 12U

// Derives this device's factory portal password into `out`, which must hold
// at least ARGUS_FACTORY_CREDENTIAL_LEN + 1 bytes. Deterministic: the same
// device and salt always produce the same password, which is what lets an
// operator recover it without the device having to store or disclose it.
esp_err_t argus_factory_credential_derive(char *out, size_t cap);

// Provisions the console verifier from the derived password IF no verifier
// exists. Returns ESP_ERR_NOT_SUPPORTED when one is already present, so
// this is safe to call unconditionally on every boot and can never overwrite
// an operator's chosen password.
esp_err_t argus_factory_credential_bootstrap(void);

// True when the stored console verifier still matches the derived factory
// password - i.e. nobody has changed it yet.
//
// Determined by VERIFYING the derived password against the stored record,
// not by a persisted flag. That deliberately avoids touching the security
// store's schema (versioned, CRC'd, A/B committed), and it cannot drift out
// of sync with reality the way a flag could: if it says "still factory",
// that is because the factory password genuinely still works.
bool argus_factory_credential_in_use(void);

// Recomputes the above. Called after a password change so the nag stops
// without needing a reboot.
void argus_factory_credential_refresh(void);

#ifdef __cplusplus
}
#endif

#endif // ARGUS_FACTORY_CREDENTIAL_H
