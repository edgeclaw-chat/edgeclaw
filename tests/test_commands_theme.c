/* tests/test_commands_theme.c -- /theme list|use|reload|install|off.
 *
 * Drives the real command entry point (edc_commands_handler) through a minimal
 * scheduler, the way test_commands_jobs does. Command replies go nowhere (no
 * channel actor), so the assertions are on the state the commands actually
 * change: the in-memory config fields and the installed file.
 */

#include "edc_actor.h"
#include "test_ws.h"
#include "edc_channel_actor.h"
#include "edc_colorscheme.h"
#include "edc_commands.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_turn.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile int _run_loop = 1; /* the symbol edc_commands pulls in */

static int passed = 0, failed = 0;
#define TEST(n) do { printf("  -- %s\n", n); } while (0)
#define CHECK(c, m) do { if (c) { printf("    ok: %s\n", m); passed++; } \
    else { printf("    FAIL: %s\n", m); failed++; } } while (0)

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm; /* not freed: lives as long as the test */
}

static void drive_command(struct edc_config *cfg, struct edc_session_manager *sm,
                          struct edc_scheduler *sched, const char *content)
{
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, (edc_channel_message_type)EDC_CHANNEL_MESSAGE_USER_TYPE,
                                sdsnew(content));
    edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.type = cm->type}, cm);
    edc_scheduler_schedule(sched);
}

/* A parseable theme file placed inside |home|/colors under |name|. */
static void seed_theme(const char *home, const char *name, const char *body)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/colors/%s.vim", home, name);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(body, 1, strlen(body), f); fclose(f); }
}

int main(void)
{
    const char *ws = edc_test_workspace();
    (void)ws;

    /* A private home under TEST_TMPDIR so /theme use|off touch a real config.json. */
    char home[1024];
    const char *base = getenv("TEST_TMPDIR");
    if (!base || !*base) base = ".";
    snprintf(home, sizeof(home), "%s/edc_cmd_theme_home", base);
    edc_dir_create_with_parents(home, 0700, NULL);
    char colors[1024];
    snprintf(colors, sizeof(colors), "%s/colors", home);
    edc_dir_create_with_parents(colors, 0700, NULL);

    /* A minimal but loadable config.json: the shell/theme writers use cJSON
     * read-modify-write, so the file must exist and be an object. */
    char cfgpath[1024];
    snprintf(cfgpath, sizeof(cfgpath), "%s/config.json", home);
    {
        FILE *f = fopen(cfgpath, "wb");
        if (f) { fwrite("{}", 1, 2, f); fclose(f); }
    }

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->edc_home = sdsnew(home);
    cfg->workspace = sdsnew(ws);
    cfg->shell_syntax = true;
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sess = edc_session_manager_get_active(sm);
    if (!sess) return 1;
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    seed_theme(home, "demo", "hi Normal guifg=#ffffff guibg=#101010\nhi Keyword guifg=#abcdef\n");

    TEST("bare /theme and /theme list do not change state");
    drive_command(cfg, sm, sched, "/theme");
    drive_command(cfg, sm, sched, "/theme list");
    CHECK(cfg->theme == NULL, "listing leaves no theme selected");
    CHECK(cfg->theme_rev == 0, "listing does not bump theme_rev");

    TEST("/theme use applies an installed theme, persists, and bumps rev");
    drive_command(cfg, sm, sched, "/theme use demo");
    CHECK(cfg->theme && strcmp(cfg->theme, "demo") == 0, "demo became active");
    CHECK(cfg->theme_rev == 1, "use bumped theme_rev once");

    TEST("/theme reload re-reads the same theme and bumps rev again");
    drive_command(cfg, sm, sched, "/theme reload");
    CHECK(cfg->theme && strcmp(cfg->theme, "demo") == 0, "theme unchanged on reload");
    CHECK(cfg->theme_rev == 2, "reload bumped theme_rev");

    TEST("/theme off restores the built-in palette and clears config");
    drive_command(cfg, sm, sched, "/theme off");
    CHECK(cfg->theme == NULL, "off cleared the in-memory theme");
    CHECK(cfg->theme_rev == 3, "off bumped theme_rev");
    drive_command(cfg, sm, sched, "/theme use off"); /* the alias path */
    CHECK(cfg->theme == NULL, "/theme use off is an alias for off");

    TEST("bad names / missing themes / unknown subcommands change nothing");
    int rev = cfg->theme_rev;
    drive_command(cfg, sm, sched, "/theme use ../escape");
    drive_command(cfg, sm, sched, "/theme use missing");
    drive_command(cfg, sm, sched, "/theme frobnicate");
    drive_command(cfg, sm, sched, "/theme use");
    drive_command(cfg, sm, sched, "/theme install");
    drive_command(cfg, sm, sched, "/theme reload"); /* no active theme */
    CHECK(cfg->theme == NULL && cfg->theme_rev == rev, "invalid commands are no-ops");

    TEST("/theme install copies a source file and derives its name");
    char src[1024];
    snprintf(src, sizeof(src), "%s/source.vim", home);
    FILE *f = fopen(src, "wb");
    if (f) { const char *b = "hi Normal guifg=#fff guibg=#000\n"; fwrite(b, 1, strlen(b), f); fclose(f); }
    char quoted[1200];
    snprintf(quoted, sizeof(quoted), "/theme install %s", src);
    drive_command(cfg, sm, sched, quoted);
    char installed[1024];
    snprintf(installed, sizeof(installed), "%s/colors/source.vim", home);
    CHECK(edc_file_exists(installed), "source.vim was installed");
    drive_command(cfg, sm, sched, "/theme use source");
    CHECK(cfg->theme && strcmp(cfg->theme, "source") == 0, "installed theme is usable");

    printf("test_commands_theme: %d passed, %d failed\n", passed, failed);
    edc_test_store_down(NULL);
    return failed ? 1 : 0;
}
