#include "edc_secret.h"
#include "edc_strutil.h"
#include "log.h"
#include <errno.h>
#include <mbedtls/base64.h>
#include <mbedtls/cipher.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>

#define BC_KEY_SIZE 32
#define BC_GCM_IV_SIZE 12
#define BC_GCM_TAG_SIZE 16
#define BC_MASTER_KEY_ENV "EDGECLAW_MASTER_KEY"
#define MAX_PASSPHRASE 1024
#define BC_KEY_VERIFY_FILE ".key_hash"

static char g_master_key[BC_KEY_SIZE];
static bool g_secret_ready = FALSE;
static bool g_key_mlocked = FALSE;

static bool derive_key(const char *passphrase, size_t len)
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md)
    {
        log_error("SHA-256 not available");
        return FALSE;
    }

    int ret = mbedtls_hkdf(md, NULL, 0, (const unsigned char *)passphrase, len, (const unsigned char *)"edgeclaw_master_key", 19,
                           (unsigned char *)g_master_key, BC_KEY_SIZE);
    if (ret != 0)
    {
        log_error("HKDF key derivation failed: %d", ret);
        return FALSE;
    }
    return TRUE;
}

static bool read_passphrase_tty(const char *prompt, char *buf, size_t buf_size)
{
    fprintf(stdout, "%s", prompt);
    fflush(stdout);

    struct termios old_term, new_term;
    if (tcgetattr(STDIN_FILENO, &old_term) != 0)
    {
        log_error("tcgetattr failed");
        return FALSE;
    }

    new_term = old_term;
    new_term.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);

    char *result = fgets(buf, (int)buf_size, stdin);
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    fprintf(stdout, "\n");
    fflush(stdout);

    if (!result)
    {
        log_error("Failed to read passphrase from stdin");
        return FALSE;
    }

    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    if (len == 0)
    {
        log_error("Empty passphrase");
        return FALSE;
    }

    return TRUE;
}

static bool lock_master_key(void)
{
    if (mlock(g_master_key, BC_KEY_SIZE) != 0)
    {
        log_warn("mlock master key failed: %s", strerror(errno));
        return FALSE;
    }
    g_key_mlocked = TRUE;
    return TRUE;
}

static bool store_verify_hash(const char *edc_home)
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md)
    {
        log_error("SHA-256 not available for verification");
        return FALSE;
    }

    unsigned char hash[32];
    int ret =
        mbedtls_md_hmac(md, (const unsigned char *)g_master_key, BC_KEY_SIZE, (const unsigned char *)"edgeclaw_verify", 15, hash);
    if (ret != 0)
    {
        log_error("HMAC for verification failed: %d", ret);
        return FALSE;
    }

    char hash_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hash_hex + i * 2, 3, "%02x", hash[i]);
    hash_hex[64] = '\0';

    char *path = g_build_filename(edc_home, BC_KEY_VERIFY_FILE, NULL);
    GError *error = NULL;
    bool ok = g_file_set_contents(path, hash_hex, -1, &error);
    if (!ok && error)
    {
        log_error("Failed to store verification hash: %s", error->message);
        g_error_free(error);
    }
    free(path);
    memset(hash, 0, sizeof(hash));
    memset(hash_hex, 0, sizeof(hash_hex));
    return ok;
}

static bool verify_master_key(const char *edc_home)
{
    char *path = g_build_filename(edc_home, BC_KEY_VERIFY_FILE, NULL);

    char *contents = NULL;
    size_t length = 0;
    GError *error = NULL;
    if (!g_file_get_contents(path, &contents, &length, &error))
    {
        if (error)
            g_error_free(error);
        free(path);
        log_error("Cannot verify passphrase: verification file not found"
                  " — run setup first");
        return FALSE;
    }
    free(path);

    while (length > 0 && (contents[length - 1] == '\n' || contents[length - 1] == '\r'))
        contents[--length] = '\0';

    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md)
    {
        free(contents);
        return FALSE;
    }

    unsigned char expected[32];
    int ret =
        mbedtls_md_hmac(md, (const unsigned char *)g_master_key, BC_KEY_SIZE, (const unsigned char *)"edgeclaw_verify", 15, expected);
    if (ret != 0)
    {
        free(contents);
        return FALSE;
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
    free(contents);
    return match;
}

