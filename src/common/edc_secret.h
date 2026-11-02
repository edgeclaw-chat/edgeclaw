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
