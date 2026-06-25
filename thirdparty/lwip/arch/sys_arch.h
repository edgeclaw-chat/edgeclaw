#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include "arch/cc.h"

struct sys_mutex {
    void *opaque;
};
typedef struct sys_mutex sys_mutex_t;

struct sys_sem {
    void *opaque;
};
typedef struct sys_sem sys_sem_t;

struct sys_mbox {
    void *opaque;
};
typedef struct sys_mbox sys_mbox_t;

typedef void *sys_thread_t;

typedef u32_t sys_prot_t;

void sys_mark_tcpip_thread(void);
#define LWIP_MARK_TCPIP_THREAD()   sys_mark_tcpip_thread()

void sys_lock_tcpip_core(void);
#define LOCK_TCPIP_CORE()          sys_lock_tcpip_core()

void sys_unlock_tcpip_core(void);
#define UNLOCK_TCPIP_CORE()        sys_unlock_tcpip_core()

#endif
