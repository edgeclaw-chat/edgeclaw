#ifndef EDC_MSG_PART_H
#define EDC_MSG_PART_H

/* 一条"要追加给模型看"的消息，跨层传值用。
 *
 * 生产者是 shell 层的 IR（pass 往载荷里 append），消费者是内核的 turn 构建
 * （task_create 逐条建消息）。两边谁都不能 include 对方的头（kernel 已经依赖
 * shell_parse，反过来就成环），所以这个纯值类型落在 common。
 *
 * 加一个新的 pass（变量展开、图片、web 引用……）只往列表里 append，不需要改
 * opts / spec / task_create 的签名 —— 内核那边是一个循环，不是 case by case。 */
#include "sds.h"
#include <stdbool.h>

enum edc_msg_role
{
    EDC_MSG_USER = 0, /* 和用户那条一起给模型看（排在它后面） */
    EDC_MSG_SYSTEM = 1,
};

struct edc_msg_part
{
    enum edc_msg_role role;
    /* 不是用户写的：请求期的 {{…}} 展开遍历用户消息，必须跳过它的内容
     * （文件正文里的 {{SELF}} 是字面量，不是变量）。 */
    bool synthetic;
    sds content;
};

#endif
