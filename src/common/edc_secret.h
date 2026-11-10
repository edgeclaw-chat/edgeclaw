#ifndef __BC_SECRET_H__
#define __BC_SECRET_H__

#include "sds.h"
#include <stdbool.h>

/* The ciphertext tag. It names the format so a later change can migrate instead of
 * guessing: the reader dispatches on it. This is v1 -- the first format to ship.
 *
 * The version is part of the prefix from the start rather than added when first needed.
 * Retrofitting it means a release whose reader cannot tell an old ciphertext from a new
 * one, which is exactly the migration that cannot be done without it. */
#define BC_SECRET_PREFIX "EDGECLAW_ENC:v1:"

/* Every tag that means "this is ciphertext", so a value carrying a version this build does
 * not implement is refused instead of being mistaken for a plaintext api_key. */
#define BC_SECRET_PREFIX_FAMILY "EDGECLAW_ENC:"

/* Initialize the secret module. Loads the master key from the
 * EDGECLAW_MASTER_KEY environment variable or stdin.
 *
 * The key is derived with PBKDF2-HMAC-SHA256 over a per-install random salt, and the salt
 * and iteration count live in edc_home/.key_hash together with a verifier:
 *
 *   {"v":2,"kdf":"pbkdf2-sha256","iter":600000,"salt":"<32 hex>","verify":"<64 hex>"}
 *
 * That file has to be read HERE rather than from config.json, because config.json is
 * encrypted with the key that comes out of this derivation -- the salt is an input to the
 * key, not something found alongside the ciphertext.
 *
 * If confirm_password is true (first-time setup), prompts twice, mints a new salt and
 * writes the record. Otherwise it prompts once and verifies against the stored verifier.
 * When edc_home is NULL no record can be kept, so the salt is ephemeral; the key still
 * works for this process, but encrypted values will not survive a restart.
 * Must be called once at startup, before edc_config_load().
 * Returns true on success. */
bool edc_secret_init(bool confirm_password, const char *edc_home);

/* Turn encryption back on for an install that chose "no encryption" at setup (see
 * edc_secret_init). Prompts for a new passphrase, mints a salt, and stores the record; the
 * caller then re-encrypts the api_key (edc_config_update_llm) because that value lives in
 * the running config, not here.
 *
 * Returns false when a key is already loaded (changing a passphrase is a different
 * operation), when there is no terminal to prompt on, or when the prompt failed. |msg|
 * receives a short reason the caller can show to the user. */
bool edc_secret_enable_encryption(const char *edc_home, sds *msg);

/* Clean up the secret module. Zeros master key from memory.
 *
 * THE MASTER KEY STAYS RESIDENT FOR THE WHOLE PROCESS, by design, and this is the only
 * call that wipes it. The obvious alternative -- wipe it right after edc_config_load()
 * has decrypted what it needs -- does not work here: `/connect` re-encrypts a new API key
 * at runtime, and edc_config_update_llm() writes whatever edc_secret_encrypt() returns,
 * so with the key gone it would overwrite the stored key with an empty string and then
 * report success. Until that write path can obtain the key again, "keys stay available"
 * is the contract. The cost is that the key lives in memory until exit; it is mlock()ed
 * (see lock_master_key) to keep it out of swap.
 *
 * Do not add a "lockdown after load" call without also fixing that write path. */
void edc_secret_cleanup(void);

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
