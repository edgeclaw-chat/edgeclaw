#include "edc_agent.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_init_env.h"
#include "edc_log.h"
#include "edc_lwip.h"
#include "edc_compat.h"
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

static void signal_handler(int sig)
{
    (void)sig;
    _run_loop = 0;
}

static void print_usage(const char *progname)
{
    printf("Usage: %s [OPTION]...\n", progname);
    printf("\n");
    printf("Edgeclaw - AI-powered assistant agent.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -s, --setup         run initial setup wizard and exit\n");
    printf("  -S, --sandbox       enable filesystem sandbox\n");
    printf("  -h, --help          display this help and exit\n");
    printf("  -V, --version       output version information and exit\n");
}

static void print_version(void)
{
    printf("edgeclaw %s\n", EDGECLAW_VERSION);
    printf("Copyright (C) 2026 Edgeclaw Authors.\n");
}

int main(int argc, char *argv[])
{
    struct sigaction sa = {.sa_handler = signal_handler, .sa_flags = 0};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    bool setup = FALSE;
    bool sandbox_enabled = FALSE;

    static struct option long_options[] = {{"setup", no_argument, 0, 's'},
                                           {"sandbox", no_argument, 0, 'S'},
                                           {"help", no_argument, 0, 'h'},
                                           {"version", no_argument, 0, 'V'},
                                           {0, 0, 0, 0}};

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "sShV", long_options, &option_index)) != -1)
    {
        switch (opt)
        {
        case 's':
            setup = TRUE;
            break;
        case 'S':
            sandbox_enabled = TRUE;
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
            setup = TRUE;
            break;
        }
    }

#ifndef EDGECLAW_DEBUG
#ifdef __linux__
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#elif defined(__APPLE__)
    ptrace(PT_DENY_ATTACH, 0, 0, 0);
#endif
#endif

    edc_log_init();

    uv_loop_t *default_loop = uv_default_loop();
    edc_file_utils_init(default_loop);

    if (setup)
    {
        sds edc_home = edc_config_resolve_bc_home();
        if (!edc_init_env(edc_home, TRUE))
        {
            log_error("Failed to initialize environment.");
            sdsfree(edc_home);
            return -1;
        }
        sdsfree(edc_home);
        printf("Setup complete.\n");
        return 0;
    }

    struct edc_agent *agent = edc_agent_new(FALSE, sandbox_enabled);
    if (!agent)
    {
        log_error("create agent failed.");
        return -1;
    }

    // agent->network->mgr.dns4.url = "udp://114.114.114.114:53";

    /* PPPoS device link (optional, configured via config.json "device_pty") */
    if (agent->config->device_pty) {
        log_info("connecting to device via PPPoS on %s (baud %d) ...",
                 agent->config->device_pty, agent->config->device_baud);
        if (edc_lwip_init_pppos_path(agent->config->device_pty, agent->config->device_baud))
            log_info("PPPoS link UP");
        else
            log_error("PPPoS link failed");
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
