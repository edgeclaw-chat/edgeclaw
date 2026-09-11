#ifndef __BC_ACTOR_H__
#define __BC_ACTOR_H__

#include "edc_containers.h"
#include "sds.h"
#include <stdbool.h>
#include <stddef.h>
#include <uv.h>

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

typedef struct edc_actor edc_actor_t;
typedef struct edc_actor_msg edc_actor_msg_t;

typedef enum
{
    BC_ACTOR_MSG_START = 0,
    BC_ACTOR_MSG_USER_INPUT,
    BC_ACTOR_MSG_LLM_REQUEST,
    BC_ACTOR_MSG_LLM_RESPONSE,
    BC_ACTOR_MSG_TOOL_REQUEST,
    BC_ACTOR_MSG_TOOL_RESPONSE,
    BC_ACTOR_MSG_CHANNEL_SEND,
    BC_ACTOR_MSG_CHANNEL_RECV,
    BC_ACTOR_MSG_MEMORY_COMPRESS,
    BC_ACTOR_MSG_MEMORY_DONE,
    BC_ACTOR_MSG_TASK_SNAPSHOT, /* TUI 请求当前 session 的 task 树快照 */
    BC_ACTOR_MSG_TOOL_EMIT,     /* toolbox → scheduler: 工具结果需经 active-task 闸门后转发 */
    BC_ACTOR_MSG_GROUP_FOLD,    /* scheduler → memory_actor: 折叠 group 内已终结 task 的 turns */
    BC_ACTOR_MSG_GROUP_FOLD_DONE, /* memory_actor → scheduler: 折叠完成, 请求持久化 */
    BC_ACTOR_MSG_TOOL_STARTED,    /* toolbox → scheduler: 某个工具**真正开始执行**
                                   * (data = strdup(call_id)); 供 resume 账本精确
                                   * 区分"未执行"(PENDING) 与"执行中"(EXECUTING) */
} edc_actor_msg_type_t;

extern const char *edc_actor_msg_type_s(edc_actor_msg_type_t type);
typedef void (*edc_actor_data_free_fn)(void *);

struct edc_actor_msg
{
    edc_actor_msg_type_t type;
    int sub_type;
    edc_actor_t *from;
    void *request_id;
    void *data;
    edc_actor_data_free_fn data_free_fn;
};

typedef void (*edc_actor_handler_t)(edc_actor_t *actor, edc_actor_msg_t *msg);

struct edc_actor
{
    EdcQueue mailbox;
    uv_mutex_t mailbox_lock;
    uv_loop_t *loop;
    uv_async_t wakeup;
    edc_actor_handler_t handler;
    void *state;
    sds name;
};

typedef struct
{
    uv_thread_t thread;
    uv_loop_t loop;
    edc_actor_t **actors;
    int actor_count;
    int actor_capacity;
    volatile int running;
} edc_actor_runner_t;

edc_actor_runner_t *edc_actor_runner_new();
void edc_actor_runner_free(edc_actor_runner_t *runner);
void edc_actor_runner_start(edc_actor_runner_t *runner);
void edc_actor_runner_stop(edc_actor_runner_t *runner);
void edc_actor_runner_add(edc_actor_runner_t *runner, edc_actor_t *actor);

edc_actor_t *edc_actor_new(const char *name, edc_actor_handler_t handler, void *state);
void edc_actor_free(edc_actor_t *actor);
void edc_actor_send(edc_actor_t *actor, edc_actor_msg_t *msg);
edc_actor_msg_t *edc_actor_msg_new(edc_actor_msg_type_t type, edc_actor_t *from, void *request_id, void *data,
                                   edc_actor_data_free_fn fn);
edc_actor_msg_t *edc_actor_msg_new_sub_type(edc_actor_msg_type_t type, int sub_type, edc_actor_t *from,
                                            void *request_id, void *data, edc_actor_data_free_fn fn);
void edc_actor_msg_free(edc_actor_msg_t *msg);

#endif
