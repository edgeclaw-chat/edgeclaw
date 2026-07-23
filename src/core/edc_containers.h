#ifndef __BC_CONTAINERS_H__
#define __BC_CONTAINERS_H__

#include <stddef.h>

typedef struct
{
    void **data;
    size_t head, tail, capacity;
} EdcQueue;

void edc_queue_init(EdcQueue *q);
void edc_queue_destroy(EdcQueue *q);
void edc_queue_push(EdcQueue *q, void *data);
void *edc_queue_pop(EdcQueue *q);
size_t edc_queue_length(EdcQueue *q);

#endif
