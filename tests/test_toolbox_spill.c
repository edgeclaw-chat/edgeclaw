/* tests/test_toolbox_spill.c -- the spill path's directory shape, and the varargs contract.
 *
 * Why a file of its own: a shell result over the cap is moved to disk, and the path that
 * names it is built by edc_path_join(), whose segments are VARARGS -- nothing checks them.
 * The bug this pins: the session segment was passed as `session_id && *session_id`, a 0/1
 * int where a `const char *` was expected. With a real session that handed the callee
 * (const char *)1 and the first `*seg` segfaulted; with no session it handed NULL, which
 * ended the walk early and dropped "tool-output" entirely, spilling into the workspace
 * root. Both are silent to the compiler by construction, so only a test can hold the line.
 *
 * What it pins:
 *   1. with a session, the file lands in <ws>/<session>/tool-output/;
 *   2. with no session (NULL) the file still lands in <ws>/tool-output/ -- "tool-output" is
 *      never lost;
 *   3. an empty session id behaves like NULL;
 *   4. the bytes on disk are the whole result, and the returned path names an existing file;
 *   5. a NULL tool name still produces a usable name. */

#include "edc_toolbox.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "sds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int test_failed = 0;
static int test_passed = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "    FAIL: " __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        test_failed++; \
    } else { \
        test_passed++; \
    } \
} while (0)

/* Substring test that is insensitive to the path SEPARATOR.
 *
 * The assertions below state the shape the spill path must have (".../15/tool-output/..."),
 * which is a statement about path COMPONENTS, not about which character separates them. The
 * product builds the path with edc_path_norm()/edc_path_join(), so on Windows the components
 * are separated by '\\' and a literal "/15/tool-output/" comparison fails against a perfectly
 * correct path: the log reads "\tmp\edgeclaw-test-xxxx\15\tool-output/exec_shell-1.txt". The
 * same applies to comparing against cfg->workspace, which is the caller's spelling.
 * Normalise both sides to '/' before searching, so this tests the structure and not the
 * platform's separator. */
static bool contains(const char *hay, const char *needle)
{
    if (!hay || !needle)
        return false;

    char h[2048];
    char n[1024];
    size_t hl = strlen(hay), nl = strlen(needle);
    if (hl >= sizeof(h) || nl >= sizeof(n))
        return strstr(hay, needle) != NULL;

    for (size_t i = 0; i < hl; i++)
        h[i] = (hay[i] == '\\') ? '/' : hay[i];
    h[hl] = '\0';
    for (size_t i = 0; i < nl; i++)
        n[i] = (needle[i] == '\\') ? '/' : needle[i];
    n[nl] = '\0';
    return strstr(h, n) != NULL;
}

/* Read a whole file; NULL when it cannot be opened. */
static sds slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    sds out = sdsempty();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out = sdscatlen(out, buf, n);
    fclose(f);
    return out;
}

int main(void)
{
    struct edc_config *cfg = edc_test_config();
    /* A stack toolbox: edc_toolbox_spill_output() reads conf->workspace and nothing else. */
    struct edc_toolbox tb;
    memset(&tb, 0, sizeof(tb));
    tb.conf = cfg;

    /* Comfortably past any preview cap, and content the filesystem must give back verbatim. */
    sds content = sdsempty();
    for (int i = 0; i < 4000; i++)
        content = sdscatprintf(content, "line %d: %s\n", i, "0123456789abcdefghij");
    const size_t len = sdslen(content);

    /* ── 1. a real session ─────────────────────────────────────────────────── */
    sds p1 = edc_toolbox_spill_output(&tb, "exec_shell", "15", content, len);
    CHECK(p1 != NULL, "spill with session '15' returned NULL");
    CHECK(contains(p1, "/15/tool-output/exec_shell-"), "path lacks /15/tool-output/: %s", p1 ? p1 : "(null)");
    CHECK(contains(p1, cfg->workspace), "path is not under the workspace: %s", p1 ? p1 : "(null)");
    {
        struct stat st;
        CHECK(p1 && stat(p1, &st) == 0 && S_ISREG(st.st_mode), "spilled file does not exist");
        sds back = p1 ? slurp(p1) : NULL;
        CHECK(back != NULL, "spilled file cannot be read back");
        CHECK(back && sdslen(back) == len, "spilled %zu bytes, wrote %zu",
              len, back ? sdslen(back) : 0);
        CHECK(back && memcmp(back, content, len) == 0, "spilled bytes differ from the result");
        if (back)
            sdsfree(back);
    }
    if (p1)
        sdsfree(p1);

    /* ── 2. no session: "tool-output" must survive ─────────────────────────── */
    sds p2 = edc_toolbox_spill_output(&tb, "exec_shell", NULL, content, len);
    CHECK(p2 != NULL, "spill with a NULL session returned NULL");
    CHECK(contains(p2, "/tool-output/exec_shell-"), "NULL session lost the tool-output dir: %s",
          p2 ? p2 : "(null)");
    CHECK(!contains(p2, "/15/"), "NULL session still used a session dir: %s", p2 ? p2 : "(null)");
    if (p2)
        sdsfree(p2);

    /* ── 3. empty session id behaves like NULL ─────────────────────────────── */
    sds p3 = edc_toolbox_spill_output(&tb, "exec_shell", "", content, len);
    CHECK(p3 != NULL, "spill with an empty session returned NULL");
    CHECK(contains(p3, "/tool-output/exec_shell-"), "empty session lost the tool-output dir: %s",
          p3 ? p3 : "(null)");
    if (p3)
        sdsfree(p3);

    /* ── 4. a nameless tool still gets a name ──────────────────────────────── */
    sds p4 = edc_toolbox_spill_output(&tb, NULL, "15", content, len);
    CHECK(contains(p4, "/tool-output/tool-"), "NULL tool name produced no usable name: %s",
          p4 ? p4 : "(null)");
    if (p4)
        sdsfree(p4);

    sdsfree(content);
    edc_test_store_down(cfg);

    fprintf(stderr, "\n%s: %d passed, %d failed\n", test_failed ? "FAILED" : "PASSED", test_passed,
            test_failed);
    return test_failed ? 1 : 0;
}
