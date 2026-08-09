#ifndef __BC_SECRET_H__
#define __BC_SECRET_H__

#include "sds.h"
#include <stdbool.h>

#define BC_SECRET_PREFIX "EDGECLAW_ENC:"

/* Initialize the secret module. Loads the master key from the
 * EDGECLAW_MASTER_KEY environment variable or stdin.
 * If confirm_password is true (first-time setup), prompts for the
 * passphrase twice to confirm the user's input and stores a
 * verification hash in edc_home/.key_hash.
 * Otherwise prompts once and verifies against the stored hash.
 * edc_home may be NULL to skip storage/verification (env var mode).
 * Must be called once at startup, before edc_config_load().
 * Returns true on success. */
bool edc_secret_init(bool confirm_password, const char *edc_home);

/* Clean up the secret module. Zeros master key from memory. */
void edc_secret_cleanup(void);

/* Lock down the secret module: zero the master key and unlock the
 * memory page. Call this after edc_config_load() completes — the
 * master key is no longer needed once all config secrets are decrypted. */
void edc_secret_lockdown(void);

/* Encrypt a plaintext string.
 * Returns a string prefixed with BC_SECRET_PREFIX followed by base64 ciphertext,
 * or NULL on failure. Caller must sdsfree() the result. */
sds edc_secret_encrypt(sds plaintext);

/* Decrypt a ciphertext string (optionally prefixed with BC_SECRET_PREFIX).
 * If the input starts with the prefix, decrypts and returns the plaintext.
 * Otherwise returns sdsnew(input) as a pass-through.
 * Caller must sdsfree() the result. */
sds edc_secret_decrypt(const char *input);

#endif
