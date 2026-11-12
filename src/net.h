//! The network of tests, src/net.c, which only they link: src/io.c's calls on their TCP and UDP sockets, in bober_logical.

#ifndef WEAK
#define WEAK __attribute__((weak))  // 0 where not linked
#endif
WEAK long net_dial(const char *net, const char *host, int port);
WEAK long net_listen(const char *net, const char *host, int port);
WEAK long net_accept(int fd);
WEAK long net_connected(int fd);
WEAK long net_read(int fd, void *buf, unsigned long n);
WEAK long net_write(int fd, const void *buf, unsigned long n);
WEAK long net_recvfrom(int fd, void *buf, unsigned long n, char *host, int *port);
WEAK long net_sendto(int fd, const void *buf, unsigned long n, const char *host, int port);
WEAK long net_shutdown(int fd, int both);
WEAK int net_idle(int fd);
WEAK long net_name(int fd, int peer, char *host);
/// Closes fd, with a reset if reset.
WEAK long net_close(int fd, int reset);
/// -1 if fd is not the network's, else whether it is ready to read or, if write, to write.
WEAK int net_ready(int fd, int write);
/// Delivers what the network sent and runs its due timers: the bober_mono() of its next timer while an ack is delayed,
/// which only a timer sends; else 0.
WEAK long net_poll(void);
/// Wakes what waits for fd to be ready to read or write (src/io.c).
void net_edge(int fd, int write);
#undef WEAK
