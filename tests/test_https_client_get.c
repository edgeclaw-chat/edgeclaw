#include "src/edc_https_client.h"
#include "uv.h"
#include <stdio.h>

static uv_sem_t done_sem;

static void edc_https_client_fn(struct edc_https_request *req, int ev, void *data)
{
    if (ev == EDC_HTTPS_EVENT_HS)
    {
        static char body[256];
        int n = snprintf(body, sizeof(body),
                         "GET /%s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "Connection: close\r\n"
                         "\r\n",
                         edc_https_request_get_path(req), edc_https_request_get_host(req));

        edc_https_request_write(req, (unsigned char *)body, n);
    }
    else if (ev == EDC_HTTPS_EVENT_HEADER)
    {
        struct edc_https_header *header = data;
        printf("%s: %s\n", header->field, header->value);
    }
    else if (ev == EDC_HTTPS_EVENT_MSG)
    {
        uv_buf_t *buf = data;
        printf("%.*s\n", (int)buf->len, buf->base);
    }
    else if (ev == EDC_HTTPS_EVENT_ERROR)
    {
        int *code = data;
        fprintf(stderr, "edc_https_client: error event, code: %d\n", *code);
    }
    else if (ev == EDC_HTTPS_EVENT_CLOSE)
    {
        printf("edc_https_client: connection closed\n");
        uv_sem_post(&done_sem);
    }
}

int main(const int argc, const char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "usage %s <url>\n", argv[0]);
        return -1;
    }

    uv_sem_init(&done_sem, 0);
    struct edc_https_client *client = edc_https_client_new();
    edc_https_client_connect(client, argv[1], edc_https_client_fn);

    uv_sem_wait(&done_sem);
    edc_https_client_free(client);
    uv_sem_destroy(&done_sem);
    return 0;
}
