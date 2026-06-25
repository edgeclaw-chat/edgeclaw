#include "src/edc_lwip.h"
#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include <glib.h>
#include <stdlib.h>
#include <string.h>

static err_t test_lwip_tcp_connect(ip_addr_t *addr, u16_t port)
{
    struct netconn *conn;
    err_t err;

    conn = netconn_new(NETCONN_TCP);
    if (!conn) {
        g_print("FAIL: netconn_new returned NULL\n");
        return ERR_MEM;
    }

    g_print("  Connecting to %s:%u ...\n", ipaddr_ntoa(addr), port);
    err = netconn_connect(conn, addr, port);
    if (err != ERR_OK) {
        g_print("FAIL: netconn_connect returned %d\n", err);
        netconn_delete(conn);
        return err;
    }
    g_print("  Connected.\n");
    netconn_close(conn);
    netconn_delete(conn);
    return ERR_OK;
}

static void test_lwip_http_get(ip_addr_t *addr)
{
    struct netconn *conn;
    err_t err;
    const char *request =
        "GET /get HTTP/1.1\r\n"
        "Host: httpbin.org\r\n"
        "Connection: close\r\n"
        "\r\n";
    char buf[4096];
    size_t written;
    int total_read = 0;
    gboolean header_ok = FALSE;

    conn = netconn_new(NETCONN_TCP);
    if (!conn) {
        g_print("FAIL: netconn_new returned NULL\n");
        return;
    }

    g_print("  Connecting to httpbin.org (%s):80 ...\n", ipaddr_ntoa(addr));
    err = netconn_connect(conn, addr, 80);
    if (err != ERR_OK) {
        g_print("FAIL: netconn_connect returned %d\n", err);
        netconn_delete(conn);
        return;
    }
    g_print("  Connected.\n");

    g_print("  Sending HTTP GET ...\n");
    err = netconn_write_partly(conn, request, strlen(request), NETCONN_COPY, &written);
    if (err != ERR_OK) {
        g_print("FAIL: netconn_write returned %d\n", err);
        netconn_close(conn);
        netconn_delete(conn);
        return;
    }
    g_print("  Sent %zu bytes, reading response ...\n", written);

    while (1) {
        struct netbuf *inbuf;
        err = netconn_recv(conn, &inbuf);
        if (err != ERR_OK) {
            break;
        }
        if (inbuf) {
            u16_t len;
            void *data;
            netbuf_data(inbuf, &data, &len);
            if (len > sizeof(buf) - 1) {
                len = sizeof(buf) - 1;
            }
            memcpy(buf, data, len);
            buf[len] = '\0';
            total_read += len;

            if (!header_ok && strstr(buf, "200 OK")) {
                header_ok = TRUE;
            }
            netbuf_delete(inbuf);
        }
    }

    netconn_close(conn);
    netconn_delete(conn);

    g_print("  Total bytes received: %d\n", total_read);
    if (header_ok) {
        g_print("  PASS: HTTP 200 OK received.\n");
    } else {
        g_print("  FAIL: HTTP 200 not found in response.\n");
    }
}

int main(int argc, char **argv)
{
    const char *ipaddr_str;
    const char *netmask_str;
    const char *gw_str;
    const char *httpbin_ip;
    ip_addr_t httpbin_addr;
    err_t err;

    (void)argc;
    (void)argv;

    if (!getenv("PRECONFIGURED_TAPIF")) {
        setenv("PRECONFIGURED_TAPIF", "tap0", 1);
    }

    ipaddr_str = getenv("LWIP_IPADDR");
    if (!ipaddr_str) {
        ipaddr_str = "192.168.100.2";
    }
    netmask_str = getenv("LWIP_NETMASK");
    if (!netmask_str) {
        netmask_str = "255.255.255.0";
    }
    gw_str = getenv("LWIP_GATEWAY");
    if (!gw_str) {
        gw_str = "192.168.100.1";
    }

    httpbin_ip = getenv("HTTPBIN_IP");
    if (httpbin_ip) {
        if (!ip4addr_aton(httpbin_ip, &httpbin_addr)) {
            g_print("Invalid HTTPBIN_IP: %s\n", httpbin_ip);
            return 1;
        }
    } else {
        IP4_ADDR(&httpbin_addr, 8, 8, 8, 8);
        g_print("HTTPBIN_IP not set, using 8.8.8.8 (DNS server) for TCP test\n");
    }

    g_print("\n=== lwIP TCP/IP test ===\n");
    g_print("TAP: %s  LWIP_IP: %s  GW: %s\n\n",
            getenv("PRECONFIGURED_TAPIF"), ipaddr_str, gw_str);

    g_print("Initializing lwIP ...\n");
    if (!edc_lwip_init(ipaddr_str, netmask_str, gw_str)) {
        g_print("FAIL: lwIP init failed.\n");
        return 1;
    }
    g_print("OK: lwIP initialized.\n\n");

    /* Test 1: TCP connect */
    g_print("Test 1: TCP connect to %s:53\n", ipaddr_ntoa(&httpbin_addr));
    err = test_lwip_tcp_connect(&httpbin_addr, 53);
    if (err == ERR_OK) {
        g_print("PASS: TCP connect succeeded.\n\n");
    } else {
        g_print("SKIP: TCP connect failed (check TAP routing/NAT).\n\n");
    }

    /* Test 2: HTTP GET (only with HTTPBIN_IP) */
    if (httpbin_ip) {
        g_print("Test 2: HTTP GET to httpbin.org\n");
        test_lwip_http_get(&httpbin_addr);
    } else {
        g_print("Test 2: SKIP (set HTTPBIN_IP env to test HTTP)\n");
    }

    edc_lwip_shutdown();
    g_print("\nDone.\n");
    return 0;
}
