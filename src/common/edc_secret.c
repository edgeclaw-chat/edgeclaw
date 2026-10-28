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
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <stdio.h>
#include <string.h>
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

static char g_master_key[BC_KEY_SIZE];
static bool g_secret_ready = false;
static bool g_key_mlocked = false;

static bool derive_key(const char *passphrase, size_t len)
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md)
    {
        log_error("SHA-256 not available");
        return false;
    }

    int ret =
        mbedtls_hkdf(md, NULL, 0, (const unsigned char *)passphrase, len, (const unsigned char *)"edgeclaw_master_key",
                     19, (unsigned char *)g_master_key, BC_KEY_SIZE);
    if (ret != 0)
    {
        log_error("HKDF key derivation failed: %d", ret);
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

static bool store_verify_hash(const char *edc_home)
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

    char hash_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hash_hex + i * 2, 3, "%02x", hash[i]);
    hash_hex[64] = '\0';

    sds path = edc_path_join(edc_home, BC_KEY_VERIFY_FILE, NULL);
    bool ok = edc_file_write(path, hash_hex, -1, NULL);
    if (!ok)
    {
        log_error("Failed to store verification hash");
    }
    sdsfree(path);
    memset(hash, 0, sizeof(hash));
    memset(hash_hex, 0, sizeof(hash_hex));
    return ok;
}

static bool verify_master_key(const char *edc_home)
{
    sds path = edc_path_join(edc_home, BC_KEY_VERIFY_FILE, NULL);

    EdcError *error = NULL;
    sds contents = edc_file_read(path, &error);
    if (!contents)
    {
        if (error)
            edc_error_free(error);
        sdsfree(path);
        log_error("Cannot verify passphrase: verification file not found"
                  " — run setup first");
        return false;
    }
    sdsfree(path);

    sdstrim(contents, "\n\r");

    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md)
    {
        sdsfree(contents);
        return false;
    }

    unsigned char expected[32];
    int ret = mbedtls_md_hmac(md, (const unsigned char *)g_master_key, BC_KEY_SIZE,
                              (const unsigned char *)"edgeclaw_verify", 15, expected);
    if (ret != 0)
    {
        sdsfree(contents);
        return false;
    }

    char expected_hex[96] = {0};
    for (int i = 0; i < 32; i++)
        snprintf(expected_hex + i * 2, 3, "%02x", expected[i]);
    expected_hex[64] = '\0';

    bool match = (strcmp(contents, expected_hex) == 0);
    if (!match)
        log_error("Invalid passphrase");

    memset(expected, 0, sizeof(expected));
    memset(expected_hex, 0, sizeof(expected_hex));
    sdsfree(contents);
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
        if (!derive_key(env_val, strlen(env_val)))
            return false;

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
        /* First-time setup: prompt twice and confirm match */
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
    if (!derive_key(buf, len))
    {
        memset(buf, 0, sizeof(buf));
        return false;
    }

    memset(buf, 0, sizeof(buf));

    if (edc_home)
    {
        if (confirm_password)
        {
            if (!store_verify_hash(edc_home))
            {
                log_error("Failed to store verification data");
                memset(g_master_key, 0, BC_KEY_SIZE);
                return false;
            }
        }
        else
        {
            if (!verify_master_key(edc_home))
            {
                memset(g_master_key, 0, BC_KEY_SIZE);
                return false;
            }
        }
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
        return sdsnew(input);

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
