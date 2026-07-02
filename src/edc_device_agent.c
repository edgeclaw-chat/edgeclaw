#include "edc_device_agent.h"

#include "sds.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CMD_TIMEOUT_SEC 30

static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_decode_char(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static unsigned char *base64_decode(const char *input, size_t *out_len)
{
    size_t in_len = strlen(input);
    if (in_len % 4 != 0)
        return NULL;

    *out_len = in_len / 4 * 3;
    if (input[in_len - 1] == '=')
        (*out_len)--;
    if (input[in_len - 2] == '=')
        (*out_len)--;

    unsigned char *out = malloc(*out_len + 1);
    if (!out)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 4)
    {
        int a = base64_decode_char(input[i]);
        int b = base64_decode_char(input[i + 1]);
        int c = i + 2 < in_len && input[i + 2] != '=' ? base64_decode_char(input[i + 2]) : 0;
        int d = i + 3 < in_len && input[i + 3] != '=' ? base64_decode_char(input[i + 3]) : 0;

        if (a < 0 || b < 0)
        {
            free(out);
            return NULL;
        }

        out[j++] = (unsigned char)((a << 2) | (b >> 4));
        if (j < *out_len)
            out[j++] = (unsigned char)(((b & 0x0f) << 4) | (c >> 2));
        if (j < *out_len)
            out[j++] = (unsigned char)(((c & 0x03) << 6) | d);
    }
    out[*out_len] = '\0';
    return out;
}

static char *base64_encode(const unsigned char *data, size_t len)
{
    size_t out_len = ((len + 2) / 3) * 4 + 1;
    char *out = malloc(out_len);
    if (!out)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i += 3)
    {
        unsigned char a = data[i];
        unsigned char b = (i + 1 < len) ? data[i + 1] : 0;
        unsigned char c = (i + 2 < len) ? data[i + 2] : 0;

        out[j++] = base64_table[a >> 2];
        out[j++] = base64_table[((a & 0x03) << 4) | (b >> 4)];
        out[j++] = (i + 1 < len) ? base64_table[((b & 0x0f) << 2) | (c >> 6)] : '=';
        out[j++] = (i + 2 < len) ? base64_table[c & 0x3f] : '=';
    }
    out[j] = '\0';
    return out;
}

static int path_is_safe(const char *path)
{
    if (!path || path[0] != '/')
        return 0;
    if (strstr(path, ".."))
        return 0;
    return 1;
}

