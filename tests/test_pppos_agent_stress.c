#include "src/edc_pppos_agent.h"

#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/ppp.h"
#include <cJSON.h>
#include "edc_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/wait.h>
#include <signal.h>
#include <pty.h>

#define AGENT_IP   "192.168.4.1"
#define CLIENT_IP  "192.168.4.2"
#define PORT       5000

static ppp_pcb *cl_ppp;
static int cl_fd;
static volatile int cl_link_up;

static u32_t out_cb(ppp_pcb *pcb, const void *d, u32_t len, void *ctx)
{ int fd = *(int *)ctx; (void)pcb; ssize_t w = write(fd, d, len); return (w < 0) ? 0 : (u32_t)w; }
static void stat_cb(ppp_pcb *pcb, int code, void *ctx) { (void)pcb; (void)ctx; cl_link_up = (code == PPPERR_NONE); }
static void *reader(void *arg) { char b[2048]; ssize_t n; (void)arg; while ((n = read(cl_fd, b, sizeof(b))) > 0) pppos_input_tcpip(cl_ppp, b, (int)n); return NULL; }

typedef struct {
    const char *cmd;
    int want_exit;          /* -1 = don't care */
    const char *want_out;   /* NULL = don't care */
    const char *want_err;   /* NULL = don't care */
    const char *desc;
} test_case_t;

static int total_ok = 0, total_fail = 0;

static int do_request(struct netconn *c, const char *cmd, int *exit_code,
                       sds *out, sds *err)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "command", cmd);
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    char hdr[256];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "POST /exec HTTP/1.1\r\nHost: a:%u\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n", PORT, strlen(req_str));

    netconn_write_partly(c, hdr, (size_t)hdr_len, NETCONN_COPY, NULL);
    netconn_write_partly(c, req_str, strlen(req_str), NETCONN_COPY, NULL);
    free(req_str);

    u8_t buf[131072]; size_t len = 0;
    struct netbuf *nb; u8_t *p; u16_t chunk; err_t e;
    while ((e = netconn_recv(c, &nb)) == ERR_OK) {
        netbuf_data(nb, (void **)&p, &chunk);
        if (len + chunk > sizeof(buf) - 1) chunk = (u16_t)(sizeof(buf) - 1 - len);
        memcpy(buf + len, p, chunk); len += chunk;
        netbuf_delete(nb);
    }
    buf[len] = '\0';

    char *body = strstr((char *)buf, "\r\n\r\n");
    if (!body) return -1;
    body += 4;

    cJSON *resp = cJSON_Parse(body);
    if (!resp) return -1;

    cJSON *ec = cJSON_GetObjectItem(resp, "exit_code");
    cJSON *so = cJSON_GetObjectItem(resp, "stdout");
    cJSON *se = cJSON_GetObjectItem(resp, "stderr");

    *exit_code = ec ? ec->valueint : -999;
    *out = edc_str_new(so && cJSON_IsString(so) ? so->valuestring : "");
    *err = edc_str_new(se && cJSON_IsString(se) ? se->valuestring : "");

    cJSON_Delete(resp);
    return 0;
}

static struct netconn *new_conn_timeout(ip_addr_t *addr, int timeout_ms)
{
    struct netconn *c;
    err_t err;
    int waited = 0;

    while (waited < timeout_ms) {
        c = netconn_new(NETCONN_TCP);
        if (!c) { sys_msleep(100); waited += 100; continue; }
        err = netconn_connect(c, addr, PORT);
        if (err == ERR_OK) return c;
        netconn_delete(c);
        sys_msleep(200);
        waited += 200;
    }
    return NULL;
}

static void close_conn(struct netconn *c)
{
    netconn_close(c);
    netconn_delete(c);
    sys_msleep(200);
}

static int run_one(ip_addr_t *addr, test_case_t *tc)
{
    struct netconn *c = new_conn_timeout(addr, 5000);
    if (!c) { printf("  FAIL [%s]: connect timeout\n", tc->desc); total_fail++; return 0; }

    int exit_code, ok = 1;
    sds out, *err;

    if (do_request(c, tc->cmd, &exit_code, &out, &err) != 0) {
        printf("  FAIL [%s]: request failed\n", tc->desc);
        close_conn(c); total_fail++; return 0;
    }
    close_conn(c);

    if (tc->want_exit >= 0 && exit_code != tc->want_exit) {
        printf("  FAIL [%s]: exit_code=%d want=%d\n", tc->desc, exit_code, tc->want_exit);
        ok = 0;
    }
    if (tc->want_out && !strstr(out, tc->want_out)) {
        printf("  FAIL [%s]: stdout missing '%s' got='%s'\n",
               tc->desc, tc->want_out, out);
        ok = 0;
    }
    if (tc->want_err && !strstr(err, tc->want_err)) {
        printf("  FAIL [%s]: stderr missing '%s' got='%s'\n",
               tc->desc, tc->want_err, err);
        ok = 0;
    }

    if (ok) { total_ok++; printf("  OK   [%s]\n", tc->desc); }
    else total_fail++;

    edc_str_free(out);
    edc_str_free(err);
    return ok;
}

