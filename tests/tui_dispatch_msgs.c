/* 测试夹具的 C 侧: 造/释放一条真通道消息 (见 tui_dispatch_msgs.h 的说明)。 */
#include "tui_dispatch_msgs.h"

#include "edc_channel_actor.h"
#include "edc_config.h"

/* 镜像必须与真枚举一致 —— 改了 edc_config.h 而忘了这里, 编译就红。 */
_Static_assert(HARNESS_MSG_USER == EDC_CHANNEL_MESSAGE_USER_TYPE, "HARNESS_MSG_USER 镜像");
_Static_assert(HARNESS_MSG_TOOL == EDC_CHANNEL_MESSAGE_TOOL_TYPE, "HARNESS_MSG_TOOL 镜像");
_Static_assert(HARNESS_MSG_THOUGHT == EDC_CHANNEL_MESSAGE_THOUGHT_TYPE, "HARNESS_MSG_THOUGHT 镜像");
_Static_assert(HARNESS_MSG_RESP == EDC_CHANNEL_MESSAGE_RESP_TYPE, "HARNESS_MSG_RESP 镜像");
_Static_assert(HARNESS_MSG_SYS == EDC_CHANNEL_MESSAGE_SYS_TYPE, "HARNESS_MSG_SYS 镜像");
_Static_assert(HARNESS_MSG_TASK_SNAPSHOT == EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE, "HARNESS_MSG_TASK_SNAPSHOT 镜像");
_Static_assert(HARNESS_MSG_TASK_TREE_CHANGED == EDC_CHANNEL_MESSAGE_TASK_TREE_CHANGED_TYPE,
               "HARNESS_MSG_TASK_TREE_CHANGED 镜像");

struct edc_channel_message *harness_msg_new(int message_type, const char *content, const char *session_id,
                                            const char *header, int from_active_task)
{
    /* 与生产路径同一个构造器 (channel actor 的)。 */
    struct edc_channel_message *m =
        edc_channel_message_new(edc_channel_adapter, (edc_channel_message_type)message_type, sdsnew(content));
    if (session_id && *session_id)
        m->session_id = sdsnew(session_id);
    if (header && *header)
        m->header = sdsnew(header);
    m->from_active_task = from_active_task != 0;
    return m;
}

void harness_msg_free(struct edc_channel_message *m)
{
    edc_channel_message_free(m);
}
