/* Tool-output redaction.
 *
 * The deny list is the boundary; this is the backstop for the cases it cannot cover -- a
 * command that reaches a secret under a name no deny entry matches (`cat ~/.zshrc`, `env`,
 * `git config --list`, `ssh -v`). What it lets through goes into the model's context and
 * on to the API provider, so each case below is "would this leak?" rather than "is this
 * pretty".
 *
 * Both directions matter: missing a secret leaks it, and redacting ordinary text corrupts
 * the tool result the model is trying to read.
 */

#include "edc_redact.h"
#include "sds.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
            g_passed++;                                                                                                \
        else                                                                                                           \
        {                                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                              \
            g_failed++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* Run the scrubber and hand back the result as a plain allocated string. */
static sds redact(const char *in)
{
    sds src = sdsnew(in);
    sds out = edc_redact_secrets(src);
    sdsfree(src);
    return out;
}

static bool unchanged(const char *in)
{
    sds out = redact(in);
    const bool eq = out && strcmp(out, in) == 0;
    sdsfree(out);
    return eq;
}

static bool has(const char *hay, const char *needle)
{
    return hay && strstr(hay, needle) != NULL;
}

/* ── 1. the NAME=value shape, which is what `cat ~/.zshrc` produces ───────── */
static void test_assignment(void)
{
    printf("\n-- assignment --\n");

    sds o = redact("export LLM_API_TOKEN=abc123def456ghi789jkl\n");
    CHECK(!has(o, "abc123def456ghi789jkl"), "an exported TOKEN value is dropped");
    CHECK(has(o, "LLM_API_TOKEN="), "the NAME is kept, so the result still reads");
    CHECK(has(o, "<redacted>"), "and the value is replaced by a marker");
    sdsfree(o);

    o = redact("  EDGECLAW_MASTER_KEY=supersecretvalue123\n");
    CHECK(!has(o, "supersecretvalue123"), "a MASTER_KEY value is dropped");
    sdsfree(o);

    o = redact("password: hunter2hunter2\n");
    CHECK(!has(o, "hunter2hunter2"), "a colon-separated PASSWORD is dropped");
    sdsfree(o);

    o = redact("{\"api_key\": \"abcdefgh12345678\"}\n");
    CHECK(!has(o, "abcdefgh12345678"), "a JSON api_key value is dropped");
    CHECK(has(o, "\"api_key\": \"<redacted>\""), "the quotes survive, so the JSON still parses");
    sdsfree(o);

    /* Case-insensitive: shell scripts and JSON differ. */
    o = redact("api_token=zzzzzzzzzzzzzzzz\n");
    CHECK(!has(o, "zzzzzzzzzzzzzzzz"), "a lowercase api_token is dropped");
    sdsfree(o);
}

/* ── 2. whole-identifier matching: no collateral damage ───────────────────── */
static void test_no_false_positive(void)
{
    printf("\n-- no false positives --\n");

    /* "TOKENIZER" starts with TOKEN; a substring test would redact a source line here. */
    CHECK(unchanged("TOKENIZER=x\n"), "TOKENIZER is not a credential name");
    CHECK(unchanged("token_count: 42\n"), "token_count is not a credential name");

    /* Too short to be a real secret; redacting it would corrupt ordinary config. */
    CHECK(unchanged("password: on\n"), "a short value is left alone");

    /* `==` is a comparison in every language the model is likely to read. */
    CHECK(unchanged("#define MAX_TOKEN==8\n"), "a comparison is not an assignment");

    CHECK(unchanged("The build finished in 3.2s with no warnings.\n"), "ordinary prose is untouched");

    /* A public certificate is not a secret and is pasted into prompts on purpose. */
    sds o = redact("-----BEGIN CERTIFICATE-----\nMIIBszCCAVmgAwIBAgIU\n-----END CERTIFICATE-----\n");
    CHECK(has(o, "MIIBszCCAVmgAwIBAgIU"), "a certificate body is NOT redacted");
    sdsfree(o);
}

/* ── 3. token prefixes ───────────────────────────────────────────────────── */
static void test_prefixed_tokens(void)
{
    printf("\n-- known token prefixes --\n");

    sds o = redact("key = sk-proj-FAKE-FAKE-FAKE-FAKE-FAKE\n");
    CHECK(!has(o, "FAKE-FAKE-FAKE-FAKE-FAKE"), "an sk- key is dropped");
    sdsfree(o);

    o = redact("export GH=ghp_FAKE-FAKE-FAKE-FAKE-FAKE\n");
    CHECK(!has(o, "FAKE-FAKE-FAKE-FAKE-FAKE"), "a ghp_ token is dropped");
    sdsfree(o);

    o = redact("aws_access_key_id = AKIAFAKEFAKEFAKEFAKE\n");
    CHECK(!has(o, "AKIAFAKEFAKEFAKEFAKE"), "an AWS key id is dropped");
    sdsfree(o);

    o = redact("xoxb-FAKE-FAKE-FAKE-not-a-real-token\n");
    CHECK(!has(o, "FAKE-FAKE-FAKE-not-a-real-token"), "a Slack token is dropped");
    sdsfree(o);

    /* Too short after the prefix: prose, not a token. */
    CHECK(unchanged("use sk-learn for that\n"), "a non-token sk- word is left alone");

    /* EdgeClaw's own ciphertext: redacted whole. Redacting only the payload would leave a
     * recognisable tag, which buys nothing -- and the decryption key may be in the same
     * output, which is the case that matters. */
    o = redact("api_key: EDGECLAW_ENC:v2:AQIDBAUGBwgJCgsMDQ4PEA==\n");
    CHECK(!has(o, "AQIDBAUGBwgJCgsMDQ4PEA=="), "an EDGECLAW_ENC payload is dropped");
    CHECK(has(o, "<redacted>"), "and replaced by a marker");
    sdsfree(o);
}

/* ── 4. PEM private keys ─────────────────────────────────────────────────── */
static void test_pem(void)
{
    printf("\n-- PEM private key blocks --\n");

    const char *pem = "-----BEGIN OPENSSH PRIVATE KEY-----\n"
                      "b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAAB\n"
                      "AAAAMwAAAAtzc2gtZWQyNTUxOQAAACBkYW5nbGVkYW5nbGVkYW4=\n"
                      "-----END OPENSSH PRIVATE KEY-----\n";
    sds o = redact(pem);
    CHECK(!has(o, "b3BlbnNzaC1rZXktdjE"), "the key body is dropped");
    CHECK(!has(o, "AAAAEbm9uZQ"), "and every line of it, not just the first");
    CHECK(has(o, "<redacted>"), "the block is replaced by a marker");
    sdsfree(o);

    /* Truncated block (output cap, or the paste ended): redact to the end rather than
     * assume the absence of an END line means there is no key body. */
    o = redact("-----BEGIN RSA PRIVATE KEY-----\nMIIEowIBAAKCAQEA\n");
    CHECK(!has(o, "MIIEowIBAAKCAQEA"), "a truncated key block is redacted to the end");
    sdsfree(o);

    /* Surrounding text survives, so the model can still see what the output was about. */
    o = redact("before\n-----BEGIN RSA PRIVATE KEY-----\nAAAA\n-----END RSA PRIVATE KEY-----\nafter\n");
    CHECK(has(o, "before") && has(o, "after"), "text around the block is preserved");
    sdsfree(o);
}

int main(void)
{
    test_assignment();
    test_no_false_positive();
    test_prefixed_tokens();
    test_pem();

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
