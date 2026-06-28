#include "src/edc_lwip.h"
#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

static int open_serial(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) { perror("open"); return -1; }
    struct termios t;
    tcgetattr(fd, &t);
    t.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_oflag &= (tcflag_t)~OPOST;
    t.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= (tcflag_t)~(CSIZE | PARENB);
    t.c_cflag |= CS8;
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    tcsetattr(fd, TCSANOW, &t);
    return fd;
}

int main(int argc, char **argv)
{
    const char *pty    = (argc > 1) ? argv[1] : "/tmp/pc_pty";
    const char *cmd    = (argc > 2) ? argv[2] : "id; uname -a";
    const char *dev_ip = "192.168.4.1";
    int port           = 5000;

    setbuf(stdout, NULL);

    int fd = open_serial(pty);
    if (fd < 0) return 1;

    printf("PPPoS init on %s ...\n", pty);
    if (!edc_lwip_init_pppos(fd)) { printf("PPPoS link failed\n"); return 1; }
    printf("Link UP.\n");

    sys_msleep(200);

    ip_addr_t addr;
    ip4addr_aton(dev_ip, &addr);

    struct netconn *c = netconn_new(NETCONN_TCP);
    if (!c) { printf("netconn_new failed\n"); return 1; }
    err_t conn_err = netconn_connect(c, &addr, port);
    if (conn_err != ERR_OK) {
        printf("connect failed (%d)\n", conn_err); return 1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "command", cmd);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "POST /exec HTTP/1.1\r\nHost: %s:%d\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n", dev_ip, port, strlen(body));

    netconn_write_partly(c, hdr, (size_t)hdr_len, NETCONN_COPY, NULL);
    netconn_write_partly(c, body, strlen(body), NETCONN_COPY, NULL);
    free(body);

    struct netbuf *nb; u8_t *p; u16_t chunk; err_t recv_err;
    while ((recv_err = netconn_recv(c, &nb)) == ERR_OK) {
        netbuf_data(nb, (void **)&p, &chunk);
        fwrite(p, 1, chunk, stdout);
        netbuf_delete(nb);
    }

    netconn_close(c);
    netconn_delete(c);
    printf("\nDone.\n");
    return 0;
}
