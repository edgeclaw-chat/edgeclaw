#include "edc_secret.h"
#include "edc_compat.h"
#include "edc_platform.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "log.h"
#include "sds.h"
#include <errno.h>
#include <mbedtls/base64.h>
#include <mbedtls/cipher.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#ifndef _WIN32
#include <termios.h>
#endif
#include <unistd.h>

#define BC_KEY_SIZE 32
#define BC_GCM_IV_SIZE 12
#define BC_GCM_TAG_SIZE 16
#define BC_MASTER_KEY_ENV "EDGECLAW_MASTER_KEY"
#define MAX_PASSPHRASE 1024
#define BC_KEY_VERIFY_FILE ".key_hash"

/* Passphrase hardening. A passphrase is the one input here an attacker can guess, so the
 * derivation has to be expensive per guess; that is the whole reason this is PBKDF2 and
 * not a bare hash. 600k iterations is OWASP's 2023 PBKDF2-HMAC-SHA256 figure and costs
 * ~50 ms on a current CPU -- imperceptible at startup, ~600,000x the work per guess that
 * the previous single-HKDF-pass scheme cost.
 *
 * The iteration count is STORED alongside the ciphertext (see kdf_store), not baked in, so
 * raising it later is a one-line change that stays compatible with existing installs: the
 * stored value is what decrypts, the constant is only what new installs get. */
#define BC_PBKDF2_ITERATIONS 600000u
#define BC_SALT_SIZE 16
#define BC_KDF_FILE_VERSION 2
#define BC_KDF_NAME "pbkdf2-sha256"

static char g_master_key[BC_KEY_SIZE];
static bool g_secret_ready = false;
static bool g_key_mlocked = false;

/* The per-install salt and iteration count the current key was derived with. Loaded from
 * .key_hash before the key is derived -- the derivation cannot run without them, which is
 * why the file is read in edc_secret_init() and not alongside config.json (which is
 * decrypted with the key that comes out of this). */
static unsigned char g_salt[BC_SALT_SIZE];
static unsigned int g_iterations = BC_PBKDF2_ITERATIONS;

/* PBKDF2-HMAC-SHA256: deliberately slow, salted. Replaces a single HKDF pass, which is a
 * key-derivation function -- fast by design, because its intended input is an
 * already-uniform key -- applied to a human passphrase, where speed is the vulnerability
 * and an empty salt let one precomputed table serve every install. */
static bool derive_key(const char *passphrase, size_t len)
{
    int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)passphrase, len, g_salt,
                                            sizeof(g_salt), g_iterations, BC_KEY_SIZE,
                                            (unsigned char *)g_master_key);
    if (ret != 0)
    {
        log_error("PBKDF2 key derivation failed: %d", ret);
        return false;
    }
    return true;
}

/* Read a password with echo off. This used to be a separate edc_terminal.{c,h}, but of
 * its 7 functions only these 2 had callers (save / restore / raw_mode / raw_restore /
 * vt_enable had zero readers) and the only user is read_passphrase_tty below -- so they
 * were absorbed here and that module disappeared. The logic is verbatim from the old
 * implementation (both POSIX and Windows branches). */
struct secret_tty_state
{
#ifdef _WIN32
    DWORD original_mode;
#else
    struct termios original;
#endif
    bool valid;
};

static int secret_tty_echo_off(int fd, struct secret_tty_state *saved)
{
#ifdef _WIN32
    HANDLE h;
    if (fd == STDIN_FILENO)
        h = GetStdHandle(STD_INPUT_HANDLE);
    else
        return -1;

    if (!GetConsoleMode(h, &saved->original_mode))
        return -1;
    saved->valid = true;

    DWORD mode = saved->original_mode & ~ENABLE_ECHO_INPUT;
    if (!SetConsoleMode(h, mode))
    {
        saved->valid = false;
        return -1;
    }
    return 0;
#else
    if (!isatty(fd))
        return -1;
    if (tcgetattr(fd, &saved->original) != 0)
        return -1;
    saved->valid = true;
    struct termios t = saved->original;
    t.c_lflag &= ~(ECHO | ECHONL);
    if (tcsetattr(fd, TCSANOW, &t) != 0)
    {
        saved->valid = false;
        return -1;
    }
    return 0;
#endif
}

