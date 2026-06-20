#include "mongoose.h"

// 事件处理回调
static void fn(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_RESOLVE)
    {
        // 当域名解析完成时触发
        char *ip = (char *)ev_data;
        printf("解析成功! IP 地址是: %s\n", ip);
        c->is_closing = 1; // 解析完关闭这个虚假连接
    }
    else if (ev == MG_EV_CLOSE)
    {
        // 连接关闭逻辑
    }
}

int main(void)
{
    struct mg_mgr mgr;
    mg_log_set(MG_LL_DEBUG);
    mg_mgr_init(&mgr); // 初始化管理器
    mgr.dns4.url = "udp://10.33.25.62:53";

    // 1. 发起异步解析请求
    // Mongoose 会在后台处理 DNS 协议交互
    const char *url = "mg://baidu.com";
    struct mg_connection *c = mg_connect(&mgr, url, fn, NULL);

    if (c == NULL)
    {
        printf("无法创建连接\n");
        return 1;
    }

    printf("正在解析域名...\n");

    // 2. 事件循环
    while (true)
    {
        mg_mgr_poll(&mgr, 10); // 驱动事件
    }

    mg_mgr_free(&mgr);
    return 0;
}