int main(void)
{
    int m, s; pid_t pid; struct termios t;

    setbuf(stdout, NULL);
    printf("=== lwIP PPPoS Agent Stress Test ===\n\n");

    openpty(&m, &s, NULL, NULL, NULL);
    tcgetattr(s, &t); cfmakeraw(&t); tcsetattr(s, TCSANOW, &t);
    signal(SIGPIPE, SIG_IGN);

    pid = fork();
    if (pid == 0) { close(m); edc_pppos_agent_run(s, AGENT_IP, CLIENT_IP, PORT); return 0; }
    close(s);

    /* ---- client init ---- */
    struct netif ni; ip4_addr_t a, g, nm;
    cl_fd = m;
    tcpip_init(NULL, NULL);
    srand((unsigned)(sys_now() ^ getpid()));
    ip4addr_aton(CLIENT_IP, &a);
    ip4addr_aton(AGENT_IP, &g);
    IP4_ADDR(&nm, 255, 255, 255, 0);
    memset(&ni, 0, sizeof(ni));
    ip_addr_copy(ni.ip_addr, a); ip_addr_copy(ni.gw, g); ip_addr_copy(ni.netmask, nm);
    cl_ppp = pppapi_pppos_create(&ni, out_cb, stat_cb, &cl_fd);
    pppapi_set_default(cl_ppp);
    ppp_set_auth(cl_ppp, PPPAUTHTYPE_NONE, NULL, NULL);
    ppp_set_ipcp_ouraddr(cl_ppp, &a); ppp_set_ipcp_hisaddr(cl_ppp, &g);
    sys_thread_new("r", (lwip_thread_fn)reader, NULL, 0, 1);
    sys_msleep(50); pppapi_connect(cl_ppp, 0);

    printf("Waiting for PPP link ...\n");
    while (!cl_link_up) sys_msleep(50);
    printf("Link UP.\n\n");
    sys_msleep(500);

    ip_addr_t addr; ip4addr_aton(AGENT_IP, &addr);

    test_case_t tests[] = {
        /* basic */
        {"echo hello", 0, "hello", NULL, "simple echo"},
        {"echo hello >&2", 0, NULL, "hello", "stderr echo"},
        {"false", 1, NULL, NULL, "exit code 1"},
        {"true", 0, NULL, NULL, "exit code 0"},
        {"exit 42", 42, NULL, NULL, "exit code 42"},
        /* empty / whitespace */
        {"", 0, "", "", "empty command"},
        {"   ", 0, "", "", "whitespace command"},
        /* special chars */
        {"echo 'line1\nline2\nline3'", 0, "line1\nline2\nline3", NULL, "multiline output"},
        {"echo 'a\"b'", 0, "a\"b", NULL, "double quote in output"},
        {"printf '\\x00\\x01\\x02'", 0, NULL, NULL, "binary output"},
        /* large output */
        {"dd if=/dev/zero bs=1024 count=64 2>/dev/null | base64", 0, NULL, NULL, "64KB binary-as-base64"},
        {"for i in $(seq 1 200); do echo $i; done", 0, "200", NULL, "200 lines stdout"},
        /* env */
        {"echo $HOME", 0, "/", NULL, "env HOME defined"},
        {"echo $PATH", 0, NULL, NULL, "env PATH defined"},
        /* command not found */
        {"nonexistent_command_xyz", 127, NULL, "not found", "command not found"},
        /* signal */
        {"kill -9 $$", -1, NULL, NULL, "killed by signal"},
        /* pipeline */
        {"echo foo | grep foo", 0, "foo", NULL, "simple pipeline"},
        /* redirection */
        {"cat /dev/null 2>/dev/null; echo ok", 0, "ok", NULL, "stdin null"},
        /* concurrent stdout+stderr */
        {"(echo out1; echo err1 >&2; echo out2; echo err2 >&2)", 0, "out1", "err2", "interleaved"},
    };
    int n = sizeof(tests) / sizeof(tests[0]);

    for (int i = 0; i < n; i++) {
        run_one(&addr, &tests[i]);
    }

    /* ---- rapid-fire: 50 iterations ---- */
    printf("\n  [rapid-fire: 50 iterations]\n");
    {
        sds o = NULL, *e = NULL; int ec; int rok = 0, rfail = 0;
        for (int i = 0; i < 50; i++) {
            char rcmd[64];
            snprintf(rcmd, sizeof(rcmd), "echo iter-%d", i);
            struct netconn *rc = new_conn_timeout(&addr, 5000);
            if (rc) {
                if (do_request(rc, rcmd, &ec, &o, &e) == 0 && ec == 0 && strstr(o, "iter-")) rok++;
                else rfail++;
                close_conn(rc);
            } else rfail++;
            if (o) edc_str_free(o);
            if (e) edc_str_free(e);
            o = e = NULL;
        }
        printf("  OK=%d FAIL=%d\n", rok, rfail);
        total_ok += rok; total_fail += rfail;
    }

    /* ---- large payload ---- */
    printf("\n  [large payload: 64KB response]\n");
    {
        sds o = NULL, *e = NULL; int ec;
        struct netconn *lc = new_conn_timeout(&addr, 5000);
        if (lc && do_request(lc, "dd if=/dev/urandom bs=1024 count=64 2>/dev/null | wc -c",
                             &ec, &o, &e) == 0 && ec == 0 && strstr(o, "65536")) {
            printf("  OK: 65536 bytes received\n"); total_ok++;
        } else {
            printf("  FAIL\n"); total_fail++;
        }
        if (lc) close_conn(lc);
        if (o) edc_str_free(o);
        if (e) edc_str_free(e);
    }

    printf("\n=== Results: OK=%d FAIL=%d ===\n", total_ok, total_fail);

    kill(pid, SIGTERM); wait(NULL);
    return total_fail > 0 ? 1 : 0;
}