static void secret_tty_echo_restore(int fd, struct secret_tty_state *saved)
{
    if (!saved || !saved->valid)
        return;
#ifdef _WIN32
    HANDLE h = (fd == STDIN_FILENO) ? GetStdHandle(STD_INPUT_HANDLE) : NULL;
    if (!h)
        return;
    SetConsoleMode(h, saved->original_mode);
#else
    tcsetattr(fd, TCSANOW, &saved->original);
#endif
    saved->valid = false;
}

static bool read_passphrase_tty(const char *prompt, char *buf, size_t buf_size)
{
    fprintf(stdout, "%s", prompt);
    fflush(stdout);

    struct secret_tty_state saved = {{0}};
    if (secret_tty_echo_off(STDIN_FILENO, &saved) != 0)
    {
        log_error("secret_tty_echo_off failed");
        return false;
    }

    char *result = fgets(buf, (int)buf_size, stdin);
    secret_tty_echo_restore(STDIN_FILENO, &saved);
    fprintf(stdout, "\n");
    fflush(stdout);

    if (!result)
    {
        log_error("Failed to read passphrase from stdin");
        return false;
    }

    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    if (len == 0)
    {
        log_error("Empty passphrase");
        return false;
    }

    return true;
}

/* Draw a fresh per-install salt from the platform CSPRNG. */
static bool random_salt(void)
{
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    bool ok = false;

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    /* Personalised differently from the cipher's DRBG in edc_secret_encrypt: the two
     * draws must not share a stream. */
    int ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                    (const unsigned char *)"edgeclaw_salt", 13);
    if (ret == 0)
        ret = mbedtls_ctr_drbg_random(&drbg, g_salt, sizeof(g_salt));
    if (ret == 0)
        ok = true;
    else
        log_error("salt generation failed: %d", ret);

    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return ok;
}

static bool lock_master_key(void)
{
    /* Which of the three answers comes back is a platform fact (edc_platform.h): no mlock
     * at all is normal, mlock refusing is worth a warning, and either way the key stays
     * usable -- it is simply not pinned against being swapped out. */
    switch (edc_platform_lock_memory(g_master_key, BC_KEY_SIZE))
    {
    case EDC_LOCK_OK:
        g_key_mlocked = true;
        return true;
    case EDC_LOCK_UNSUPPORTED:
        log_debug("master key is not locked in memory: no mlock on this platform");
        return true;
    case EDC_LOCK_FAILED:
    default:
        log_warn("mlock master key failed: %s", strerror(errno));
        return false;
    }
}

/* ── the on-disk key descriptor ───────────────────────────────────────────────
 * .key_hash used to hold one bare HMAC digest: 64 hex characters, no salt, no record of
 * how the key was produced. It now holds the record the derivation needs to be repeatable
 * and the parameters a future change can migrate from:
 *
 *   v 2
 *   kdf pbkdf2-sha256
 *   iter 600000
 *   salt <32 hex>
 *   verify <64 hex>
 *
 * Line-oriented, not JSON: this module is a dependency-free leaf and the shape is one flat
 * record (see kdf_store).
 *
 * The salt is not a secret (its job is to make one precomputed table useless for every
 * install), so it lives next to the verifier rather than in config.json -- it has to be
 * readable BEFORE the key exists, and config.json is encrypted WITH that key.
 *
 * The verifier stays an HMAC over the derived key rather than a hash of the passphrase:
 * it answers "is this passphrase right" without adding a second copy of the secret, and
 * because the key it is taken over is salted, it is no longer a table shared by everyone
 * who picked the same passphrase. */

/* HMAC-SHA256 over the derived key, hex. The string verifier in .key_hash. */
static bool compute_verify_hex(char out[65])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md)
    {
        log_error("SHA-256 not available for verification");
        return false;
    }

    unsigned char hash[32];
    int ret = mbedtls_md_hmac(md, (const unsigned char *)g_master_key, BC_KEY_SIZE,
                              (const unsigned char *)"edgeclaw_verify", 15, hash);
    if (ret != 0)
    {
        log_error("HMAC for verification failed: %d", ret);
        return false;
    }

    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", hash[i]);
    out[64] = '\0';
    memset(hash, 0, sizeof(hash));
    return true;
}

