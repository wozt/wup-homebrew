/* lwIP options: lwIP inside the GamePad's stock firmware, on raw Ethernet frames of the Broadcom radio (wup_netif.c).
 * Two ways (app.cfg): net = yes (WUP_NET_SOCKETS): lwIP's sockets, the stack in its own threads (sys_arch.c);
 * net = raw: lwIP's callback API only, polled from the app's loop (NO_SYS): the menu. */
#pragma once
#ifdef WUP_NET_SOCKETS
#define NO_SYS                      0
#define LWIP_SOCKET                 1
#define LWIP_NETCONN                1
#define SYS_LIGHTWEIGHT_PROT        1
#define LWIP_COMPAT_SOCKETS         0    /* the standard names are real functions (src/net.c), not macros */
#define LWIP_POSIX_SOCKETS_IO_NAMES 0
#define LWIP_TCPIP_CORE_LOCKING     1    /* socket calls run the stack themselves under its lock: no hand-over per call */
#define LWIP_SOCKET_OFFSET          3    /* descriptors 3..: files are 32.. (src/sys.c) */
#ifdef WUP_NET_NETCONN
#define MEMP_NUM_NETCONN            WUP_NET_NETCONN
#else
#define MEMP_NUM_NETCONN            12
#endif
#ifdef WUP_NET_UDP_QUEUE
#define MEMP_NUM_NETBUF             (WUP_NET_UDP_QUEUE + 16)
#elif defined(WUP_NET_NETBUF)
#define MEMP_NUM_NETBUF             WUP_NET_NETBUF
#else
#define MEMP_NUM_NETBUF             48
#endif
#define MEMP_NUM_TCPIP_MSG_API      8
#define MEMP_NUM_TCPIP_MSG_INPKT    16
#define LWIP_SO_RCVTIMEO            1
#define LWIP_SO_SNDTIMEO            1
#define LWIP_SO_RCVBUF              1
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_HAVE_LOOPIF            1    /* 127.0.0.1 */
#define LWIP_NETIF_LOOPBACK         1
#define LWIP_TIMEVAL_PRIVATE        0
#define LWIP_ERRNO_STDINCLUDE       1
#define TCPIP_THREAD_STACKSIZE      4096
#define TCPIP_THREAD_PRIO           1    /* (libwup: more urgent than the app) */
#define TCPIP_MBOX_SIZE             16
#define DEFAULT_THREAD_STACKSIZE    4096
#define DEFAULT_TCP_RECVMBOX_SIZE   16
#ifdef WUP_NET_UDP_QUEUE                 /* (app.cfg defines: datagrams a socket holds; a stream's bursts want more) */
#define DEFAULT_UDP_RECVMBOX_SIZE   WUP_NET_UDP_QUEUE
#else
#define DEFAULT_UDP_RECVMBOX_SIZE   32
#endif
#define DEFAULT_RAW_RECVMBOX_SIZE   8
#define DEFAULT_ACCEPTMBOX_SIZE     4
#define RECV_BUFSIZE_DEFAULT        (64 * 1024)
#else
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define SYS_LIGHTWEIGHT_PROT        0
#endif
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_DHCP                   1
#define LWIP_ACD                    0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DNS                    1
#define LWIP_IGMP                   0
#define LWIP_NETIF_STATUS_CALLBACK  1
#ifndef WUP_NET
#define LWIP_STATS                  0
#endif
#define MEM_ALIGNMENT               4
#ifdef WUP_NET                  /* apps: libwup's networking (src/net.c, src/wup_netif.c) */
#ifdef WUP_NET_MEM_SIZE
#define MEM_SIZE                    WUP_NET_MEM_SIZE
#else
#define MEM_SIZE                    (48 * 1024)
#endif
/* received frames live in the pool until the app reads them: more than a full TCP window, and out-of-
   order segments capped, so ACKs always find a buffer (else a busy socket stalls on retransmits) */
#ifdef WUP_NET_PBUF_POOL
#define PBUF_POOL_SIZE              WUP_NET_PBUF_POOL
#else
#define PBUF_POOL_SIZE              48
#endif
#define TCP_QUEUE_OOSEQ             1
#define TCP_OOSEQ_MAX_PBUFS         6
#define LWIP_STATS                  1
#define MEMP_STATS                  1
#define LINK_STATS                  0
#define ETHARP_STATS                0
#define IP_STATS                    0
#define IPFRAG_STATS                0
#define ICMP_STATS                  0
#define UDP_STATS                   0
#define TCP_STATS                   1
#define MEM_STATS                   1
#define SYS_STATS                   0
#define MEMP_NUM_PBUF               32
#define PBUF_POOL_BUFSIZE           1536
#define MEMP_NUM_UDP_PCB            12
#ifdef WUP_NET_TCP_PCB
#define MEMP_NUM_TCP_PCB            WUP_NET_TCP_PCB
#else
#define MEMP_NUM_TCP_PCB            16   /* the menu: FTP's 3 sessions (control + data), Netloader, debug, TIME_WAIT */
#endif
#ifdef WUP_NET_TCP_PCB_LISTEN
#define MEMP_NUM_TCP_PCB_LISTEN     WUP_NET_TCP_PCB_LISTEN
#else
#define MEMP_NUM_TCP_PCB_LISTEN     6    /* the menu: FTP, its 3 passive data ports, Netloader, debug */
#endif
#define SO_REUSE                    1    /* a data port can be listened on again while its last connection lingers */
#ifdef WUP_NET_TCP_SEG
#define MEMP_NUM_TCP_SEG            WUP_NET_TCP_SEG
#else
#define MEMP_NUM_TCP_SEG            48
#endif
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 3)
/* SNTP (lwIP's client; the Homebrew Menu's clock, wup-menu: menu/clock.c) */
#define SNTP_SERVER_DNS             1
#define SNTP_STARTUP_DELAY          0
#define SNTP_SET_SYSTEM_TIME_US(sec, us) clock_set_from_network((sec), (us))
void clock_set_from_network(unsigned sec, unsigned us);
#define TCP_MSS                     1460
#define TCP_WND                     (12 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS)
#define DNS_MAX_SERVERS             2
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1   /* <netinet/in.h> has htons & co. */
#define LWIP_RAND()                 wup_netif_rand()
unsigned wup_netif_rand(void);
/* the heap and pools in .bss.net: a large app keeps them next to its code in WUP_SPARE (wupbuild.py) */
#ifdef __arm__
#define LWIP_DECLARE_MEMORY_ALIGNED(variable_name, size) \
    u8_t variable_name[LWIP_MEM_ALIGN_BUFFER(size)] __attribute__((section(".bss.net"), aligned(4)))
#endif
#else
#ifdef WUP_NET_EMBEDDED                /* (unused) */
#define MEM_SIZE                    (32 * 1024)
#define PBUF_POOL_SIZE              24
#else
#define MEM_SIZE                    (64 * 1024)
#define PBUF_POOL_SIZE              32
#endif
#define MEMP_NUM_PBUF               32
#define PBUF_POOL_BUFSIZE           1536
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_TCP_PCB            16   /* FTP: 3 sessions (control + data), plus wuplink, debug, TIME_WAIT */
#define MEMP_NUM_TCP_PCB_LISTEN     6    /* FTP control + 3 passive data ports, wuplink, debug */
#define MEMP_NUM_TCP_SEG            32
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS)
#define LWIP_RAND()                 wup_net_rand()
unsigned wup_net_rand(void);
#endif
