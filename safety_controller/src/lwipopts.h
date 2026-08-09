#ifndef __LWIPOPTS_H_
#define __LWIPOPTS_H_

#define NO_SYS                  1
#define SYS_LIGHTWEIGHT_PROT    0
#define LWIP_SOCKET             0
#define LWIP_NETCONN            0
#define LWIP_DHCP               0
#define LWIP_UDP                1
#define LWIP_TCP                0
#define LWIP_ICMP               1
#define LWIP_ARP                1
#define LWIP_RAW                0
#define LWIP_DNS                0
#define LWIP_IGMP               0
#define LWIP_STATS              0
#define LWIP_NETIF_API          0

#define MEM_ALIGNMENT           64
#define MEM_SIZE                (256 * 1024)
#define MEMP_NUM_PBUF           16
#define MEMP_NUM_UDP_PCB        4
#define PBUF_POOL_SIZE          256
#define PBUF_POOL_BUFSIZE       1700

#define CHECKSUM_GEN_UDP        1
#define CHECKSUM_CHECK_UDP      1

#endif