static void hex_encode(const unsigned char *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + i * 2, 3, "%02x", in[i]);
    out[n * 2] = '\0';
}

/* Parse hex into |out|, which must be exactly |n| bytes. Refuses on any other length or a
 * non-hex character: a silently truncated salt would derive a key that cannot decrypt and
 * report it as a bad passphrase. */
static bool hex_decode(const char *in, unsigned char *out, size_t n)
{
    if (!in || strlen(in) != n * 2)
        return false;
    for (size_t i = 0; i < n; i++)
    {
        int hi = -1, lo = -1;
        for (int k = 0; k < 2; k++)
        {
            char c = in[i * 2 + k];
            int v = (c >= '0' && c <= '9')   ? c - '0'
                    : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                    : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                             : -1;
            if (v < 0)
                return false;
            if (k == 0)
                hi = v;
            else
                lo = v;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return true;
}

static sds kdf_file_path(const char *edc_home)
{
    return edc_path_join(edc_home, BC_KEY_VERIFY_FILE, NULL);
}

/* Is there a record at all? Distinguishes "first run" (mint and store) from "a record
 * exists but kdf_load refused it" (report and stop, rather than overwriting a salt the
 * existing ciphertext may still need). */
static bool kdf_file_exists(const char *edc_home)
{
    sds path = kdf_file_path(edc_home);
    bool exists = edc_file_is_regular(path);
    sdsfree(path);
    return exists;
}

/* Write the record for the CURRENT salt/iterations/key. Overwrites, so switching between
 * env-var and interactive use keeps one authoritative record.
 *
 * Line-oriented rather than JSON on purpose: this module is a deliberately dependency-free
 * leaf (no cJSON), the shape is one flat record, and a fixed "key value" line needs no
 * escaping. */
static bool kdf_store(const char *edc_home)
{
    if (!edc_home)
        return false;

    char verify_hex[65];
    if (!compute_verify_hex(verify_hex))
        return false;

    char salt_hex[BC_SALT_SIZE * 2 + 1];
    hex_encode(g_salt, sizeof(g_salt), salt_hex);

    sds body = sdscatprintf(sdsempty(), "v %d\nkdf %s\niter %u\nsalt %s\nverify %s\n", BC_KDF_FILE_VERSION,
                            BC_KDF_NAME, g_iterations, salt_hex, verify_hex);

    sds path = kdf_file_path(edc_home);
    bool ok = edc_file_write(path, body, -1, NULL);
    sdsfree(body);
    memset(verify_hex, 0, sizeof(verify_hex));
    memset(salt_hex, 0, sizeof(salt_hex));

    if (!ok)
    {
        log_error("Failed to store the key record in %s", path);
        sdsfree(path);
        return false;
    }

#ifndef _WIN32
    /* The verifier is an offline guessing oracle by nature: keep it out of other users'
     * reach (edc_file_write creates 0644). The salt alone would not need this. */
    if (chmod(path, 0600) != 0)
        log_warn("cannot restrict permissions on %s: %s", path, strerror(errno));
#endif
    sdsfree(path);
    return true;
}

/* Load salt/iterations (and optionally the verifier) from |edc_home|. Returns false when
 * the record is missing or unusable; |*have_verify| says whether a verifier was present. */
static bool kdf_load(const char *edc_home, char verify_hex[65], bool *have_verify)
{
    if (have_verify)
        *have_verify = false;

    sds path = kdf_file_path(edc_home);
    EdcError *error = NULL;
    sds contents = edc_file_read(path, &error);
    if (!contents)
    {
        if (error)
            edc_error_free(error);
        sdsfree(path);
        return false;
    }
    sdsfree(path);

    /* A pre-v2 install left 64 bare hex characters here: no salt and no version, so it
     * cannot be reused. Say that instead of "unknown record version -1". */
    const char *probe = contents;
    while (*probe == ' ' || *probe == '\t' || *probe == '\n' || *probe == '\r')
        probe++;
    if (*probe != 'v')
    {
        log_error("%s is not a v%d key record -- it looks like the pre-v2 verification hash, "
                  "which has no salt and cannot be reused; remove it and run setup again",
                  BC_KEY_VERIFY_FILE, BC_KDF_FILE_VERSION);
        sdsfree(contents);
        return false;
    }

    int version = -1;
    unsigned int iterations = 0;
    bool kdf_ok = false;
    unsigned char salt[BC_SALT_SIZE];
    bool salt_ok = false;
    bool ver_ok = false;
    char ver[65] = {0};

    /* Walk the lines by hand: strtok_r is POSIX-only, and this module builds on Windows. */
    char *cur = contents;
    while (cur && *cur)
    {
        char *nl = strchr(cur, '\n');
        if (nl)
            *nl = '\0';

        char *sp = strchr(cur, ' ');
        if (sp)
        {
            *sp = '\0';
            const char *val = sp + 1;
            if (strcmp(cur, "v") == 0)
                version = atoi(val);
            else if (strcmp(cur, "kdf") == 0)
                kdf_ok = (strcmp(val, BC_KDF_NAME) == 0);
            else if (strcmp(cur, "iter") == 0)
                iterations = (unsigned int)strtoul(val, NULL, 10);
            else if (strcmp(cur, "salt") == 0)
                salt_ok = hex_decode(val, salt, sizeof(salt));
            else if (strcmp(cur, "verify") == 0 && strlen(val) == 64)
            {
                snprintf(ver, sizeof(ver), "%s", val);
                ver_ok = true;
            }
        }

        cur = nl ? nl + 1 : NULL;
    }

    bool ok = false;
    if (version != BC_KDF_FILE_VERSION)
        log_error("%s: unsupported record version %d (this build reads v%d); remove it and run setup "
                  "again",
                  BC_KEY_VERIFY_FILE, version, BC_KDF_FILE_VERSION);
    else if (!kdf_ok)
        log_error("%s: unknown or missing kdf (expected '%s')", BC_KEY_VERIFY_FILE, BC_KDF_NAME);
    else if (iterations < 1)
        log_error("%s: no usable iteration count", BC_KEY_VERIFY_FILE);
    else if (!salt_ok)
        log_error("%s: no usable salt", BC_KEY_VERIFY_FILE);
    else
    {
        g_iterations = iterations;
        memcpy(g_salt, salt, sizeof(g_salt));
        if (ver_ok)
        {
            memcpy(verify_hex, ver, sizeof(ver));
            if (have_verify)
                *have_verify = true;
        }
        ok = true;
    }

    memset(salt, 0, sizeof(salt));
    memset(ver, 0, sizeof(ver));
    sdsfree(contents);
    return ok;
}


/* Compare the current key's verifier against the stored one. The caller has already loaded
 * the salt and derived the key, so a mismatch means the passphrase was wrong. */
static bool verify_master_key(const char stored_hex[65])
{
    char actual[65];
    if (!compute_verify_hex(actual))
        return false;

    bool match = (strcmp(stored_hex, actual) == 0);
    if (!match)
        log_error("Invalid passphrase");

    memset(actual, 0, sizeof(actual));
    return match;
}

bool edc_secret_init(bool confirm_password, const char *edc_home)
{
    if (g_secret_ready)
        return true;

    /* Priority 1: env var (scripts / Docker) */
    const char *env_val = getenv(BC_MASTER_KEY_ENV);
    if (env_val && strlen(env_val) > 0)
    {
        char stored_verify[65];
        bool have_verify = false;

        /* The salt has to be the one the stored ciphertext was made with, so an existing
         * record wins. Drawing a fresh salt here would change the key on every launch and
         * nothing encrypted earlier would ever decrypt again. */
        if (edc_home && kdf_load(edc_home, stored_verify, &have_verify))
        {
            if (!derive_key(env_val, strlen(env_val)))
                return false;

            /* Not fatal: env mode is also how CI feeds a throwaway key. But say so, because
             * the symptom otherwise is a decryption failure much further along. */
            if (have_verify && !verify_master_key(stored_verify))
            {
                log_warn("%s does not match the passphrase this install was set up with -- "
                         "values encrypted earlier will NOT decrypt; re-run /connect to store "
                         "the key again",
                         BC_MASTER_KEY_ENV);
                kdf_store(edc_home);
            }
        }
        else
        {
            if (edc_home && kdf_file_exists(edc_home))
            {
                /* A record is there but unusable -- kdf_load has already said why (unknown
                 * version, no salt, pre-v2 hash). Overwriting it would throw away the salt
                 * the stored ciphertext was made with, so stop instead. */
                log_error("refusing to continue with an unusable key record; fix or remove %s",
                          BC_KEY_VERIFY_FILE);
                memset(g_master_key, 0, BC_KEY_SIZE);
                return false;
            }
            if (edc_home)
            {
                /* First run in env mode: mint the install's salt and keep it, so the next
                 * launch derives the same key. */
                if (!random_salt() || !derive_key(env_val, strlen(env_val)) || !kdf_store(edc_home))
                {
                    memset(g_master_key, 0, BC_KEY_SIZE);
                    return false;
                }
            }
            else
            {
                /* No home to store it in: the salt can only be ephemeral, so whatever gets
                 * encrypted now is unreadable next time. */
                log_warn("no edc_home given: the salt is ephemeral, so encrypted values will not "
                         "survive a restart");
                if (!random_salt() || !derive_key(env_val, strlen(env_val)))
                {
                    memset(g_master_key, 0, BC_KEY_SIZE);
                    return false;
                }
            }
        }

        unsetenv(BC_MASTER_KEY_ENV);
        log_info("Master key loaded from %s", BC_MASTER_KEY_ENV);

        lock_master_key();
        g_secret_ready = true;
        return true;
    }

    /* Priority 2: stdin (interactive) */
    if (!isatty(STDIN_FILENO))
    {
        log_error("No terminal for passphrase input"
                  " (set %s for scripting)",
                  BC_MASTER_KEY_ENV);
        return false;
    }

    char buf[MAX_PASSPHRASE];

    if (confirm_password)
    {
        /* First-time setup: prompt twice and confirm match.
         *
         * The two lines before the prompt are the only explanation a new user gets for a
         * password they are about to invent for a program they have not used yet. The
         * prompt alone said nothing about what the passphrase is for, that losing it has
         * a cost, or that this can be skipped -- and it is the FIRST thing a first run
         * prints. */
        fprintf(stderr,
                "EdgeClaw encrypts the API key in config.json with this passphrase.\n"
                "It is not stored anywhere, so it cannot be recovered: forget it and you\n"
                "re-enter the key (settings and history are unaffected). Set %s to skip\n"
                "this prompt.\n",
                BC_MASTER_KEY_ENV);
        if (!read_passphrase_tty("Enter new passphrase: ", buf, sizeof(buf)))
            return false;

        char confirm[MAX_PASSPHRASE];
        if (!read_passphrase_tty("Confirm passphrase: ", confirm, sizeof(confirm)))
        {
            memset(buf, 0, sizeof(buf));
            return false;
        }

        if (strcmp(buf, confirm) != 0)
        {
            log_error("Passphrases do not match");
            memset(buf, 0, sizeof(buf));
            memset(confirm, 0, sizeof(confirm));
            return false;
        }
        memset(confirm, 0, sizeof(confirm));
    }
    else
    {
        if (!read_passphrase_tty("Enter passphrase: ", buf, sizeof(buf)))
            return false;
    }

    size_t len = strlen(buf);
    bool ok = false;

    if (confirm_password)
    {
        /* New passphrase: this install's salt is minted FIRST, because the key depends on
         * it. Storing the record is not optional once a home exists -- without it the next
         * launch would draw a different salt and derive a key that cannot decrypt what this
         * run encrypts. */
        ok = random_salt() && derive_key(buf, len);
        memset(buf, 0, sizeof(buf));
        if (ok && edc_home && !kdf_store(edc_home))
        {
            log_error("Failed to store verification data");
            ok = false;
        }
        if (ok && !edc_home)
            log_warn("no edc_home given: the salt is ephemeral, so encrypted values will not "
                     "survive a restart");
    }
    else
    {
        /* Existing passphrase: the salt and iteration count come from the stored record,
         * and the verifier is what says the passphrase was right. */
        char stored_verify[65];
        bool have_verify = false;
        if (!edc_home || !kdf_load(edc_home, stored_verify, &have_verify))
        {
            memset(buf, 0, sizeof(buf));
            log_error("Cannot verify passphrase: no usable key record — run setup first");
            return false;
        }

        ok = derive_key(buf, len);
        memset(buf, 0, sizeof(buf));
        if (ok && (!have_verify || !verify_master_key(stored_verify)))
            ok = false;
    }

    if (!ok)
    {
        memset(g_master_key, 0, BC_KEY_SIZE);
        return false;
    }

    log_info("Master key loaded from stdin");
    lock_master_key();
    g_secret_ready = true;
    return true;
}

/* Wipes the key. The only such path: the key is deliberately resident for the whole
 * process because /connect re-encrypts at runtime (see the note in edc_secret.h). */
void edc_secret_cleanup(void)
{
    if (g_key_mlocked)
    {
        edc_platform_unlock_memory(g_master_key, BC_KEY_SIZE);
        g_key_mlocked = false;
    }
    memset(g_master_key, 0, BC_KEY_SIZE);
    g_secret_ready = false;
}

sds edc_secret_encrypt(sds plaintext)
{
    if (!plaintext || !g_secret_ready)
        return NULL;

    mbedtls_cipher_context_t cipher;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    unsigned char iv[BC_GCM_IV_SIZE];
    unsigned char *output = NULL;
    char *b64_buf = NULL;
    char *result = NULL;

    const mbedtls_cipher_info_t *cipher_info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_256_GCM);
    if (!cipher_info)
    {
        log_error("AES-256-GCM not available");
        return NULL;
    }

    mbedtls_cipher_init(&cipher);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    int ret =
        mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"edgeclaw_enc", 12);
    if (ret != 0)
    {
        log_error("DRBG seed failed: %d", ret);
        goto cleanup;
    }

    ret = mbedtls_cipher_setup(&cipher, cipher_info);
    if (ret != 0)
    {
        log_error("Cipher setup failed: %d", ret);
        goto cleanup;
    }

    ret = mbedtls_cipher_setkey(&cipher, (const unsigned char *)g_master_key, BC_KEY_SIZE * 8, MBEDTLS_ENCRYPT);
    if (ret != 0)
    {
        log_error("Set key failed: %d", ret);
        goto cleanup;
    }

    ret = mbedtls_ctr_drbg_random(&ctr_drbg, iv, BC_GCM_IV_SIZE);
    if (ret != 0)
    {
        log_error("IV generation failed: %d", ret);
        goto cleanup;
    }

    size_t plain_len = sdslen(plaintext);
    size_t output_len = plain_len + BC_GCM_TAG_SIZE;
    output = EDC_NEW(unsigned char, output_len);
    size_t olen = 0;

    ret = mbedtls_cipher_auth_encrypt_ext(&cipher, iv, BC_GCM_IV_SIZE, NULL, 0, (const unsigned char *)plaintext,
                                          plain_len, output, output_len, &olen, BC_GCM_TAG_SIZE);

    if (ret != 0)
    {
        log_error("Encryption failed: %d", ret);
        free(output);
        output = NULL;
        goto cleanup;
    }

    size_t total = BC_GCM_IV_SIZE + olen;
    unsigned char *packed = EDC_NEW(unsigned char, total);
    memcpy(packed, iv, BC_GCM_IV_SIZE);
    memcpy(packed + BC_GCM_IV_SIZE, output, olen);
    free(output);
    output = NULL;

    size_t b64_len = 0;
    ret = mbedtls_base64_encode(NULL, 0, &b64_len, packed, total);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
    {
        log_error("Base64 size calc failed: %d", ret);
        free(packed);
        goto cleanup;
    }

    b64_buf = EDC_NEW(char, b64_len + 1);
    ret = mbedtls_base64_encode((unsigned char *)b64_buf, b64_len + 1, &b64_len, packed, total);
    if (ret != 0)
    {
        log_error("Base64 encode failed: %d", ret);
        free(packed);
        free(b64_buf);
        b64_buf = NULL;
        goto cleanup;
    }
    b64_buf[b64_len] = '\0';

    free(packed);
    packed = NULL;

    result = sdscatprintf(sdsempty(), "%s%s", BC_SECRET_PREFIX, b64_buf);
    free(b64_buf);

cleanup:
    free(output);
    mbedtls_cipher_free(&cipher);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    return result;
}

