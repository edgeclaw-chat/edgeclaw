#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

#define NO_SYS                      0
#define LWIP_SOCKET                 1
#define LWIP_NETCONN                1

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0

#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1

#define LWIP_DNS                    1
#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0

#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_API              1
#define LWIP_NETIF_LOOPBACK         1

#define MEM_ALIGNMENT               4
#define MEM_SIZE                    16000

#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_RAW_PCB            4

#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)

#define TCPIP_THREAD_STACKSIZE      0
#define TCPIP_THREAD_PRIO           1
#define TCPIP_MBOX_SIZE             64
#define DEFAULT_RAW_RECVMBOX_SIZE   8
#define DEFAULT_UDP_RECVMBOX_SIZE   8
#define DEFAULT_TCP_RECVMBOX_SIZE   8
#define DEFAULT_ACCEPTMBOX_SIZE     8

#define LWIP_TCPIP_CORE_LOCKING     1
#define LWIP_TCPIP_CORE_LOCKING_INPUT 0

#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

#endif
