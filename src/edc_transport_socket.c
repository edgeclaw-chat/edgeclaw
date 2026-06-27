#include "edc_transport_socket.h"

#include <cJSON.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct edc_transport_socket
{
    struct edc_device_transport base;

    int listen_fd;
    int conn_fd;
    char bind_ip[64];
    uint16_t port;
};

static int send_all(int fd, const void *buf, size_t len)
{
    size_t total = 0;
    while (total < len)
    {
        ssize_t n = send(fd, (const char *)buf + total, len - total, 0);
        if (n <= 0)
            return -1;
        total += (size_t)n;
    }
    return 0;
}

static int recv_http_request(int fd, char *buf, size_t *len, size_t cap)
{
    int content_length = -1;
    *len = 0;

    while (*len < cap)
    {
        ssize_t n = recv(fd, buf + *len, cap - *len, 0);
        if (n <= 0)
            break;
        *len += (size_t)n;
        buf[*len] = '\0';

        char *body = strstr(buf, "\r\n\r\n");
        if (!body)
            continue;
        if (content_length < 0)
        {
            char *cl = strcasestr(buf, "Content-Length:");
            if (!cl)
            {
                cl = strcasestr(buf, "content-length:");
            }
            content_length = cl ? atoi(cl + 15) : 0;
        }
        if (*len >= (size_t)(body + 4 - buf) + (size_t)content_length)
            break;
    }
    return (*len > 0) ? 0 : -1;
}

static int transport_socket_recv(struct edc_device_transport *base, struct edc_device_request *req)
{
    struct edc_transport_socket *t = (struct edc_transport_socket *)base;

    struct sockaddr_in cli_addr;
    socklen_t cli_len = sizeof(cli_addr);
    int conn_fd = accept(t->listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
    if (conn_fd < 0)
    {
        if (errno == EINTR)
            return -1;
        perror("agent: accept");
        usleep(100000);
        return -1;
    }

    {
        int one = 1;
        setsockopt(conn_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    char req_buf[131072];
    size_t req_len = 0;
    if (recv_http_request(conn_fd, req_buf, &req_len, sizeof(req_buf)) != 0)
    {
        close(conn_fd);
        return -1;
    }

    req_buf[req_len] = '\0';
    char *body = strstr(req_buf, "\r\n\r\n");
    if (!body)
    {
        const char *bad = "HTTP/1.1 400\r\nContent-Length: 0\r\n\r\n";
        send_all(conn_fd, bad, strlen(bad));
        close(conn_fd);
        return -1;
    }
    body += 4;

    cJSON *req_json = cJSON_Parse(body);
    if (!req_json)
    {
        const char *bad = "HTTP/1.1 400\r\nContent-Length: 0\r\n\r\n";
        send_all(conn_fd, bad, strlen(bad));
        close(conn_fd);
        return -1;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(req_json, "command");
    if (!cmd_item || !cJSON_IsString(cmd_item))
    {
        cJSON_Delete(req_json);
        const char *bad = "HTTP/1.1 400\r\nContent-Length: 0\r\n\r\n";
        send_all(conn_fd, bad, strlen(bad));
        close(conn_fd);
        return -1;
    }

    req->command = strdup(cmd_item->valuestring);
    req->timeout = 0;

    cJSON *timeout_item = cJSON_GetObjectItem(req_json, "timeout");
    if (timeout_item && cJSON_IsNumber(timeout_item) && timeout_item->valueint > 0)
        req->timeout = timeout_item->valueint;

    cJSON_Delete(req_json);

    t->conn_fd = conn_fd;
    return 0;
}

static int transport_socket_send(struct edc_device_transport *base, const struct edc_device_response *resp)
{
    struct edc_transport_socket *t = (struct edc_transport_socket *)base;

    if (t->conn_fd < 0)
        return -1;

    cJSON *resp_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp_json, "exit_code", resp->exit_code);
    cJSON_AddStringToObject(resp_json, "stdout", resp->stdout_buf);
    cJSON_AddStringToObject(resp_json, "stderr", resp->stderr_buf);

    char *resp_str = cJSON_PrintUnformatted(resp_json);
    size_t body_len = strlen(resp_str);

    char hdr[256];
    int hdr_len = snprintf(hdr, sizeof(hdr),
                           "HTTP/1.1 200 OK\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %zu\r\n"
                           "\r\n",
                           body_len);

    send_all(t->conn_fd, hdr, (size_t)hdr_len);
    send_all(t->conn_fd, resp_str, body_len);

    cJSON_Delete(resp_json);
    free(resp_str);

    close(t->conn_fd);
    t->conn_fd = -1;

    fprintf(stderr, "agent: done (exit_code=%d)\n", resp->exit_code);
    return 0;
}

static void transport_socket_cleanup(struct edc_device_transport *base)
{
    struct edc_transport_socket *t = (struct edc_transport_socket *)base;

    if (t->conn_fd >= 0)
    {
        close(t->conn_fd);
        t->conn_fd = -1;
    }
    if (t->listen_fd >= 0)
    {
        close(t->listen_fd);
        t->listen_fd = -1;
    }
    free(t);
}

struct edc_device_transport *edc_transport_socket_create(const char *bind_ip, uint16_t port)
{
    struct edc_transport_socket *t = (struct edc_transport_socket *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;

    t->base.recv = transport_socket_recv;
    t->base.send = transport_socket_send;
    t->base.cleanup = transport_socket_cleanup;
    t->conn_fd = -1;
    t->listen_fd = -1;

    strncpy(t->bind_ip, bind_ip, sizeof(t->bind_ip) - 1);
    t->port = port;

    t->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (t->listen_fd < 0)
    {
        perror("agent: socket");
        free(t);
        return NULL;
    }

    int reuse = 1;
    setsockopt(t->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (strcmp(bind_ip, "0.0.0.0") == 0)
        addr.sin_addr.s_addr = INADDR_ANY;
    else
        inet_pton(AF_INET, bind_ip, &addr.sin_addr);

    if (bind(t->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("agent: bind");
        close(t->listen_fd);
        free(t);
        return NULL;
    }

    if (listen(t->listen_fd, 1) < 0)
    {
        perror("agent: listen");
        close(t->listen_fd);
        free(t);
        return NULL;
    }

    fprintf(stderr, "agent: listening on %s:%u\n", bind_ip, port);

    return &t->base;
}
