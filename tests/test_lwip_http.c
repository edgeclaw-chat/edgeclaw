#include "src/edc_lwip.h"
#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include <glib.h>
#include <string.h>

static struct netconn *echo_server_conn;
static volatile gboolean server_running;

static void echo_server_thread(void *arg)
{
    err_t err;
    struct netconn *client;
    struct netbuf *inbuf;
    char *response = "Hello from lwIP echo server!\n";
    size_t written;

    (void)arg;

    echo_server_conn = netconn_new(NETCONN_TCP);
    if (!echo_server_conn) {
        g_print("FAIL: server netconn_new\n");
        return;
    }

    err = netconn_bind(echo_server_conn, IP_ADDR_ANY, 12345);
    if (err != ERR_OK) {
        g_print("FAIL: server bind: %d\n", err);
        netconn_delete(echo_server_conn);
        return;
    }

    err = netconn_listen(echo_server_conn);
    if (err != ERR_OK) {
        g_print("FAIL: server listen: %d\n", err);
        netconn_delete(echo_server_conn);
        return;
    }

    g_print("  Echo server listening on 127.0.0.1:12345\n");

    err = netconn_accept(echo_server_conn, &client);
    if (err != ERR_OK) {
        g_print("FAIL: server accept: %d\n", err);
        netconn_close(echo_server_conn);
        netconn_delete(echo_server_conn);
        return;
    }
    g_print("  Client connected.\n");

    err = netconn_recv(client, &inbuf);
    if (err == ERR_OK && inbuf) {
        u16_t len;
        void *data;
        netbuf_data(inbuf, &data, &len);
        g_print("  Server received: %.*s\n", len, (char *)data);
        netbuf_delete(inbuf);
    }

    err = netconn_write_partly(client, response, strlen(response), NETCONN_COPY, &written);
    g_print("  Server sent %zu bytes.\n", written);

    netconn_close(client);
    netconn_delete(client);
    netconn_close(echo_server_conn);
    netconn_delete(echo_server_conn);
    server_running = FALSE;
}

static void test_lwip_loopback_echo(void)
{
    struct netconn *client;
    err_t err;
    ip_addr_t loopback;
    struct netbuf *inbuf;
    char *message = "Hello lwIP!";
    size_t written;

    IP4_ADDR(&loopback, 127, 0, 0, 1);

    g_print("Test 1: Loopback TCP echo\n");

    /* start echo server in background */
    server_running = TRUE;
    sys_thread_new("echo_srv", echo_server_thread, NULL, 0, 1);

    /* wait for server to start */
    sys_msleep(100);

    client = netconn_new(NETCONN_TCP);
    if (!client) {
        g_print("FAIL: client netconn_new\n");
        return;
    }

    g_print("  Client connecting to %s:12345 ...\n", ipaddr_ntoa(&loopback));
    err = netconn_connect(client, &loopback, 12345);
    if (err != ERR_OK) {
        g_print("FAIL: client connect: %d\n", err);
        netconn_delete(client);
        return;
    }
    g_print("  Client connected.\n");

    err = netconn_write_partly(client, message, strlen(message), NETCONN_COPY, &written);
    if (err != ERR_OK) {
        g_print("FAIL: client write: %d\n", err);
    } else {
        g_print("  Client sent %zu bytes: %s\n", written, message);
    }

    err = netconn_recv(client, &inbuf);
    if (err == ERR_OK && inbuf) {
        u16_t len;
        void *data;
        netbuf_data(inbuf, &data, &len);
        g_print("  Client received: %.*s", len, (char *)data);
        netbuf_delete(inbuf);
        g_print("PASS: Loopback TCP echo works.\n");
    } else {
        g_print("FAIL: client recv: %d\n", err);
    }

    netconn_close(client);
    netconn_delete(client);

    while (server_running) {
        sys_msleep(50);
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    g_print("=== lwIP Loopback TCP Test ===\n\n");

    g_print("Initializing lwIP (loopback only, no TAP needed) ...\n");
    edc_lwip_init_loopback();
    g_print("OK: lwIP initialized.\n\n");

    test_lwip_loopback_echo();

    edc_lwip_shutdown();
    g_print("\nDone.\n");
    return 0;
}
