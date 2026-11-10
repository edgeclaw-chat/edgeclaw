/* test_secret: the passphrase-verification and encrypt/decrypt paths.
 *
 * These were the last unexercised part of edc_secret.c, and they are the security-critical
 * half: store_verify_hash() is what a first run writes, verify_master_key() is what decides
 * whether a returning user typed the right passphrase, and encrypt/decrypt are what protect
 * the stored API key. None of it ran in any test.
 *
 * The obstacle is that the only way in is edc_secret_init(), which calls read_passphrase_tty()
 * and needs a terminal. Rather than stub the reader out (that would leave the reader itself
 * untested, which is most of the risk), this test hands the process a REAL pty: the slave
 * becomes fd 0, the passphrase is written to the master, and the module's own echo-off code
 * runs against it. So the coverage is of the real path, not a seam.
 *
 * edc_secret_init() is one-shot per process (it returns early when the key is already
 * loaded), so each case starts with edc_secret_cleanup() -- which is also what makes the
 * "wrong passphrase" and "different master key" cases possible.
 *
 * Uses a temp directory under $TMPDIR for .key_hash. No network, no prompt, no env var.
 */

#include "edc_config.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_path.h"
#include "edc_secret.h"
#include "log.h"
#include "sds.h"

#include <inttypes.h> /* PRId64 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
/* openpty() is declared in <util.h> on macOS (in libSystem) and in <pty.h> on Linux
 * (in libutil). Same function, same signature; only the header and the library differ. */
#if defined(__APPLE__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#include "test_fs_util.h"
#endif

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: %s\n", msg); g_failed++; } \
    else { printf("    ok: %s\n", msg); g_passed++; } \
} while (0)

static char g_dir[512];

/* ── capturing what the module says ────────────────────────────────────────────
 * Several of these cases differ only in the sentence logged ("Invalid passphrase" vs
 * "verification file not found"), so the message is asserted, not just the bool. */
static char g_log[2048];

static void capture_log(log_Event *ev)
{
    if (ev->level != LOG_ERROR)
        return;
    const size_t used = strlen(g_log);
    if (used + 1 >= sizeof(g_log))
        return;
    vsnprintf(g_log + used, sizeof(g_log) - used, ev->fmt, ev->ap);
}

/* ── a terminal for the process to read from ───────────────────────────────────
 * fed before edc_secret_init() is called: the lines sit in the pty buffer until fgets()
 * consumes them, so there is no timing to get wrong. */
static int g_master = -1;
static int g_saved_stdin = -1;

static int tty_begin(void)
{
    int master = -1, slave = -1;
    if (openpty(&master, &slave, NULL, NULL, NULL) != 0)
        return -1;

    g_saved_stdin = dup(STDIN_FILENO);
    if (g_saved_stdin < 0)
    {
        close(master);
        close(slave);
        return -1;
    }
    if (dup2(slave, STDIN_FILENO) < 0)
    {
        close(master);
        close(slave);
        close(g_saved_stdin);
        return -1;
    }
    close(slave); /* fd 0 keeps the terminal open */
    g_master = master;

    /* fgets() reads through the `stdin` FILE, not the fd, and a previous case may have hit
     * end-of-input on it -- once the EOF indicator is set, fgets returns NULL without
     * touching fd 0 again, even though fd 0 is now a fresh terminal. The product never
     * meets this (it initialises once per process); a test that re-initialises does. */
    clearerr(stdin);
    return 0;
}

static void tty_feed(const char *lines)
{
    if (g_master >= 0)
    {
        const ssize_t ignored = write(g_master, lines, strlen(lines));
        (void)ignored;
    }
}

static void tty_end(void)
{
    if (g_saved_stdin >= 0)
    {
        dup2(g_saved_stdin, STDIN_FILENO);
        close(g_saved_stdin);
        g_saved_stdin = -1;
    }
    if (g_master >= 0)
    {
        close(g_master);
        g_master = -1;
    }
}

/* Run one edc_secret_init() against a pty carrying |input|. Returns its result and leaves
 * the terminal restored. */
static bool init_with_input(bool confirm, const char *edc_home, const char *input)
{
    edc_secret_cleanup();
    unsetenv("EDGECLAW_MASTER_KEY");

    if (tty_begin() != 0)
    {
        printf("    (skipped: cannot open a pty)\n");
        return false;
    }
    tty_feed(input);
    const bool ok = edc_secret_init(confirm, edc_home);
    tty_end();
    return ok;
}

