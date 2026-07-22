#include "edc_containers.h"
#include <stdlib.h>
#include <string.h>

void edc_queue_init(EdcQueue *q)
{
    q->data = NULL;
    q->head = q->tail = q->capacity = 0;
}

void edc_queue_destroy(EdcQueue *q)
{
    free(q->data);
    q->data = NULL;
    q->head = q->tail = q->capacity = 0;
}

void edc_queue_push(EdcQueue *q, void *data)
{
    if (q->head > 0 && q->head == q->tail)
    {
        q->head = q->tail = 0;
    }
    if (q->tail >= q->capacity)
    {
        if (q->head > 0)
        {
            size_t len = q->tail - q->head;
            memmove(q->data, q->data + q->head, len * sizeof(void *));
            q->head = 0;
            q->tail = len;
        }
        if (q->tail >= q->capacity)
        {
            size_t new_cap = q->capacity ? q->capacity * 2 : 8;
            void **new_data = realloc(q->data, new_cap * sizeof(void *));
            if (!new_data)
                return;
            q->data = new_data;
            q->capacity = new_cap;
        }
    }
    q->data[q->tail++] = data;
}

void *edc_queue_pop(EdcQueue *q)
{
    if (q->head >= q->tail)
        return NULL;
    return q->data[q->head++];
}

size_t edc_queue_length(EdcQueue *q)
{
    return q->tail - q->head;
}