static int exec_cmd(const char *cmd, sds *out, sds *err, int *exit_code, int timeout_sec)
{
    *out = sdsempty();
    *err = sdsempty();
    *exit_code = -1;

#if defined(__linux__) || defined(__APPLE__)
    int out_pipe[2], err_pipe[2];
    pid_t pid;

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

    fd_set rfds;
    int max_fd = (out_pipe[0] > err_pipe[0]) ? out_pipe[0] : err_pipe[0];
    int done_out = 0, done_err = 0;
    time_t start_time = time(NULL);

    while (!done_out || !done_err)
    {
        struct timeval tv;
        struct timeval *tvp = NULL;
        if (timeout_sec > 0)
        {
            int elapsed = (int)(time(NULL) - start_time);
            int remaining = timeout_sec - elapsed;
            if (remaining <= 0)
            {
                kill(-pid, SIGKILL);
                *err = sdscat(*err, "\n[command timed out]");
                break;
            }
            tv.tv_sec = remaining;
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
#endif
}

static void handle_exec(struct edc_device_request *req, struct edc_device_transport *transport)
{
    int timeout = req->timeout > 0 ? req->timeout : CMD_TIMEOUT_SEC;
    fprintf(stderr, "agent: exec: %s (timeout %ds)\n", req->command, timeout);

    sds out_buf, err_buf;
    int exit_code;
    exec_cmd(req->command, &out_buf, &err_buf, &exit_code, timeout);

    struct edc_device_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = REQ_EXEC;
    resp.exit_code = exit_code;
    resp.stdout_buf = out_buf;
    resp.stderr_buf = err_buf;

    transport->send(transport, &resp);

    sdsfree(out_buf);
    sdsfree(err_buf);
}

static void handle_file_upload(struct edc_device_request *req, struct edc_device_transport *transport)
{
    struct edc_device_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = REQ_FILE_UPLOAD;
    resp.file_status = -1;

    if (!path_is_safe(req->file_path))
    {
        fprintf(stderr, "agent: upload: unsafe path '%s'\n", req->file_path);
        resp.stdout_buf = sdsnew("unsafe path");
        transport->send(transport, &resp);
        return;
    }

    if (!req->file_data)
    {
        fprintf(stderr, "agent: upload: no file data\n");
        resp.stdout_buf = sdsnew("no file data provided");
        transport->send(transport, &resp);
        return;
    }

    size_t decoded_len = 0;
    unsigned char *decoded = base64_decode(req->file_data, &decoded_len);
    if (!decoded)
    {
        fprintf(stderr, "agent: upload: base64 decode failed\n");
        resp.stdout_buf = sdsnew("base64 decode failed");
        transport->send(transport, &resp);
        return;
    }

    mode_t mode = req->file_mode > 0 ? (mode_t)req->file_mode : 0644;
    int flags = O_WRONLY | O_CREAT;
    if (req->file_offset == 0)
        flags |= O_TRUNC;

    int fd = open(req->file_path, flags, mode);
    if (fd < 0)
    {
        perror("agent: upload open");
        free(decoded);
        resp.stdout_buf = sdsnew("failed to open file for writing");
        transport->send(transport, &resp);
        return;
    }

    if (req->file_offset > 0)
    {
        if (lseek(fd, (off_t)req->file_offset, SEEK_SET) < 0)
        {
            perror("agent: upload lseek");
            close(fd);
            free(decoded);
            resp.stdout_buf = sdsnew("lseek failed");
            transport->send(transport, &resp);
            return;
        }
    }

    fprintf(stderr, "agent: upload: writing %zu bytes to %s offset=%zu (mode %o)\n",
            decoded_len, req->file_path, req->file_offset, mode);

    ssize_t written = write(fd, decoded, decoded_len);
    close(fd);
    free(decoded);

    if (written != (ssize_t)decoded_len)
    {
        fprintf(stderr, "agent: upload: write failed (%zd / %zu)\n", written, decoded_len);
        resp.stdout_buf = sdsnew("write failed");
        transport->send(transport, &resp);
        return;
    }

    resp.file_status = 0;
    resp.file_size = req->file_offset + (size_t)written;
    fprintf(stderr, "agent: upload: done (offset=%zu, wrote %zd, total=%zu)\n",
            req->file_offset, written, resp.file_size);
    transport->send(transport, &resp);
}

static void handle_file_download(struct edc_device_request *req, struct edc_device_transport *transport)
{
    struct edc_device_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = REQ_FILE_DOWNLOAD;
    resp.file_status = -1;

    if (!path_is_safe(req->file_path))
    {
        fprintf(stderr, "agent: download: unsafe path '%s'\n", req->file_path);
        resp.stdout_buf = sdsnew("unsafe path");
        transport->send(transport, &resp);
        return;
    }

    int fd = open(req->file_path, O_RDONLY);
    if (fd < 0)
    {
        perror("agent: download open");
        resp.stdout_buf = sdsnew("file not found or cannot be read");
        transport->send(transport, &resp);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    {
        close(fd);
        resp.stdout_buf = sdsnew("not a regular file");
        transport->send(transport, &resp);
        return;
    }

    size_t total_size = (size_t)st.st_size;
    size_t offset = req->file_offset;
    size_t length = req->file_length;

    if (offset >= total_size)
    {
        close(fd);
        resp.file_status = 0;
        resp.file_size = 0;
        resp.stdout_buf = sdsempty();
        transport->send(transport, &resp);
        sdsfree(resp.stdout_buf);
        return;
    }

    if (length == 0 || offset + length > total_size)
        length = total_size - offset;

    if (lseek(fd, (off_t)offset, SEEK_SET) < 0)
    {
        close(fd);
        resp.stdout_buf = sdsnew("lseek failed");
        transport->send(transport, &resp);
        return;
    }

    fprintf(stderr, "agent: download: reading %zu bytes from %s offset=%zu\n",
            length, req->file_path, offset);

    unsigned char *raw = malloc(length);
    if (!raw)
    {
        close(fd);
        resp.stdout_buf = sdsnew("out of memory");
        transport->send(transport, &resp);
        return;
    }

    ssize_t total = 0;
    while (total < (ssize_t)length)
    {
        ssize_t n = read(fd, raw + total, length - (size_t)total);
        if (n <= 0)
            break;
        total += n;
    }
    close(fd);

    if (total != (ssize_t)length)
    {
        free(raw);
        resp.stdout_buf = sdsnew("read failed");
        transport->send(transport, &resp);
        return;
    }

    char *encoded = base64_encode(raw, length);
    free(raw);
    if (!encoded)
    {
        resp.stdout_buf = sdsnew("base64 encode failed");
        transport->send(transport, &resp);
        return;
    }

    resp.file_status = 0;
    resp.file_size = total_size;
    resp.file_offset = offset;
    resp.file_length = (size_t)total;
    resp.stdout_buf = encoded;

    fprintf(stderr, "agent: download: done (offset=%zu, len=%zu, total=%zu)\n",
            offset, length, total_size);
    transport->send(transport, &resp);

    free(encoded);
}

void edc_device_agent_run(struct edc_device_transport *transport)
{
    while (1)
    {
        struct edc_device_request req;
        memset(&req, 0, sizeof(req));

        if (transport->recv(transport, &req) != 0)
            continue;

        switch (req.type)
        {
        case REQ_EXEC:
            handle_exec(&req, transport);
            break;
        case REQ_FILE_UPLOAD:
            handle_file_upload(&req, transport);
            break;
        case REQ_FILE_DOWNLOAD:
            handle_file_download(&req, transport);
            break;
        default:
            fprintf(stderr, "agent: unknown request type %d\n", req.type);
            break;
        }

        sdsfree(req.command);
        sdsfree(req.file_path);
        sdsfree(req.file_data);
    }
}