sds edc_secret_decrypt(const char *input)
{
    if (!input || !g_secret_ready)
        return NULL;

    size_t prefix_len = strlen(BC_SECRET_PREFIX);
    if (strncmp(input, BC_SECRET_PREFIX, prefix_len) != 0)
    {
        /* Carries the family tag but not a version this build implements: refuse it. The
         * alternative -- passing it through as a plaintext key -- would hand a ciphertext
         * to the provider as the API key and, via /connect, re-encrypt over the real one. */
        if (strncmp(input, BC_SECRET_PREFIX_FAMILY, strlen(BC_SECRET_PREFIX_FAMILY)) == 0)
        {
            log_error("encrypted value uses an unsupported format (this build reads '%s')",
                      BC_SECRET_PREFIX);
            return NULL;
        }
        return sdsnew(input);
    }

    const char *b64_input = input + prefix_len;
    if (strlen(b64_input) == 0)
        return NULL;

    mbedtls_cipher_context_t cipher;
    unsigned char *packed = NULL;
    unsigned char *plain_buf = NULL;
    sds result = NULL;

    const mbedtls_cipher_info_t *cipher_info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_256_GCM);
    if (!cipher_info)
    {
        log_error("AES-256-GCM not available");
        return NULL;
    }

    size_t b64_len = strlen(b64_input);
    size_t packed_len = 0;

    int ret = mbedtls_base64_decode(NULL, 0, &packed_len, (const unsigned char *)b64_input, b64_len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
    {
        log_error("Base64 decode size calc failed: %d", ret);
        return NULL;
    }

    if (packed_len < BC_GCM_IV_SIZE + BC_GCM_TAG_SIZE)
    {
        log_error("Encrypted data too small: %zu bytes", packed_len);
        return NULL;
    }

    packed = EDC_NEW(unsigned char, packed_len);
    ret = mbedtls_base64_decode(packed, packed_len, &packed_len, (const unsigned char *)b64_input, b64_len);
    if (ret != 0)
    {
        log_error("Base64 decode failed: %d", ret);
        free(packed);
        return NULL;
    }

    const unsigned char *iv = packed;
    const unsigned char *input_data = packed + BC_GCM_IV_SIZE;
    size_t input_len = packed_len - BC_GCM_IV_SIZE;
    size_t plain_len = input_len - BC_GCM_TAG_SIZE;

    plain_buf = EDC_NEW(unsigned char, plain_len + 1);

    mbedtls_cipher_init(&cipher);

    ret = mbedtls_cipher_setup(&cipher, cipher_info);
    if (ret != 0)
    {
        log_error("Cipher setup failed: %d", ret);
        free(packed);
        free(plain_buf);
        return NULL;
    }

    ret = mbedtls_cipher_setkey(&cipher, (const unsigned char *)g_master_key, BC_KEY_SIZE * 8, MBEDTLS_DECRYPT);
    if (ret != 0)
    {
        log_error("Set key failed: %d", ret);
        mbedtls_cipher_free(&cipher);
        free(packed);
        free(plain_buf);
        return NULL;
    }

    size_t olen = 0;
    ret = mbedtls_cipher_auth_decrypt_ext(&cipher, iv, BC_GCM_IV_SIZE, NULL, 0, input_data, input_len, plain_buf,
                                          plain_len, &olen, BC_GCM_TAG_SIZE);

    if (ret != 0)
    {
        log_error("Decryption failed (wrong key or corrupted data): %d", ret);
        mbedtls_cipher_free(&cipher);
        free(packed);
        free(plain_buf);
        return NULL;
    }

    plain_buf[olen] = '\0';
    result = sdsnew((const char *)plain_buf);

    memset(plain_buf, 0, plain_len + 1);
    free(plain_buf);
    free(packed);

    mbedtls_cipher_free(&cipher);

    return result;
}
