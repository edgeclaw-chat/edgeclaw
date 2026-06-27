#include "edc_device_agent.h"
#include "edc_transport_socket.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *bind_ip = "0.0.0.0";
    int port = 5000;

    signal(SIGPIPE, SIG_IGN);

    if (argc > 1)
        bind_ip = argv[1];
    if (argc > 2)
        port = atoi(argv[2]);

    fprintf(stderr, "device_agent_socket: bind_ip=%s port=%d\n", bind_ip, port);

    struct edc_device_transport *transport = edc_transport_socket_create(bind_ip, (uint16_t)port);
    if (!transport)
        return 1;

    edc_device_agent_run(transport);
    transport->cleanup(transport);
    return 0;
}
