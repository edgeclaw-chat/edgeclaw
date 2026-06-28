#include "edc_device_agent.h"

#include "sds.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CMD_TIMEOUT_SEC 30

static int exec_cmd(const char *cmd, sds *out, sds *err, int *exit_code, int timeout_sec)
{
    int out_pipe[2], err_pipe[2];
    pid_t pid;

    *out = sdsempty();
    *err = sdsempty();
    *exit_code = -1;

    if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0)
        return -1;

    pid = fork();
    if (pid < 0)
    {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return -1;
    }

    if (pid == 0)
    {
        close(STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        setpgid(0, 0);
#ifdef __ANDROID__
        execl("/system/bin/sh", "sh", "-c", cmd, (char *)NULL);
#else
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
#endif
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);
    setpgid(pid, 0);

    fd_set rfds;
    int max_fd = (out_pipe[0] > err_pipe[0]) ? out_pipe[0] : err_pipe[0];
    int done_out = 0, done_err = 0;

    while (!done_out || !done_err)
    {
        struct timeval tv;
        struct timeval *tvp = NULL;
        if (timeout_sec > 0)
        {
            tv.tv_sec = timeout_sec;
            tv.tv_usec = 0;
            tvp = &tv;
        }

        FD_ZERO(&rfds);
        if (!done_out)
            FD_SET(out_pipe[0], &rfds);
        if (!done_err)
            FD_SET(err_pipe[0], &rfds);

        int ret = select(max_fd + 1, &rfds, NULL, NULL, tvp);
        if (ret < 0)
            break;
        if (ret == 0)
        {
            kill(-pid, SIGKILL);
            *err = sdscat(*err, "\n[command timed out]");
            break;
        }

        char buf[1024];
        ssize_t n;
        if (!done_out && FD_ISSET(out_pipe[0], &rfds))
        {
            n = read(out_pipe[0], buf, sizeof(buf));
            if (n > 0)
                *out = sdscatlen(*out, buf, (size_t)n);
            else
                done_out = 1;
        }
        if (!done_err && FD_ISSET(err_pipe[0], &rfds))
        {
            n = read(err_pipe[0], buf, sizeof(buf));
            if (n > 0)
                *err = sdscatlen(*err, buf, (size_t)n);
            else
                done_err = 1;
        }
    }
    close(out_pipe[0]);
    close(err_pipe[0]);

    int status;
    waitpid(pid, &status, 0);
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return 0;
}

void edc_device_agent_run(struct edc_device_transport *transport)
{
    while (1)
    {
        struct edc_device_request req = {0};
        if (transport->recv(transport, &req) != 0)
            continue;

        int timeout = req.timeout > 0 ? req.timeout : CMD_TIMEOUT_SEC;
        fprintf(stderr, "agent: exec: %s (timeout %ds)\n", req.command, timeout);

        sds out_buf, err_buf;
        int exit_code;
        exec_cmd(req.command, &out_buf, &err_buf, &exit_code, timeout);

        struct edc_device_response resp;
        resp.exit_code = exit_code;
        resp.stdout_buf = out_buf;
        resp.stderr_buf = err_buf;

        transport->send(transport, &resp);

        free(req.command);
        sdsfree(out_buf);
        sdsfree(err_buf);
    }
}
