#ifndef __EDC_HTTPS_CLIENT_H__
#define __EDC_HTTPS_CLIENT_H__

#include <stddef.h>

#define EDC_HTTPS_EVENT_HS 0
#define EDC_HTTPS_EVENT_HEADER 1
#define EDC_HTTPS_EVENT_MSG 2
#define EDC_HTTPS_EVENT_ERROR 3
#define EDC_HTTPS_EVENT_CLOSE 4

struct edc_https_request;
struct edc_https_client;

struct edc_https_header
{
    const char *field;
    const char *value;
    struct edc_https_header *next;
};

typedef void (*edc_https_event_fn)(struct edc_https_request *req, int ev, void *data);

extern struct edc_https_client *edc_https_client_new(const char *cacert_path);
extern void edc_https_client_free(struct edc_https_client *client);

/**
 * Queue an HTTPS request for @p url.
 *
 * The URL is validated synchronously; @p priv is only taken ownership of when
 * this function returns 0. On failure (-1) @p priv is left untouched and the
 * caller must release it.
 */
extern int edc_https_client_connect(struct edc_https_client *client, const char *url, edc_https_event_fn fn,
                                    void *priv);

extern const char *edc_https_request_get_host(struct edc_https_request *req);
extern const char *edc_https_request_get_path(struct edc_https_request *req);
extern void *edc_https_request_get_priv(struct edc_https_request *req);
extern int edc_https_request_write(struct edc_https_request *req, const unsigned char *buf, size_t len);
#endif
