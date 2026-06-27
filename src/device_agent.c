#include "edc_pppos_agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>

static int open_serial(const char *path, int baud)
{
    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror("open serial");
        return -1;
    }

    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    cfsetspeed(&t, (speed_t)baud);
    tcsetattr(fd, TCSANOW, &t);
    return fd;
}

int main(int argc, char **argv)
{
    const char *serial = "/dev/ttyS0";
    const char *my_ip  = "192.168.4.1";
    const char *pc_ip  = "192.168.4.2";
    int port = 5000;
    int baud = 115200;

    signal(SIGPIPE, SIG_IGN);

    if (argc > 1) serial = argv[1];
    if (argc > 2) my_ip  = argv[2];
    if (argc > 3) pc_ip  = argv[3];
    if (argc > 4) port   = atoi(argv[4]);

    fprintf(stderr, "device_agent: serial=%s my_ip=%s pc_ip=%s port=%d baud=%d\n",
            serial, my_ip, pc_ip, port, baud);

    int fd = open_serial(serial, baud);
    if (fd < 0) return 1;

    edc_pppos_agent_run(fd, my_ip, pc_ip, (uint16_t)port);
    return 0;
}
