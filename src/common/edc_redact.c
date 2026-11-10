#include "edc_redact.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#define REDACTED "<redacted>"

/* Shortest run of token characters that is worth treating as a credential. Below this a
 * "match" is far more likely to be prose or an identifier. */
#define MIN_TOKEN_LEN 8

/* A value shorter than this after a NAME= is too short to be a real credential, and
 * redacting it would mangle ordinary config (e.g. "password: required"). */
#define MIN_ASSIGNED_LEN 8

/* Characters that appear inside tokens. Kept deliberately wide: missing a character
 * truncates the redaction instead of failing it, which would leak the tail. */
static bool is_token_char(unsigned char c)
{
    return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '+' || c == '/' || c == '=';
}

static bool starts_with(const char *s, size_t len, size_t i, const char *lit)
{
    const size_t n = strlen(lit);
    return i + n <= len && memcmp(s + i, lit, n) == 0;
}

static bool ci_starts_with(const char *s, size_t len, size_t i, const char *lit)
{
    const size_t n = strlen(lit);
    if (i + n > len)
        return false;
    for (size_t k = 0; k < n; k++)
        if (tolower((unsigned char)s[i + k]) != tolower((unsigned char)lit[k]))
            return false;
    return true;
}

/* ── PEM private-key blocks ─────────────────────────────────────────────────
 * Matched as a whole block: the armor header alone is not the secret, the base64 body is,
 * and redacting only the header would leave the key behind. */
static bool match_pem(const char *s, size_t len, size_t i, size_t *consume)
{
    static const char k_begin[] = "-----BEGIN ";
    if (!starts_with(s, len, i, k_begin))
        return false;

    /* The header line has to name a private key; certificates and public keys are not
     * secrets and are routinely pasted into prompts on purpose. */
    size_t hdr_end = i;
    while (hdr_end < len && s[hdr_end] != '\n')
        hdr_end++;

    bool named = false;
    for (size_t p = i; p + 11 <= hdr_end; p++)
    {
        if (ci_starts_with(s, hdr_end, p, "PRIVATE KEY"))
        {
            named = true;
            break;
        }
    }
    if (!named)
        return false;

    /* Through the END line, and the newline that closes it, so no base64 survives. */
    size_t p = hdr_end;
    while (p < len)
    {
        if (starts_with(s, len, p, "-----END "))
        {
            size_t e = p;
            while (e < len && s[e] != '\n')
                e++;
            if (e < len && s[e] == '\n')
                e++;
            *consume = e - i;
            return true;
        }
        p++;
    }

    /* No END line: the block is truncated (output cap, or the pasted text ended). Redact
     * from the header to the end rather than assume there is no key body. */
    *consume = len - i;
    return true;
}

/* ── well-known token prefixes ──────────────────────────────────────────────*/
typedef struct
{
    const char *prefix;
    bool until_space; /* EDGECLAW_ENC payload contains ':' (the format version) */
} prefix_t;

static const prefix_t k_prefixes[] = {
    {"EDGECLAW_ENC:", true}, /* EdgeClaw's own ciphertext */
    {"sk-", false},          /* OpenAI / DeepSeek / generic "sk-" secret keys */
    {"sk_live_", false},     /* Stripe */
    {"sk_test_", false},
    {"ghp_", false},         /* GitHub */
    {"gho_", false},
    {"ghu_", false},
    {"ghs_", false},
    {"ghr_", false},
    {"github_pat_", false},
    {"glpat-", false},       /* GitLab */
    {"xoxb-", false},        /* Slack */
    {"xoxp-", false},
    {"xoxa-", false},
    {"xoxr-", false},
    {"xoxs-", false},
    {"AKIA", false},         /* AWS access key id */
    {"ASIA", false},
    {"AIza", false},         /* Google API key */
    {"npm_", false},
    {"pypi-", false},
    {"hf_", false},          /* Hugging Face */
    {"dop_v1_", false},      /* DigitalOcean */
};

