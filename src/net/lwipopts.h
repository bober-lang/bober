//! lwIP's options for src/net.c: no OS, TCP and UDP over the IPv4 and IPv6 loopback alone, memory from malloc.

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define MEM_LIBC_MALLOC 1
#define MEMP_MEM_MALLOC 1
#define MEM_ALIGNMENT 8
#define LWIP_IPV6 1
#define LWIP_ARP 0
#define LWIP_IPV6_MLD 0
#define LWIP_IPV6_AUTOCONFIG 0
#define LWIP_IPV6_DUP_DETECT_ATTEMPTS 0
#define LWIP_IPV6_FRAG 0
#define LWIP_IPV6_REASS 0
#define LWIP_ND6_QUEUEING 0
#define IP_FRAG 0
#define IP_REASSEMBLY 0
#define LWIP_NETIF_LOOPBACK 1
#define LWIP_HAVE_LOOPIF 1
#define SO_REUSE 1
#define LWIP_STATS 0
#define TCP_QUEUE_OOSEQ 0
#define TCP_TMR_INTERVAL 40  // an ack is delayed 40 ms at most, as Linux's
#define TCP_WND_UPDATE_THRESHOLD 1  // a read opens a closed window at once: no probe need find it
#define CHECKSUM_GEN_IP 0
#define CHECKSUM_GEN_UDP 0
#define CHECKSUM_GEN_TCP 0
#define CHECKSUM_GEN_ICMP 0
#define CHECKSUM_GEN_ICMP6 0
#define CHECKSUM_CHECK_IP 0
#define CHECKSUM_CHECK_UDP 0
#define CHECKSUM_CHECK_TCP 0
#define CHECKSUM_CHECK_ICMP 0
#define CHECKSUM_CHECK_ICMP6 0
// A socket buffers 256 KiB each way, as a kernel's starts to, in segments of 16 KiB, the most lwIP takes.
#define LWIP_WND_SCALE 1
#define TCP_RCV_SCALE 3
#define TCP_MSS 16000
#define TCP_WND (256 * 1024)
#define TCP_SND_BUF (256 * 1024)
#define TCP_SND_QUEUELEN 1024
#define LWIP_DISABLE_TCP_SANITY_CHECKS 1  // they guard 16-bit windows, which scaling widens
#define LWIP_RAND() 0  // where ports start: the same in each run, and not of the random numbers of tests
