#include "arch/sys_arch.h"
#include "lwip/sys.h"
#include "lwip/err.h"
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

struct sys_mutex_impl {
    pthread_mutex_t mutex;
    int valid;
};

struct sys_sem_impl {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int valid;
};

struct sys_mbox_entry {
    void *msg;
    struct sys_mbox_entry *next;
};

struct sys_mbox_impl {
    struct sys_mbox_entry *head;
    struct sys_mbox_entry *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int valid;
};

static pthread_mutex_t lwip_protect_mutex = PTHREAD_MUTEX_INITIALIZER;

void sys_init(void)
{
}

u32_t sys_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (u32_t)(tv.tv_sec * 1000UL + tv.tv_usec / 1000UL);
}

u32_t sys_jiffies(void)
{
    return sys_now();
}

void sys_msleep(u32_t ms)
{
    usleep(ms * 1000);
}

sys_prot_t sys_arch_protect(void)
{
    pthread_mutex_lock(&lwip_protect_mutex);
    return 0;
}

void sys_arch_unprotect(sys_prot_t pval)
{
    (void)pval;
    pthread_mutex_unlock(&lwip_protect_mutex);
}

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    struct sys_mutex_impl *m;

    m = (struct sys_mutex_impl *)malloc(sizeof(struct sys_mutex_impl));
    if (!m) {
        return ERR_MEM;
    }
    pthread_mutex_init(&m->mutex, NULL);
    m->valid = 1;
    mutex->opaque = m;
    return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    struct sys_mutex_impl *m = (struct sys_mutex_impl *)mutex->opaque;
    pthread_mutex_lock(&m->mutex);
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    struct sys_mutex_impl *m = (struct sys_mutex_impl *)mutex->opaque;
    pthread_mutex_unlock(&m->mutex);
}

void sys_mutex_free(sys_mutex_t *mutex)
{
    struct sys_mutex_impl *m = (struct sys_mutex_impl *)mutex->opaque;
    if (m) {
        pthread_mutex_destroy(&m->mutex);
        free(m);
        mutex->opaque = NULL;
    }
}

int sys_mutex_valid(sys_mutex_t *mutex)
{
    return mutex->opaque != NULL;
}

void sys_mutex_set_invalid(sys_mutex_t *mutex)
{
    mutex->opaque = NULL;
}

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    struct sys_sem_impl *s;

    s = (struct sys_sem_impl *)malloc(sizeof(struct sys_sem_impl));
    if (!s) {
        return ERR_MEM;
    }
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->count = count;
    s->valid = 1;
    sem->opaque = s;
    return ERR_OK;
}

void sys_sem_signal(sys_sem_t *sem)
{
    struct sys_sem_impl *s = (struct sys_sem_impl *)sem->opaque;

    pthread_mutex_lock(&s->mutex);
    s->count++;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    struct sys_sem_impl *s = (struct sys_sem_impl *)sem->opaque;
    int acquired = 0;

    pthread_mutex_lock(&s->mutex);

    if (s->count > 0) {
        s->count--;
        acquired = 1;
    } else if (timeout == 0) {
        while (s->count == 0) {
            pthread_cond_wait(&s->cond, &s->mutex);
        }
        s->count--;
        acquired = 1;
    } else {
        struct timespec ts;
        struct timeval tv;
        gettimeofday(&tv, NULL);
        ts.tv_sec = tv.tv_sec + timeout / 1000;
        ts.tv_nsec = (tv.tv_usec + (timeout % 1000) * 1000) * 1000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (s->count == 0) {
            if (pthread_cond_timedwait(&s->cond, &s->mutex, &ts) == ETIMEDOUT) {
                break;
            }
        }
        if (s->count > 0) {
            s->count--;
            acquired = 1;
        }
    }

    pthread_mutex_unlock(&s->mutex);
    return acquired ? 0 : SYS_ARCH_TIMEOUT;
}

void sys_sem_free(sys_sem_t *sem)
{
    struct sys_sem_impl *s = (struct sys_sem_impl *)sem->opaque;

    if (s) {
        pthread_cond_destroy(&s->cond);
        pthread_mutex_destroy(&s->mutex);
        free(s);
        sem->opaque = NULL;
    }
}

int sys_sem_valid(sys_sem_t *sem)
{
    return sem->opaque != NULL;
}