static bool match_prefixed(const char *s, size_t len, size_t i, size_t *consume)
{
    for (size_t k = 0; k < sizeof(k_prefixes) / sizeof(k_prefixes[0]); k++)
    {
        const char *pfx = k_prefixes[k].prefix;
        if (!starts_with(s, len, i, pfx))
            continue;

        /* A prefix buried inside a longer word is not a token ("risk-", "task-"). */
        if (!(pfx[0] == 'E' && strcmp(pfx, "EDGECLAW_ENC:") == 0))
        {
            if (i > 0 && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'))
                continue;
        }

        const size_t pfx_len = strlen(pfx);
        size_t e = i + pfx_len;
        if (k_prefixes[k].until_space)
        {
            while (e < len && !isspace((unsigned char)s[e]))
                e++;
            if (e > i + pfx_len)
            {
                *consume = e - i;
                return true;
            }
            continue;
        }

        while (e < len && is_token_char((unsigned char)s[e]))
            e++;
        if (e - (i + pfx_len) >= MIN_TOKEN_LEN)
        {
            *consume = e - i;
            return true;
        }
    }
    return false;
}

/* ── NAME=value assignments ─────────────────────────────────────────────────
 * This is the one that catches `cat ~/.zshrc`: `export LLM_API_TOKEN=abc123...` has no
 * recognisable token prefix, only a recognisable NAME. */
static const char *const k_names[] = {
    "TOKEN",   "SECRET",     "PASSWORD",  "PASSWD",     "APIKEY",      "API_KEY",
    "ACCESS_KEY", "PRIVATE_KEY", "MASTER_KEY", "CREDENTIAL", "CREDENTIALS", "AUTH",
    "BEARER",  "SESSION_KEY", "ENCRYPTION_KEY", "SIGNING_KEY",
};

/* Is the identifier at |i| (length |n|) a credential name? Matched either as the WHOLE
 * identifier ("TOKEN") or as its last `_`-separated segment ("LLM_API_TOKEN",
 * "EDGECLAW_MASTER_KEY") -- real files name things after what they hold, not with the bare
 * word. "TOKENIZER" and "token_count" match neither, which is the point. */
static bool name_matches(const char *id, size_t n)
{
    for (size_t k = 0; k < sizeof(k_names) / sizeof(k_names[0]); k++)
    {
        const char *want = k_names[k];
        const size_t wlen = strlen(want);
        if (wlen > n)
            continue;

        /* Whole identifier. */
        bool whole = (wlen == n);
        /* ... or a trailing segment, which must sit behind a separator so a name is never
         * recognised from inside a longer word. */
        bool suffix = (n > wlen && id[n - wlen - 1] == '_');
        if (!whole && !suffix)
            continue;

        const char *tail = id + (n - wlen);
        bool eq = true;
        for (size_t c = 0; c < wlen && eq; c++)
            if (toupper((unsigned char)tail[c]) != want[c])
                eq = false;
        if (eq)
            return true;
    }
    return false;
}

/* Returns the offset of the value inside s[i..] and its length. */
static bool match_assignment(const char *s, size_t len, size_t i, size_t *value_off, size_t *value_len)
{
    if (i > 0 && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_' || s[i - 1] == '.'))
        return false;
    if (!isalpha((unsigned char)s[i]) && s[i] != '_')
        return false;

    size_t id_end = i;
    while (id_end < len && (isalnum((unsigned char)s[id_end]) || s[id_end] == '_'))
        id_end++;
    if (!name_matches(s + i, id_end - i))
        return false;

    /* Optional closing quote (the JSON form `"api_key": "..."`), then the separator. */
    size_t p = id_end;
    if (p < len && (s[p] == '"' || s[p] == '\''))
        p++;
    while (p < len && (s[p] == ' ' || s[p] == '\t'))
        p++;
    if (p >= len || (s[p] != '=' && s[p] != ':'))
        return false;
    p++;
    /* `==` is a comparison, not an assignment. */
    if (p < len && s[p] == '=')
        return false;
    while (p < len && (s[p] == ' ' || s[p] == '\t'))
        p++;
    if (p >= len)
        return false;

    /* A quoted value: redact what is inside the quotes, leave them, so JSON and shell
     * syntax still read correctly. */
    const char quote = (s[p] == '"' || s[p] == '\'') ? s[p] : 0;
    if (quote)
        p++;
    const size_t vstart = p;
    if (quote)
    {
        while (p < len && s[p] != quote && s[p] != '\n')
            p++;
    }
    else
    {
        while (p < len && !isspace((unsigned char)s[p]) && s[p] != ',' && s[p] != ';' && s[p] != ')')
            p++;
    }
    if (p - vstart < MIN_ASSIGNED_LEN)
        return false;

    *value_off = vstart;
    *value_len = p - vstart;
    return true;
}

sds edc_redact_secrets(sds text)
{
    if (!text)
        return NULL;

    const char *s = text;
    const size_t len = sdslen(text);
    sds out = sdsempty();
    size_t i = 0;

    while (i < len)
    {
        size_t n = 0;
        if (match_pem(s, len, i, &n) || match_prefixed(s, len, i, &n))
        {
            out = sdscat(out, REDACTED);
            i += n;
            continue;
        }

        size_t voff = 0, vlen = 0;
        if (match_assignment(s, len, i, &voff, &vlen))
        {
            /* Copy the NAME and the separator, drop the value. */
            out = sdscatlen(out, s + i, voff - i);
            out = sdscat(out, REDACTED);
            i = voff + vlen;
            continue;
        }

        out = sdscatlen(out, s + i, 1);
        i++;
    }

    return out;
}