bool edc_secret_init(bool confirm_password, const char *edc_home)
{
    if (g_secret_ready)
        return TRUE;

    /* Priority 1: env var (scripts / Docker) */
    const char *env_val = g_getenv(BC_MASTER_KEY_ENV);
    if (env_val && strlen(env_val) > 0)
    {
        if (!derive_key(env_val, strlen(env_val)))
            return FALSE;

        g_unsetenv(BC_MASTER_KEY_ENV);
        log_info("Master key loaded from %s", BC_MASTER_KEY_ENV);

        lock_master_key();
        g_secret_ready = TRUE;
        return TRUE;
    }

    /* Priority 2: stdin (interactive) */
    if (!isatty(STDIN_FILENO))
    {
        log_error("No terminal for passphrase input"
                  " (set %s for scripting)",
                  BC_MASTER_KEY_ENV);
        return FALSE;
    }

    char buf[MAX_PASSPHRASE];

    if (confirm_password)
    {
        /* First-time setup: prompt twice and confirm match */
        if (!read_passphrase_tty("Enter new passphrase: ", buf, sizeof(buf)))
            return FALSE;

        char confirm[MAX_PASSPHRASE];
        if (!read_passphrase_tty("Confirm passphrase: ", confirm, sizeof(confirm)))
        {
            memset(buf, 0, sizeof(buf));
            return FALSE;
        }

        if (strcmp(buf, confirm) != 0)
        {
            log_error("Passphrases do not match");
            memset(buf, 0, sizeof(buf));
            memset(confirm, 0, sizeof(confirm));
            return FALSE;
        }
        memset(confirm, 0, sizeof(confirm));
    }
    else
    {
        if (!read_passphrase_tty("Enter passphrase: ", buf, sizeof(buf)))
            return FALSE;
    }

    size_t len = strlen(buf);
    if (!derive_key(buf, len))
    {
        memset(buf, 0, sizeof(buf));
        return FALSE;
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
                return FALSE;
            }
        }
        else
        {
            if (!verify_master_key(edc_home))
            {
                memset(g_master_key, 0, BC_KEY_SIZE);
                return FALSE;
            }
        }
    }

    log_info("Master key loaded from stdin");
    lock_master_key();
    g_secret_ready = TRUE;
    return TRUE;
}

void edc_secret_cleanup(void)
{
    if (g_key_mlocked)
    {
        munlock(g_master_key, BC_KEY_SIZE);
        g_key_mlocked = FALSE;
    }
    memset(g_master_key, 0, BC_KEY_SIZE);
    g_secret_ready = FALSE;
}

void edc_secret_lockdown(void)
{
    if (!g_secret_ready)
        return;

    if (g_key_mlocked)
    {
        munlock(g_master_key, BC_KEY_SIZE);
        g_key_mlocked = FALSE;
    }
    memset(g_master_key, 0, BC_KEY_SIZE);
    g_secret_ready = FALSE;

    log_info("Master key wiped — secret module locked down");
}

char *edc_secret_encrypt(const char *plaintext)
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

    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"edgeclaw_enc", 12);
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

    size_t plain_len = strlen(plaintext);
    size_t output_len = plain_len + BC_GCM_TAG_SIZE;
    output = g_new(unsigned char, output_len);
    size_t olen = 0;

    ret = mbedtls_cipher_auth_encrypt_ext(&cipher, iv, BC_GCM_IV_SIZE, NULL, 0, (const unsigned char *)plaintext, plain_len,
                                          output, output_len, &olen, BC_GCM_TAG_SIZE);

    if (ret != 0)
    {
        log_error("Encryption failed: %d", ret);
        free(output);
        output = NULL;
        goto cleanup;
    }

    size_t total = BC_GCM_IV_SIZE + olen;
    unsigned char *packed = g_new(unsigned char, total);
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

    b64_buf = g_new(char, b64_len + 1);
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

    result = edc_strdup_printf("%s%s", BC_SECRET_PREFIX, b64_buf);
    free(b64_buf);

cleanup:
    free(output);
    mbedtls_cipher_free(&cipher);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    return result;
}

char *edc_secret_decrypt(const char *input)
{
    if (!input || !g_secret_ready)
        return NULL;

    size_t prefix_len = strlen(BC_SECRET_PREFIX);
    if (strncmp(input, BC_SECRET_PREFIX, prefix_len) != 0)
        return strdup(input);

    const char *b64_input = input + prefix_len;
    if (strlen(b64_input) == 0)
        return NULL;

    mbedtls_cipher_context_t cipher;
    unsigned char *packed = NULL;
    unsigned char *plain_buf = NULL;
    char *result = NULL;

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

    packed = g_new(unsigned char, packed_len);
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

    plain_buf = g_new(unsigned char, plain_len + 1);

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
    result = strdup((const char *)plain_buf);

    memset(plain_buf, 0, plain_len + 1);
    free(plain_buf);
    free(packed);

    mbedtls_cipher_free(&cipher);

    return result;
}
