//! The network of tests, src/net.c, which only they link: src/io.c's calls on their TCP and UDP sockets, in bober_logical.

/// Its calls; all 0 where it is not linked, as src/io.c defines them weakly.
extern struct bober_net {
  long (*dial)(const char *net, const char *host, int port);
  long (*listen)(const char *net, const char *host, int port);
  long (*accept)(int fd);
  long (*connected)(int fd);
  long (*read)(int fd, void *buf, unsigned long n);
  long (*write)(int fd, const void *buf, unsigned long n);
  long (*recvfrom)(int fd, void *buf, unsigned long n, char *host, int *port);
  long (*sendto)(int fd, const void *buf, unsigned long n, const char *host, int port);
  long (*shutdown)(int fd, int both);
  int (*idle)(int fd);
  long (*name)(int fd, int peer, char *host);
  /// Closes fd, with a reset if reset.
  long (*close)(int fd, int reset);
  /// -1 if fd is not the network's, else whether it is ready to read or, if write, to write.
  int (*ready)(int fd, int write);
  /// Delivers what the network sent and runs its due timers: the bober_mono() of its next timer while an ack is
  /// delayed, which only a timer sends; else 0.
  long (*poll)(void);
} bober_net;
/// Wakes what waits for fd to be ready to read or write (src/io.c).
void net_edge(int fd, int write);
