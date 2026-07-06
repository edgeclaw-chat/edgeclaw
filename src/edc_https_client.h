#ifndef __EDC_HTTPS_CLIENT_H__
#define __EDC_HTTPS_CLIENT_H__

#define EDC_HTTPS_EVENT_HS 0
#define EDC_HTTPS_EVENT_HEADER 1
#define EDC_HTTPS_EVENT_MSG 2
#define EDC_HTTPS_EVENT_ERROR 3
#define EDC_HTTPS_EVENT_CLOSE 4

struct edc_https_request;
struct edc_https_client;
typedef void (*edc_https_event_fn)(struct edc_https_request *req, int ev, void *data);

extern struct edc_https_client *edc_https_client_new(void);
extern void edc_https_client_free(struct edc_https_client *client);
extern void edc_https_client_connect(struct edc_https_client *client, const char *url, edc_https_event_fn fn,
                                     void *priv);

extern const char *edc_https_request_get_host(struct edc_https_request *req);
extern const char *edc_https_request_get_path(struct edc_https_request *req);
extern void *edc_https_request_get_priv(struct edc_https_request *req);
#endif