/* ── helpers ───────────────────────────────────────────────────────────────── */
static void reset_dir(void)
{
    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (test_sh(cmd) != 0)
        fprintf(stderr, "    (warning: could not reset %s)\n", g_dir);
}

static bool file_exists(const char *dir, const char *name)
{
    sds p = edc_path_join(dir, name, NULL);
    struct stat st;
    const bool ok = stat(p, &st) == 0 && S_ISREG(st.st_mode);
    sdsfree(p);
    return ok;
}

static sds read_hash_file(const char *dir)
{
    sds p = edc_path_join(dir, ".key_hash", NULL);
    EdcError *e = NULL;
    sds out = edc_file_read(p, &e);
    if (e)
        edc_error_free(e);
    sdsfree(p);
    return out;
}

/* Pull one "key value" line out of the key record. Returns a fresh sds or NULL. */
static sds record_field(const char *record, const char *key)
{
    if (!record)
        return NULL;
    const size_t klen = strlen(key);
    const char *cur = record;
    while (cur && *cur)
    {
        const char *nl = strchr(cur, '\n');
        const size_t len = nl ? (size_t)(nl - cur) : strlen(cur);
        if (len > klen && strncmp(cur, key, klen) == 0 && cur[klen] == ' ')
            return sdsnewlen(cur + klen + 1, len - klen - 1);
        cur = nl ? nl + 1 : NULL;
    }
    return NULL;
}

