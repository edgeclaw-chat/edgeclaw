#include "edc_agent.h"
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_init_env.h"
#include "edc_log.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "utlist_fix.h"
#ifndef _WIN32
#include "edc_lwip.h"
#endif
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#elif defined(__APPLE__)
#include <sys/ptrace.h>
#endif

#include "log.h"

#define EDGECLAW_VERSION "1.0.0"

volatile sig_atomic_t _run_loop = 1;

#ifdef _WIN32
#include <windows.h>

static BOOL WINAPI win32_ctrl_handler(DWORD ctrl_type)
{
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_SHUTDOWN_EVENT)
    {
        _run_loop = 0;
        return true;
    }
    return false;
}
#else
static void signal_handler(int sig)
{
    (void)sig;
    _run_loop = 0;
}
#endif

static void print_usage(const char *progname)
{
    printf("Usage: %s [OPTION]... [SCRIPT|FILE]\n", progname);
    printf("\n");
    printf("Edgeclaw - AI-powered assistant agent.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -s, --setup         run initial setup wizard and exit\n");
    printf("  -S, --sandbox       enable filesystem sandbox\n");
    printf("  -c, --command CMD   execute a single command and exit\n");
    printf("  -h, --help          display this help and exit\n");
    printf("  -V, --version       output version information and exit\n");
    printf("\n");
    printf("If a file path is given as the final argument, each line of the\n");
    printf("file is executed as a command (shell script mode).\n");
}

static void print_version(void)
{
    printf("edgeclaw %s\n", EDGECLAW_VERSION);
    printf("Copyright (C) 2026 Edgeclaw Authors.\n");
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
    SetConsoleCtrlHandler(win32_ctrl_handler, true);
#else
    struct sigaction sa = {.sa_handler = signal_handler, .sa_flags = 0};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#endif

    bool setup = false;
    bool sandbox_enabled = false;
    char *command = NULL;

    static struct option long_options[] = {{"setup", no_argument, 0, 's'},         {"sandbox", no_argument, 0, 'S'},
                                           {"command", required_argument, 0, 'c'}, {"help", no_argument, 0, 'h'},
                                           {"version", no_argument, 0, 'V'},       {0, 0, 0, 0}};

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "sSc:hV", long_options, &option_index)) != -1)
    {
        switch (opt)
        {
        case 's':
            setup = true;
            break;
        case 'S':
            sandbox_enabled = true;
            break;
        case 'c':
            command = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        case 'V':
            print_version();
            return 0;
        case '?':
            print_usage(argv[0]);
            return 1;
        default:
            break;
        }
    }

    for (int i = optind; i < argc; i++)
    {
        if (strcmp(argv[i], "setup") == 0)
        {
            setup = true;
            break;
        }
    }

#ifndef EDGECLAW_DEBUG
#ifdef __linux__
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#elif defined(__APPLE__)
    ptrace(PT_DENY_ATTACH, 0, 0, 0);
#elif defined(_WIN32)
    /* Windows: no ptrace equivalent; rely on ASLR + DEP enabled by linker */
#endif
#endif

    edc_log_init();

    uv_loop_t *default_loop = uv_default_loop();
    edc_file_utils_init(default_loop);

    if (setup)
    {
        sds edc_home = edc_config_resolve_bc_home();
        if (!edc_init_env(edc_home, true))
        {
            log_error("Failed to initialize environment.");
            sdsfree(edc_home);
            return -1;
        }
        sdsfree(edc_home);
        printf("Setup complete.\n");
        return 0;
    }

    struct edc_agent *agent = edc_agent_new(false, sandbox_enabled);
    if (!agent)
    {
        log_error("create agent failed.");
        return -1;
    }

    /* 注册命令处理器 (shell → agent 耦合解耦) */
    edc_agent_set_command_handler(edc_commands_handler);

    // agent->network->mgr.dns4.url = "udp://114.114.114.114:53";

    /* PPPoS device link (optional, configured via config.json "device_pty") */
#ifndef _WIN32
    if (agent->config->device_pty)
    {
        log_info("connecting to device via PPPoS on %s (baud %d) ...", agent->config->device_pty,
                 agent->config->device_baud);
        if (edc_lwip_init_pppos_path(agent->config->device_pty, agent->config->device_baud))
            log_info("PPPoS link UP");
        else
            log_error("PPPoS link failed");
    }
#endif

    /* ── -c / script mode ─────────────────────────────── */
    if (command)
    {
        struct edc_channel_message *msg = edc_channel_message_new(edc_channel_cli, sdsnew(command));
        edc_actor_t *actor = edc_scheduler_get_actor(agent->scheduler);
        edc_commands_handler(agent, actor, msg);
        edc_channel_message_free(msg);

        int timeout = 300;
        while (timeout-- > 0)
        {
            int has_active = 0;
            struct edc_task_group *g;
            DL_FOREACH(agent->scheduler->session->groups, g)
            {
                struct edc_task *t;
                LL_FOREACH2(g->members, t, group_next)
                {
                    if (t->state != TASK_COMPLETED && t->state != TASK_CANCELLED && t->state != TASK_ERROR &&
                        t->state != TASK_SUSPENDED)
                    {
                        has_active = 1;
                        break;
                    }
                }
                if (has_active)
                    break;
            }
            if (!has_active)
                break;
            usleep(100000);
        }
        _run_loop = 0;
        usleep(200000); /* let pending uv callbacks settle */
        edc_agent_free(agent);
        return 0;
    }

    /* Check for script file argument */
    {
        const char *script_path = NULL;
        if (optind < argc)
            script_path = argv[optind];

        if (script_path)
        {
            EdcError *err = NULL;
            sds contents = edc_file_read(script_path, &err);
            if (contents)
            {
                int nlines = 0;
                sds *lines = sdssplitlen(contents, sdslen(contents), "\n", 1, &nlines);
                for (int i = 0; i < nlines; i++)
                {
                    sdstrim(lines[i], " \t\r");
                    if (sdslen(lines[i]) == 0 || lines[i][0] == '#')
                        continue;

                    struct edc_channel_message *msg = edc_channel_message_new(edc_channel_cli, sdsdup(lines[i]));
                    edc_actor_t *actor = edc_scheduler_get_actor(agent->scheduler);
                    edc_commands_handler(agent, actor, msg);
                    edc_channel_message_free(msg);

                    /* brief pause between commands */
                    int timeout = 30;
                    while (timeout-- > 0)
                    {
                        int has_active = 0;
                        struct edc_task_group *g;
                        DL_FOREACH(agent->scheduler->session->groups, g)
                        {
                            struct edc_task *t;
                            LL_FOREACH2(g->members, t, group_next)
                            {
                                if (t->state != TASK_COMPLETED && t->state != TASK_CANCELLED &&
                                    t->state != TASK_ERROR && t->state != TASK_SUSPENDED)
                                {
                                    has_active = 1;
                                    break;
                                }
                            }
                            if (has_active)
                                break;
                        }
                        if (!has_active)
                            break;
                        usleep(100000);
                    }
                }
                sdsfreesplitres(lines, nlines);
                sdsfree(contents);
            }
            else
            {
                fprintf(stderr, "cannot read script: %s\n", script_path);
                if (err)
                    edc_error_free(err);
            }
            _run_loop = 0;
            usleep(200000);
            edc_agent_free(agent);
            return 0;
        }
    }

    while (_run_loop)
    {
        edc_log_check_rotate();
        if (usleep(1 * 1000000) == -1 && errno == EINTR)
            continue;
    }

    edc_agent_free(agent);

    return 0;
}
