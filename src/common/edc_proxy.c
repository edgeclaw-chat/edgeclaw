#include "edc_proxy.h"

#include "edc_compat.h" /* strcasecmp -> _stricmp on MSVC */
#include "edc_string_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The first of these that is set, per the convention curl established and everything follows. */
static const char *const kProxyVars[] = {"https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"};
static const char *const kNoProxyVars[] = {"no_proxy", "NO_PROXY"};

static const char *first_env(const char *const *names, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        const char *v = getenv(names[i]);
        if (v && v[0])
            return v;
    }
    return NULL;
}

/* Case-insensitive prefix test, ASCII only -- the scheme and the separator are the only things
 * this needs to recognise, and tolower() on bytes is safe for them. */
static bool starts_with_ci(const char *s, const char *prefix)
{
    for (size_t i = 0; prefix[i]; i++)
    {
        if (s[i] == '\0')
            return false;
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i]))
            return false;
    }
    return true;
}

/* Strip |host| of a leading dot and of any ":port" suffix, in place, for comparison only. */
static void host_only(char *host)
{
    while (*host == '.')
        memmove(host, host + 1, strlen(host));

    /* A no_proxy entry may carry a port ("example.com:8080"); the port is ignored, because a
     * user writing that means the host, and refusing to match it would send traffic for that
     * host through the proxy they just excluded. */
    char *colon = strrchr(host, ':');
    if (colon && strchr(colon, ']') == NULL && strcmp(colon, ":") != 0)
    {
        /* Careful with IPv6 literals ("[::1]"), which keep their colons. */
        if (strchr(host, '[') == NULL)
            *colon = '\0';
    }
}

static bool host_matches(const char *target, const char *entry)
{
    if (strcmp(entry, "*") == 0)
        return true;
    if (strcasecmp(target, entry) == 0)
        return true;

    /* "example.com" also covers "api.example.com", which is how no_proxy is universally
     * documented: the entry is a domain suffix, not a whole hostname. */
    const size_t tl = strlen(target);
    const size_t el = strlen(entry);
    if (el > 0 && tl > el && strcasecmp(target + (tl - el), entry) == 0 && target[tl - el - 1] == '.')
        return true;
    return false;
}

static bool no_proxy_excludes(const char *target_host)
{
    const char *np = first_env(kNoProxyVars, sizeof(kNoProxyVars) / sizeof(kNoProxyVars[0]));
    if (!np || !target_host)
        return false;

    /* Split on both separators: the variable is documented as comma-separated, but space is
     * accepted everywhere in practice and a user typing "a.com b.com" means two entries. */
    sds copy = sdsnew(np);
    char *save = NULL;
    for (char *tok = strtok_r(copy, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save))
    {
        char entry[256];
        snprintf(entry, sizeof(entry), "%s", tok);
        host_only(entry);
        if (host_matches(target_host, entry))
        {
            sdsfree(copy);
            return true;
        }
    }
    sdsfree(copy);
    return false;
}

/* Parse "user:pass" (already stripped of the host part) into "Basic <base64>". */
static sds basic_auth(const char *userinfo)
{
    if (!userinfo || !userinfo[0])
        return NULL;
    sds encoded = edc_base64_encode((const unsigned char *)userinfo, strlen(userinfo));
    if (!encoded)
        return NULL;
    sds header = sdscatprintf(sdsempty(), "Basic %s", encoded);
    sdsfree(encoded);
    return header;
}

bool edc_proxy_resolve(const char *target_host, struct edc_proxy *out, const char **why)
{
    if (out)
    {
        out->host = NULL;
        out->port = 0;
        out->auth = NULL;
    }
    if (why)
        *why = NULL;

    const char *value = first_env(kProxyVars, sizeof(kProxyVars) / sizeof(kProxyVars[0]));
    if (!value)
        return false; /* nothing configured: a direct connection is the right answer */

    if (target_host && no_proxy_excludes(target_host))
    {
        if (why)
            *why = "excluded by no_proxy";
        return false;
    }

    /* Scheme. Only http:// -- see the header for why the others are refused rather than
     * downgraded to a direct connection. */
    const char *rest = value;
    const char *scheme_end = strstr(value, "://");
    if (scheme_end)
    {
        const size_t scheme_len = (size_t)(scheme_end - value);
        if (scheme_len != 4 || !starts_with_ci(value, "http"))
        {
            if (why)
                *why = "only an http:// proxy is supported (socks and https:// proxies are not)";
            return false;
        }
        rest = scheme_end + 3;
    }

    /* Credentials. Split at the LAST '@' so a password may contain one. */
    char work[1024];
    snprintf(work, sizeof(work), "%s", rest);

    char *at = strrchr(work, '@');
    char *hostpart = work;
    sds auth = NULL;
    if (at)
    {
        *at = '\0';
        auth = basic_auth(work);
        hostpart = at + 1;
    }

    /* Drop any path: "http://proxy:3128/" is a common way to write it. */
    char *slash = strchr(hostpart, '/');
    if (slash)
        *slash = '\0';

    if (!hostpart[0])
    {
        sdsfree(auth);
        if (why)
            *why = "the proxy setting names no host";
        return false;
    }

    /* host[:port]. Bracketed IPv6 keeps its colons. Sized to hold |work|, which is where the
     * text came from: a smaller buffer here is a truncation the compiler can see. */
    char host[1024];
    int port = 80;
    if (hostpart[0] == '[')
    {
        const char *close = strchr(hostpart, ']');
        if (!close)
        {
            sdsfree(auth);
            if (why)
                *why = "the bracketed IPv6 proxy address is unterminated";
            return false;
        }
        snprintf(host, sizeof(host), "%.*s", (int)(close - hostpart - 1), hostpart + 1);
        if (close[1] == ':' && close[2])
            port = atoi(close + 2);
    }
    else
    {
        char *colon = strrchr(hostpart, ':');
        if (colon)
        {
            *colon = '\0';
            if (colon[1])
                port = atoi(colon + 1);
        }
        snprintf(host, sizeof(host), "%s", hostpart);
    }

    if (port <= 0 || port > 65535)
    {
        sdsfree(auth);
        if (why)
            *why = "the proxy setting has an invalid port";
        return false;
    }

    if (out)
    {
        out->host = sdsnew(host);
        out->port = port;
        out->auth = auth;
        if (!out->host)
        {
            sdsfree(auth);
            if (why)
                *why = "out of memory reading the proxy setting";
            return false;
        }
    }
    else
    {
        sdsfree(auth);
    }
    return true;
}

void edc_proxy_clear(struct edc_proxy *p)
{
    if (!p)
        return;
    sdsfree(p->host);
    sdsfree(p->auth);
    p->host = NULL;
    p->auth = NULL;
    p->port = 0;
}