static bool is_lower_hex(const sds s, size_t n)
{
    if (!s || sdslen(s) != n)
        return false;
    for (size_t i = 0; i < n; i++)
    {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

/* ── 1. first-time setup writes the key record ─────────────────────────────── */
static void test_setup_writes_hash(void)
{
    printf("\n── first-time setup writes the key record ──\n");
    reset_dir();

    CHECK(init_with_input(true, g_dir, "correct horse\ncorrect horse\n"),
          "init(confirm) accepts a passphrase typed twice");
    CHECK(file_exists(g_dir, ".key_hash"), ".key_hash was written");

    sds rec = read_hash_file(g_dir);
    sds v = record_field(rec, "v");
    sds kdf = record_field(rec, "kdf");
    sds iter = record_field(rec, "iter");
    sds salt = record_field(rec, "salt");
    sds verify = record_field(rec, "verify");

    CHECK(v && strcmp(v, "2") == 0, "the record is version 2");
    CHECK(kdf && strcmp(kdf, "pbkdf2-sha256") == 0, "the record names PBKDF2-HMAC-SHA256");
    /* 600k is the floor the code ships with; the check is >= so raising the constant does
     * not silently weaken it. */
    CHECK(iter && atol(iter) >= 600000, "the iteration count is the hardened 600k");
    CHECK(is_lower_hex(salt, 32), "the salt is 16 bytes of lowercase hex");
    CHECK(is_lower_hex(verify, 64), "the verifier is a 32-byte HMAC in lowercase hex");
    /* The file used to BE one bare digest. That shape had no salt, which made one
     * precomputed table serve every install -- the reason for this rewrite. */
    CHECK(rec && sdslen(rec) != 64 && strchr(rec, '\n') != NULL,
          "the file is a record, not one bare digest");

    sdsfree(v);
    sdsfree(kdf);
    sdsfree(iter);
    sdsfree(salt);
    sdsfree(verify);
    sdsfree(rec);
}

/* ── 2. the same passphrase verifies ──────────────────────────────────────── */
static void test_verify_accepts_correct_passphrase(void)
{
    printf("\n── verifying the right passphrase ──\n");
    /* g_dir still holds the hash from the setup case. */
    CHECK(init_with_input(false, g_dir, "correct horse\n"),
          "init(verify) accepts the passphrase that setup stored");
}

/* ── 3. a wrong passphrase is refused, and says so ────────────────────────── */
static void test_verify_rejects_wrong_passphrase(void)
{
    printf("\n── verifying a wrong passphrase ──\n");
    g_log[0] = '\0';
    CHECK(!init_with_input(false, g_dir, "wrong horse\n"), "init(verify) refuses a wrong passphrase");
    printf("    log: %s\n", g_log);
    CHECK(strstr(g_log, "Invalid passphrase") != NULL, "and names the passphrase as the problem");
}

/* ── 4. no record at all: the message points at setup, not at the passphrase ── */
static void test_verify_without_hash(void)
{
    printf("\n── verifying when setup never ran ──\n");
    reset_dir();

    g_log[0] = '\0';
    CHECK(!init_with_input(false, g_dir, "anything\n"), "init(verify) fails without a .key_hash");
    printf("    log: %s\n", g_log);
    CHECK(strstr(g_log, "run setup first") != NULL,
          "and says setup has not run, instead of blaming the passphrase");
    CHECK(strstr(g_log, "Invalid passphrase") == NULL, "it does not claim the passphrase was wrong");
}

/* ── 5. the two prompts have to match ─────────────────────────────────────── */
static void test_confirm_mismatch_rejected(void)
{
    printf("\n── first-time setup with a typo ──\n");
    reset_dir();

    g_log[0] = '\0';
    CHECK(!init_with_input(true, g_dir, "first try\nsecond try\n"),
          "init(confirm) refuses two different passphrases");
    printf("    log: %s\n", g_log);
    CHECK(strstr(g_log, "do not match") != NULL, "and says they did not match");
    CHECK(!file_exists(g_dir, ".key_hash"), "no hash is written for a rejected setup");
}

/* ── 6. an empty passphrase is refused ────────────────────────────────────── */
static void test_empty_passphrase_rejected(void)
{
    printf("\n── empty passphrase ──\n");
    reset_dir();

    g_log[0] = '\0';
    CHECK(!init_with_input(false, g_dir, "\n"), "init refuses an empty passphrase");
    printf("    log: %s\n", g_log);
    CHECK(strstr(g_log, "Empty passphrase") != NULL, "and says it was empty");
}

/* ── 7. encrypt / decrypt round trip, and a ciphertext from another key ───── */
static void test_encrypt_decrypt(void)
{
    printf("\n── encrypt / decrypt ──\n");
    reset_dir();

    if (!init_with_input(true, g_dir, "key one\nkey one\n"))
    {
        printf("    (skipped: setup failed)\n");
        return;
    }

    sds plain = sdsnew("sk-the-users-api-key");
    sds enc = edc_secret_encrypt(plain);
    CHECK(enc != NULL, "encrypt returns a ciphertext");
    CHECK(enc && strncmp(enc, BC_SECRET_PREFIX, strlen(BC_SECRET_PREFIX)) == 0,
          "the ciphertext carries the recognisable prefix");

    sds back = enc ? edc_secret_decrypt(enc) : NULL;
    CHECK(back && strcmp(back, plain) == 0, "decrypt returns the original key");

    /* Not a prefix: decryption passes it through untouched, so a plaintext api_key in an
     * old config keeps working. */
    sds passthrough = edc_secret_decrypt(plain);
    CHECK(passthrough && strcmp(passthrough, plain) == 0,
          "a value without the prefix is passed through unchanged");

    /* The interesting failure: the same ciphertext under a different master key must not
     * decrypt. This is what makes the stored key safe if the passphrase changes. */
    sds enc_copy = enc ? sdsdup(enc) : NULL;
    edc_secret_cleanup();
    if (enc_copy && init_with_input(true, g_dir, "key two\nkey two\n"))
    {
        sds wrong = edc_secret_decrypt(enc_copy);
        CHECK(wrong == NULL, "a ciphertext from another master key does not decrypt");
        sdsfree(wrong);
    }
    else
    {
        printf("    (skipped: could not re-key)\n");
    }

    sdsfree(enc_copy);
    sdsfree(passthrough);
    sdsfree(back);
    sdsfree(enc);
    sdsfree(plain);
}

/* ── 8. the env-var mode (scripts / Docker) ──────────────────────────────── */
static void test_env_var_mode(void)
{
    printf("\n── master key from the environment ──\n");
    reset_dir();
    edc_secret_cleanup();

    /* The non-interactive path. No passphrase prompt, but the SALT still has to be kept:
     * it is an input to the key, so a fresh salt on the next launch would derive a
     * different key and nothing encrypted before could ever be read again. */
    setenv("EDGECLAW_MASTER_KEY", "from-the-environment", 1);
    CHECK(edc_secret_init(false, g_dir), "init succeeds from EDGECLAW_MASTER_KEY without a terminal");
    CHECK(getenv("EDGECLAW_MASTER_KEY") == NULL, "the variable is unset once the key is loaded");
    CHECK(file_exists(g_dir, ".key_hash"), "env-var mode records the salt it derived with");

    sds rec = read_hash_file(g_dir);
    sds salt_first = record_field(rec, "salt");
    sdsfree(rec);

    /* And the process can use the key it loaded. */
    sds plain = sdsnew("sk-from-env");
    sds enc = edc_secret_encrypt(plain);
    CHECK(enc != NULL, "the key loaded from the environment works for encryption");
    sdsfree(plain);

    /* A second launch with the same env key must land on the same salt -- that is the
     * whole point of persisting it in this mode. */
    edc_secret_cleanup();
    setenv("EDGECLAW_MASTER_KEY", "from-the-environment", 1);
    CHECK(edc_secret_init(false, g_dir), "a second env-var init succeeds");
    rec = read_hash_file(g_dir);
    sds salt_second = record_field(rec, "salt");
    sdsfree(rec);
    CHECK(salt_first && salt_second && strcmp(salt_first, salt_second) == 0,
          "the salt is reused, not re-minted on every launch");

    sds back = enc ? edc_secret_decrypt(enc) : NULL;
    CHECK(back && strcmp(back, "sk-from-env") == 0,
          "a value encrypted on the first launch still decrypts on the second");
    if (back)
        sdsfree(back);

    sdsfree(salt_first);
    sdsfree(salt_second);
    sdsfree(enc);
}

/* ── 8b. the salt is per install ─────────────────────────────────────────── */
/* Two installs that pick the SAME passphrase must still get different keys. That is what
 * the salt buys, and what the old empty-salt derivation gave away: one precomputed
 * passphrase->key table served every install, and the verifier in .key_hash was the same
 * bytes everywhere. */
static void test_salt_is_per_install(void)
{
    printf("\n── the salt is per install ──\n");

    reset_dir();
    edc_secret_cleanup();
    if (!init_with_input(true, g_dir, "same passphrase\nsame passphrase\n"))
    {
        printf("    (skipped: first setup failed)\n");
        return;
    }
    sds rec = read_hash_file(g_dir);
    sds salt_a = record_field(rec, "salt");
    sds verify_a = record_field(rec, "verify");
    sdsfree(rec);

    reset_dir();
    edc_secret_cleanup();
    if (!init_with_input(true, g_dir, "same passphrase\nsame passphrase\n"))
    {
        printf("    (skipped: second setup failed)\n");
        sdsfree(salt_a);
        sdsfree(verify_a);
        return;
    }
    rec = read_hash_file(g_dir);
    sds salt_b = record_field(rec, "salt");
    sds verify_b = record_field(rec, "verify");
    sdsfree(rec);

    CHECK(salt_a && salt_b && strcmp(salt_a, salt_b) != 0,
          "two installs with one passphrase get different salts");
    CHECK(verify_a && verify_b && strcmp(verify_a, verify_b) != 0,
          "and therefore different verifiers (no shared passphrase->hash table)");

    sdsfree(salt_a);
    sdsfree(verify_a);
    sdsfree(salt_b);
    sdsfree(verify_b);
}

/* ── 8c. a version this build does not implement is refused ──────────────── */
static void test_unknown_prefix_refused(void)
{
    printf("\n── a format version this build does not implement ──\n");
    reset_dir();
    edc_secret_cleanup();
    if (!init_with_input(true, g_dir, "key four\nkey four\n"))
    {
        printf("    (skipped: setup failed)\n");
        return;
    }

    /* Carries the family tag but an unknown version. Treating it as "not encrypted" would
     * hand a ciphertext to the provider as the API key, and /connect could then re-encrypt
     * over the real one. */
    sds out = edc_secret_decrypt("EDGECLAW_ENC:v1:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    CHECK(out == NULL, "a value from another format version is refused, not passed through");
    if (out)
        sdsfree(out);

    /* No tag at all is still a plaintext key: hand-edited config.json files rely on it. */
    out = edc_secret_decrypt("sk-plain-text-key");
    CHECK(out && strcmp(out, "sk-plain-text-key") == 0, "a value with no tag is still plaintext");
    if (out)
        sdsfree(out);
}

/* ── 8d. a pre-v2 record cannot be reused ────────────────────────────────── */
static void test_pre_v2_record_refused(void)
{
    printf("\n── the pre-v2 verification hash ──\n");
    reset_dir();
    edc_secret_cleanup();

    /* What the previous release wrote: one bare digest, no salt. There is no way to derive
     * the same key from it, so it has to be reported rather than guessed at. */
    sds p = edc_path_join(g_dir, ".key_hash", NULL);
    const char *legacy = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    CHECK(edc_file_write(p, legacy, -1, NULL), "a legacy record can be planted");
    sdsfree(p);

    g_log[0] = '\0';
    CHECK(!init_with_input(false, g_dir, "anything\n"), "init(verify) refuses a pre-v2 record");
    printf("    log: %s\n", g_log);
    CHECK(strstr(g_log, "no salt") != NULL || strstr(g_log, "pre-v2") != NULL,
          "and explains that the old hash carries no salt");
}

/* ── 9. using the module without a key ───────────────────────────────────── */
static void test_without_key(void)
{
    printf("\n── no key loaded ──\n");
    edc_secret_cleanup();
    unsetenv("EDGECLAW_MASTER_KEY");

    sds plain = sdsnew("sk-whatever");
    CHECK(edc_secret_encrypt(plain) == NULL, "encrypt returns NULL when no key is loaded");
    CHECK(edc_secret_encrypt(NULL) == NULL, "encrypt(NULL) returns NULL");
    CHECK(edc_secret_decrypt(BC_SECRET_PREFIX "AAAA") == NULL,
          "decrypt returns NULL when no key is loaded");
    sdsfree(plain);
}

/* ── 10. a corrupted ciphertext is refused, not guessed at ───────────────── */
static void test_corrupt_ciphertext(void)
{
    printf("\n── corrupt ciphertext ──\n");
    reset_dir();
    if (!init_with_input(true, g_dir, "key three\nkey three\n"))
    {
        printf("    (skipped: setup failed)\n");
        return;
    }

    /* Too short to hold an IV + tag: rejected before any crypto runs. */
    CHECK(edc_secret_decrypt(BC_SECRET_PREFIX "AAAA") == NULL,
          "a too-short ciphertext is refused");

    /* Carries the prefix but is not base64: rejected by the decoder. */
    CHECK(edc_secret_decrypt(BC_SECRET_PREFIX "not base64 at all!!") == NULL,
          "a non-base64 ciphertext is refused");

    /* base64 that decodes, but not to a usable AES-GCM payload. */
    CHECK(edc_secret_decrypt(BC_SECRET_PREFIX "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") == NULL,
          "a wrong-length ciphertext is refused");
}

/* ── 11. end of input at the prompt ──────────────────────────────────────── */
static void test_prompt_eof(void)
{
    printf("\n── the prompt hits end of input ──\n");
    reset_dir();
    edc_secret_cleanup();
    unsetenv("EDGECLAW_MASTER_KEY");

    if (tty_begin() != 0)
    {
        printf("    (skipped: cannot open a pty)\n");
        return;
    }
    /* Ctrl-D on an empty line is the terminal's end-of-input: the slave stays a terminal,
     * so isatty() still holds and fgets() is the thing that fails -- which is the branch
     * this case is about. (Closing the master instead would make isatty() false and take
     * the earlier "no terminal" path.) */
    tty_feed("\x04");

    g_log[0] = '\0';
    const bool ok = edc_secret_init(false, g_dir);
    tty_end();
    printf("    log: %s\n", g_log);
    CHECK(!ok, "init fails when the terminal gives no passphrase");
    CHECK(strstr(g_log, "Failed to read passphrase") != NULL, "and names the failed read");
}

/* ── 12. init is idempotent, and says something useful with no terminal ──── */
static void test_init_idempotent(void)
{
    printf("\n── calling init twice ──\n");
    reset_dir();

    CHECK(init_with_input(true, g_dir, "key four\nkey four\n"), "first init loads the key");
    /* Already ready: the second call must not re-prompt (there is nothing on the pty, so a
     * re-prompt would read EOF and fail). */
    CHECK(edc_secret_init(false, g_dir), "a second init returns true without re-prompting");
}

static void test_no_terminal(void)
{
    printf("\n── no terminal at all ──\n");
    reset_dir();
    edc_secret_cleanup();
    unsetenv("EDGECLAW_MASTER_KEY");

    /* The CI case: stdin is not a terminal and no key came from the environment. */
    const int saved = dup(STDIN_FILENO);
    const int devnull = open("/dev/null", O_RDONLY);
    if (saved < 0 || devnull < 0)
    {
        printf("    (skipped: cannot redirect stdin)\n");
        if (saved >= 0)
            close(saved);
        if (devnull >= 0)
            close(devnull);
        return;
    }
    dup2(devnull, STDIN_FILENO);
    close(devnull);

    g_log[0] = '\0';
    const bool ok = edc_secret_init(false, g_dir);

    dup2(saved, STDIN_FILENO);
    close(saved);

    printf("    log: %s\n", g_log);
    CHECK(!ok, "init fails when stdin is not a terminal");
    CHECK(strstr(g_log, "No terminal for passphrase input") != NULL,
          "and tells the user to set EDGECLAW_MASTER_KEY for scripting");
}

/* ── 13. a config write must not silently lose the stored key ────────────── */
static void test_update_llm_refuses_without_key(void)
{
    printf("\n── /connect with no master key loaded ──\n");
    reset_dir();

    /* A config the way the product leaves it: a real encrypted key on disk. */
    if (!init_with_input(true, g_dir, "key five\nkey five\n"))
    {
        printf("    (skipped: setup failed)\n");
        return;
    }
    sds enc = edc_secret_encrypt(sdsnew("sk-ORIGINAL-KEY"));
    sds cfg_path = edc_path_join(g_dir, "config.json", NULL);
    {
        FILE *f = fopen(cfg_path, "w");
        if (!f || !enc)
        {
            printf("    (skipped: cannot write a config)\n");
            if (f)
                fclose(f);
            sdsfree(enc);
            sdsfree(cfg_path);
            return;
        }
        fprintf(f, "{\"api_url\":\"https://old.example\",\"model\":\"m1\",\"api_key\":\"%s\"}", enc);
        fclose(f);
    }

    /* Read the file back so the comparison is against bytes, not assumptions. */
    EdcError *e = NULL;
    sds before = edc_file_read(cfg_path, &e);
    if (e)
        edc_error_free(e);
    CHECK(before && strstr(before, enc) != NULL, "the config starts with the original ciphertext");

    /* Now the master key is gone -- key never initialised, failed to load, whatever. This
     * is the state that used to make the next /connect overwrite the key with "". */
    edc_secret_cleanup();

    g_log[0] = '\0';
    const bool ok = edc_config_update_llm(g_dir, "https://new.example", "m2", "sk-NEW-KEY");
    printf("    log: %s\n", g_log);
    CHECK(!ok, "the update is refused instead of reporting success");

    e = NULL;
    sds after = edc_file_read(cfg_path, &e);
    if (e)
        edc_error_free(e);
    CHECK(after && before && strcmp(after, before) == 0,
          "the config file is byte-for-byte unchanged, so the stored key survives");
    /* Checked by looking for the ORIGINAL ciphertext rather than for an empty "api_key":
     * cJSON prints with tabs, so a literal `"api_key":""` match silently succeeded even on
     * the buggy code -- which is exactly the kind of assertion that makes a regression test
     * look useful while proving nothing. */
    CHECK(after && enc && strstr(after, enc) != NULL,
          "the original ciphertext is still in the file");

    sdsfree(after);
    sdsfree(before);
    sdsfree(cfg_path);
    sdsfree(enc);
}

int main(void)
{
    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/edc_test_secret_%" PRId64,
             (tmp && *tmp) ? tmp : "/tmp", (int64_t)getpid());

    log_add_callback(capture_log, NULL, LOG_TRACE);

    printf("workspace: %s\n", g_dir);

    test_setup_writes_hash();
    test_verify_accepts_correct_passphrase();
    test_verify_rejects_wrong_passphrase();
    test_verify_without_hash();
    test_confirm_mismatch_rejected();
    test_empty_passphrase_rejected();
    test_encrypt_decrypt();
    test_env_var_mode();
    test_salt_is_per_install();
    test_unknown_prefix_refused();
    test_pre_v2_record_refused();
    test_without_key();
    test_corrupt_ciphertext();
    test_prompt_eof();
    test_init_idempotent();
    test_no_terminal();
    test_update_llm_refuses_without_key();

    edc_secret_cleanup();
    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (test_sh(cmd) != 0)
        fprintf(stderr, "(warning: could not clean %s)\n", g_dir);

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
