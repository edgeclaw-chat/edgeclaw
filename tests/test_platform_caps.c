/* The platform capability table.
 *
 * These are the CHOICES that used to be spelled as #ifdef at each call site: is there a
 * kernel sandbox, which shell does the tool spawn, how is the child started. Pinning them
 * here is what keeps a decision from being re-made (and drifting) in four files -- the
 * shell tool, the kill path, the key-locking code and the config loader each used to know
 * their own answer.
 *
 * The assertions are deliberately about SHAPE as well as values: the shell tool builds its
 * argv by appending the command line to the head below, so the head has to be a real,
 * NULL-terminated, short vector on every platform. */

#include "edc_platform.h"

#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            passed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

int main(void)
{
    const struct edc_platform_caps *caps = edc_platform_caps();

    CHECK(caps != NULL, "there is always a table");
    if (!caps)
        return 1;

    /* The helper every call site uses must be the field, not a second opinion. */
    CHECK(edc_platform_has_sandbox() == caps->kernel_sandbox, "edc_platform_has_sandbox() mirrors the table");

    /* Usable as a spawn vector on every platform: a file, a short non-empty NULL-terminated
     * prefix (the shell tool appends the command line to it). */
    CHECK(caps->shell_file && caps->shell_file[0], "shell_file is set");
    {
        int words = 0;
        for (const char *const *a = caps->shell_argv_head; a && *a; a++)
            words++;
        CHECK(words >= 1 && words <= 3, "the argv prefix is 1..3 words");
        CHECK(caps->shell_argv_head[0] && caps->shell_argv_head[0][0], "the argv prefix starts with a word");
    }

#if defined(_WIN32)
    CHECK(!caps->kernel_sandbox, "no kernel sandbox backend on Windows (see edc_platform.h)");
    CHECK(caps->spawn_mode == EDC_SPAWN_HIDE, "Windows hides the console rather than detaching");
    CHECK(strcmp(caps->shell_file, "powershell.exe") == 0, "Windows spawns powershell.exe");
    CHECK(strcmp(caps->shell_argv_head[0], "powershell.exe") == 0, "argv[0] is the program");
    CHECK(caps->shell_argv_head[1] && strcmp(caps->shell_argv_head[1], "-NonInteractive") == 0,
          "a prompt must not wait on a console nobody watches");
    CHECK(caps->shell_argv_head[2] && strcmp(caps->shell_argv_head[2], "-Command") == 0,
          "-Command takes the rest of the line");
#elif defined(__linux__) || defined(__APPLE__)
    CHECK(caps->kernel_sandbox, "a kernel sandbox backend exists here");
    CHECK(caps->spawn_mode == EDC_SPAWN_DETACH, "POSIX detaches: no controlling terminal");
    CHECK(strcmp(caps->shell_file, "/bin/sh") == 0, "POSIX spawns /bin/sh");
    CHECK(strcmp(caps->shell_argv_head[0], "sh") == 0, "argv[0] is the program");
    CHECK(caps->shell_argv_head[1] && strcmp(caps->shell_argv_head[1], "-c") == 0, "sh takes -c");
    CHECK(caps->shell_argv_head[2] == NULL, "the POSIX prefix is exactly two words");
#else
    CHECK(!caps->kernel_sandbox, "no backend on this system yet");
    CHECK(caps->spawn_mode == EDC_SPAWN_DETACH, "assume POSIX spawn semantics");
#endif

    /* mlock has three answers and "not supported" is not a failure. */
    {
        char page[64];
        memset(page, 0, sizeof(page));
#ifdef _WIN32
        CHECK(edc_platform_lock_memory(page, sizeof(page)) == EDC_LOCK_UNSUPPORTED, "no mlock on this platform");
#else
        CHECK(edc_platform_lock_memory(page, sizeof(page)) == EDC_LOCK_OK, "mlock works here");
        edc_platform_unlock_memory(page, sizeof(page));
#endif
    }

    /* Signalling "the group" of a pid that has none must be a no-op, not a crash: the shell
     * tool calls this unconditionally on every timeout and abort. */
    edc_platform_kill_process_group(0);
    passed++;

    printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
