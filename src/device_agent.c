#include "edc_device_agent.h"
#include "edc_transport_pppos.h"
#include "edc_transport_socket.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int open_serial(const char *path, int baud)
{
    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0)
    {
        perror("open serial");
        return -1;
    }

    struct termios t;
    tcgetattr(fd, &t);
    t.c_iflag &= (tcflag_t) ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_oflag &= (tcflag_t)~OPOST;
    t.c_lflag &= (tcflag_t) ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= (tcflag_t) ~(CSIZE | PARENB);
    t.c_cflag |= CS8;
    cfsetispeed(&t, (speed_t)baud);
    cfsetospeed(&t, (speed_t)baud);
    tcsetattr(fd, TCSANOW, &t);
    return fd;
}

int main(int argc, char **argv)
{
    const char *serial = "/dev/ttyS0";
    const char *my_ip = "192.168.4.1";
    const char *pc_ip = "192.168.4.2";
    int port = 5000;
    int baud = 115200;
    int use_socket = 0;

    signal(SIGPIPE, SIG_IGN);

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--socket") == 0)
            use_socket = 1;
        else if (strcmp(argv[i], "--help") == 0)
        {
            fprintf(stderr,
                "Usage: device_agent [--socket] [serial] [my_ip] [pc_ip] [port] [baud]\n"
                "  --socket           Use TCP socket transport (ignore serial args)\n"
                "                     Args become: [bind_ip] [port]\n"
                "  Default PPPoS mode: device_agent [serial] [my_ip] [pc_ip] [port] [baud]\n");
            return 0;
        }
    }

    if (use_socket)
    {
        const char *bind_ip = "0.0.0.0";
        int ip_set = 0;
        for (int i = 1; i < argc; i++)
        {
            if (strcmp(argv[i], "--socket") == 0)
                continue;
            if (!ip_set)
            {
                bind_ip = argv[i];
                ip_set = 1;
            }
            else
                port = atoi(argv[i]);
        }
        fprintf(stderr, "device_agent: socket mode bind_ip=%s port=%d\n", bind_ip, port);

        struct edc_device_transport *transport = edc_transport_socket_create(bind_ip, (uint16_t)port);
        if (!transport)
            return 1;
        edc_device_agent_run(transport);
        transport->cleanup(transport);
        return 0;
    }

    if (argc > 1)
        serial = argv[1];
    if (argc > 2)
        my_ip = argv[2];
    if (argc > 3)
        pc_ip = argv[3];
    if (argc > 4)
        port = atoi(argv[4]);
    if (argc > 5)
        baud = atoi(argv[5]);

    fprintf(stderr, "device_agent: serial=%s my_ip=%s pc_ip=%s port=%d baud=%d\n", serial, my_ip, pc_ip, port, baud);

    int fd = open_serial(serial, baud);
    if (fd < 0)
        return 1;

    struct edc_device_transport *transport = edc_transport_pppos_create(fd, my_ip, pc_ip, (uint16_t)port);
    if (!transport)
        return 1;

    edc_device_agent_run(transport);
    transport->cleanup(transport);
    return 0;
}
