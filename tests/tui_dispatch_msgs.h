#ifndef __TUI_DISPATCH_MSGS_H__
#define __TUI_DISPATCH_MSGS_H__

/* 造一条真 `struct edc_channel_message` 给 TUI 喂进来 —— 归 C 侧 (见同名 .c)。
 *
 * 为什么不直接在测试的 C++ 里造: `sds.h` 在 C++ 下编不过 (它的 `SDS_HDR_VAR`
 * 宏做 `void* → sdshdr8*` 隐式转换), 而消息的字段全是 sds —— 这正是从前 TUI
 * 需要一个 C bridge 的**唯一**理由。所以这里照老办法把边界划在最窄处:
 * 只有"造消息/释放消息"两个函数, 参数与返回值都是纯 C 类型 + 不透明指针。
 *
 * 消息类型的取值在这里**镜像**一份 (`edc_config.h` 引了 sds, C++ 侧同样不能引),
 * 由 .c 里的 `_Static_assert` 钉住两边一致 (与 `edc_api.h` 对 `EDC_RENDER_*`
 * 的做法同一个套路)。 */
enum
{
    HARNESS_MSG_USER = 0,
    HARNESS_MSG_TOOL = 1,
    HARNESS_MSG_THOUGHT = 2,
    HARNESS_MSG_RESP = 3,
    HARNESS_MSG_SYS = 4,
    HARNESS_MSG_TASK_SNAPSHOT = 5,
    HARNESS_MSG_TASK_TREE_CHANGED = 6,
    HARNESS_MSG_TASK_HISTORY = 7,
    /* 认不出来的类型: 前端必须什么都不做 (兜底分支)。 */
    HARNESS_MSG_GARBAGE = 99
};

struct edc_channel_message;

#ifdef __cplusplus
extern "C"
{
#endif

/* |header| / |session_id| 为 NULL 或空 = 不设该字段。 */
extern struct edc_channel_message *harness_msg_new(int message_type, const char *content, const char *session_id,
                                                   const char *header, int from_active_task);
extern void harness_msg_free(struct edc_channel_message *m);

#ifdef __cplusplus
}
#endif

#endif
