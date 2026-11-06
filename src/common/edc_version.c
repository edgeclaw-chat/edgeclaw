#include "edc_version.h"

#include <stdio.h>

const char *edc_version(void)
{
    return EDC_VERSION;
}

const char *edc_version_commit(void)
{
    return EDC_COMMIT;
}

const char *edc_version_build_time(void)
{
    return EDC_BUILD_TIME;
}

int edc_version_banner(char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return 0;
    return snprintf(buf, cap, "edgeclaw %s (%s) built %s", EDC_VERSION, EDC_COMMIT,
                    EDC_BUILD_TIME);
}

const char *edc_platform(void)
{
    /* Spelled the way the release tarballs are (edgeclaw_aarch64_linux.tar.gz), so a
     * version report and a download page use the same words. */
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
    return "windows/arm64";
#else
    return "windows/amd64";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "macos/arm64";
#else
    return "macos/amd64";
#endif
#elif defined(__ANDROID__)
#if defined(__aarch64__)
    return "android/aarch64";
#else
    return "android/arm";
#endif
#else
#if defined(__aarch64__)
    return "linux/aarch64";
#elif defined(__arm__)
    return "linux/arm";
#elif defined(__x86_64__)
    return "linux/amd64";
#else
    return "linux/unknown";
#endif
#endif
}

void edc_version_print(int schema_version, FILE *out)
{
    if (!out)
        out = stdout;
    fprintf(out, "edgeclaw %s (%s)\n", EDC_VERSION, EDC_COMMIT);
    fprintf(out, "schema   v%d\n", schema_version);
    fprintf(out, "built    %s\n", EDC_BUILD_TIME);
    fprintf(out, "platform %s\n", edc_platform());
}

/* Every line here earns its place by being something a first-time user would otherwise
 * have to read the manual to learn, or by being the answer to a question the program
 * currently answers with silence. Deliberately not a flag dump: the full list is /help. */
/* Everything below `--help` shares two closing lines: where the manual is, and where the
 * log is. Those two are true for every frontend and are exactly what a stuck user needs. */
static void usage_tail(FILE *out)
{
    fprintf(out,
            "\n"
            "Manual: ~/.edgeclaw/docs/edgeclaw.md (written out on first run).\n"
            "If it will not start, the reason is in ~/.edgeclaw/log/edgeclaw-<date>.log.\n");
}

/* argv[0] is whatever the user typed to get here -- "./edgeclaw", a full path, or the
 * basename. A usage block that echoes the path is noise; the last component is the name
 * of the program. */
static const char *program_name(const char *program)
{
    const char *name = program;
    if (name)
    {
        for (const char *p = name; *p; p++)
        {
            if (*p == '/' || *p == '\\')
                name = p + 1;
        }
    }
    return (name && name[0]) ? name : "edgeclaw";
}

void edc_usage_print(const char *program, FILE *out)
{
    if (!out)
        out = stdout;
    const char *name = program_name(program);

    fprintf(out,
            "%s %s (%s) -- a terminal agent where every message becomes a task\n"
            "\n"
            "Usage: %s [options]\n"
            "\n"
            "  --version        print version, schema, build time and platform\n"
            "  --no-sandbox     disable the filesystem sandbox for this run\n"
            "  --help, -h       this text\n"
            "\n"
            "Running it with no arguments starts the interface. The first run writes\n"
            "~/.edgeclaw/config.json, asks for a passphrase to encrypt your API key, and\n"
            "opens a setup page for provider, key and model.\n"
            "\n"
            "Inside: /help lists the commands, /connect changes provider or key.\n",
            name, EDC_VERSION, EDC_COMMIT, name);
    usage_tail(out);
}

void edc_usage_print_cli(const char *program, FILE *out)
{
    if (!out)
        out = stdout;
    const char *name = program_name(program);

    /* Same skeleton as the interactive one, different body: this binary takes the line as
     * an argument, so "run it with no arguments" and "/help" would both be untrue here. */
    fprintf(out,
            "%s %s (%s) -- one line in, one task out\n"
            "\n"
            "Usage: %s [options] '<line>'\n"
            "\n"
            "  --version        print version, schema, build time and platform\n"
            "  --json           emit every message as a JSON line, not just the answer\n"
            "  --silent         emit nothing; the exit status is the answer\n"
            "  --session <id>   use this session instead of the one derived from the cwd\n"
            "  --timeout <sec>  give up after this long\n"
            "  --no-sandbox     disable the filesystem sandbox for this run\n"
            "  --sandbox=off    same as --no-sandbox\n"
            "  --help, -h       this text\n"
            "\n"
            "The line is exactly what you would type in the interface, quoting included:\n"
            "  %s 'explain @src/main.c'\n"
            "  %s '/task new \"write the docs\"'\n"
            "  %s '\"a\" | \"b\"'\n",
            name, EDC_VERSION, EDC_COMMIT, name, name, name, name);
    usage_tail(out);
}