void sys_sem_set_invalid(sys_sem_t *sem)
{
    sem->opaque = NULL;
}

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    struct sys_mbox_impl *mb;

    (void)size;
    mb = (struct sys_mbox_impl *)malloc(sizeof(struct sys_mbox_impl));
    if (!mb) {
        return ERR_MEM;
    }
    mb->head = NULL;
    mb->tail = NULL;
    pthread_mutex_init(&mb->mutex, NULL);
    pthread_cond_init(&mb->cond, NULL);
    mb->valid = 1;
    mbox->opaque = mb;
    return ERR_OK;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    struct sys_mbox_impl *mb = (struct sys_mbox_impl *)mbox->opaque;
    struct sys_mbox_entry *entry;

    entry = (struct sys_mbox_entry *)malloc(sizeof(struct sys_mbox_entry));
    if (!entry) {
        return;
    }
    entry->msg = msg;
    entry->next = NULL;

    pthread_mutex_lock(&mb->mutex);
    if (mb->tail) {
        mb->tail->next = entry;
        mb->tail = entry;
    } else {
        mb->head = entry;
        mb->tail = entry;
    }
    pthread_cond_signal(&mb->cond);
    pthread_mutex_unlock(&mb->mutex);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    sys_mbox_post(mbox, msg);
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
    return sys_mbox_trypost(mbox, msg);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    struct sys_mbox_impl *mb = (struct sys_mbox_impl *)mbox->opaque;
    struct sys_mbox_entry *entry;
    u32_t result = 0;

    pthread_mutex_lock(&mb->mutex);

    if (timeout == 0) {
        while (mb->head == NULL) {
            pthread_cond_wait(&mb->cond, &mb->mutex);
        }
    } else {
        struct timespec ts;
        struct timeval tv;
        gettimeofday(&tv, NULL);
        ts.tv_sec = tv.tv_sec + timeout / 1000;
        ts.tv_nsec = (tv.tv_usec + (timeout % 1000) * 1000) * 1000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (mb->head == NULL) {
            if (pthread_cond_timedwait(&mb->cond, &mb->mutex, &ts) == ETIMEDOUT) {
                result = SYS_ARCH_TIMEOUT;
                break;
            }
        }
    }

    if (result != SYS_ARCH_TIMEOUT) {
        entry = mb->head;
        mb->head = entry->next;
        if (!mb->head) {
            mb->tail = NULL;
        }
        pthread_mutex_unlock(&mb->mutex);

        if (msg) {
            *msg = entry->msg;
        }
        free(entry);
        result = 0;
    } else {
        pthread_mutex_unlock(&mb->mutex);
    }

    return result;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    struct sys_mbox_impl *mb = (struct sys_mbox_impl *)mbox->opaque;
    struct sys_mbox_entry *entry;

    pthread_mutex_lock(&mb->mutex);

    entry = mb->head;
    if (entry) {
        mb->head = entry->next;
        if (!mb->head) {
            mb->tail = NULL;
        }
        pthread_mutex_unlock(&mb->mutex);

        if (msg) {
            *msg = entry->msg;
        }
        free(entry);
        return 0;
    }

    pthread_mutex_unlock(&mb->mutex);
    return SYS_MBOX_EMPTY;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    struct sys_mbox_impl *mb = (struct sys_mbox_impl *)mbox->opaque;

    if (mb) {
        struct sys_mbox_entry *entry;

        pthread_mutex_lock(&mb->mutex);
        while (mb->head) {
            entry = mb->head;
            mb->head = entry->next;
            free(entry);
        }
        pthread_mutex_unlock(&mb->mutex);

        pthread_cond_destroy(&mb->cond);
        pthread_mutex_destroy(&mb->mutex);
        free(mb);
        mbox->opaque = NULL;
    }
}

int sys_mbox_valid(sys_mbox_t *mbox)
{
    return mbox->opaque != NULL;
}

void sys_mbox_set_invalid(sys_mbox_t *mbox)
{
    mbox->opaque = NULL;
}

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread, void *arg,
                            int stacksize, int prio)
{
    pthread_t *t;
    pthread_attr_t attr;

    (void)name;
    (void)prio;

    t = (pthread_t *)malloc(sizeof(pthread_t));
    if (!t) {
        return NULL;
    }

    pthread_attr_init(&attr);
    if (stacksize > 0) {
        pthread_attr_setstacksize(&attr, (size_t)stacksize);
    }
    pthread_create(t, &attr, (void *(*)(void *))thread, arg);
    pthread_attr_destroy(&attr);

    return t;
}
