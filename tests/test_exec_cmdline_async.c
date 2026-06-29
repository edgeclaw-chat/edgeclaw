#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

// 当子进程有数据写入 stdout 时触发的回调函数
static bool on_stdout_readable(GIOChannel *source, GIOCondition condition, void* data)
{
    char *line = NULL;
    size_t length = 0;
    GIOStatus status;

    // 读取一行数据
    status = g_io_channel_read_line(source, &line, &length, NULL, NULL);

    if (status == G_IO_STATUS_NORMAL && line != NULL)
    {
        g_print("子进程输出: %s", line);
        free(line);
        return TRUE; // 继续监视
    }
    else if (status == G_IO_STATUS_EOF)
    {
        g_print("子进程 stdout 已关闭 (EOF)。\n");
        return FALSE; // 停止监视
    }

    return FALSE;
}

int main(int argc, char *argv[])
{
    GError *error = NULL;
    char *command_line[] = {"ls", "-l", "/", NULL}; // 要执行的命令
    int stdout_fd;
    GPid child_pid;

    // 1. 异步启动子进程并打开 stdout 管道
    bool success = g_spawn_async_with_pipes(NULL,                // 工作目录 (NULL 为当前目录)
                                                command_line,        // 命令及参数
                                                NULL,                // 环境变量
                                                G_SPAWN_SEARCH_PATH, // 标志 (在 PATH 中寻找可执行文件)
                                                NULL, NULL,          // 子进程启动前的设置函数
                                                &child_pid,          // 返回子进程 PID
                                                NULL,                // 返回 stdin 的文件描述符 (不需要则传 NULL)
                                                &stdout_fd,          // 返回 stdout 的文件描述符
                                                NULL,                // 返回 stderr 的文件描述符
                                                &error               // 错误捕获
    );

    if (!success)
    {
        g_printerr("启动失败: %s\n", error->message);
        g_error_free(error);
        return EXIT_FAILURE;
    }

    g_print("子进程已启动，PID: %d\n", child_pid);

    // 2. 将原始文件描述符 (int) 转换为 GIOChannel
    GIOChannel *stdout_channel = g_io_channel_unix_new(stdout_fd);

    // 3. 将通道添加到主循环监视中
    // G_IO_IN 表示有数据可读，G_IO_HUP 表示连接挂断
    g_io_add_watch(stdout_channel, G_IO_IN | G_IO_HUP, (GIOFunc)on_stdout_readable, NULL);

    // 释放对 channel 的初始引用（watch 会维持它，直到回调返回 FALSE）
    g_io_channel_unref(stdout_channel);

    // 4. 运行 GLib 主循环
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    return EXIT_SUCCESS;
}

