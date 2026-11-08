//! The I/O of src/rt.h, through the libc of the target: the subset of POSIX that Linux, macOS and FreeBSD share. Only
//! sockets wait: they are non-blocking, a call on one that would block gives BOBER_IO_AGAIN, and bober_io_ready parks its
//! fiber until poll finds the socket ready; once no fiber is ready, bober_park polls (bober_io_wait). Without
//! src/fiber.c, as in boot, the weak bober_park below polls in place.

#define _GNU_SOURCE  // statx
#include "rt.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

uint64_t __attribute__((weak)) bober_self(void) { return 0; }
void __attribute__((weak)) bober_wake(uint64_t f) {}
void __attribute__((weak)) bober_park(void) { bober_io_wait(); }

int64_t bober_mono(void) {
  struct timespec t;
  return clock_gettime(CLOCK_MONOTONIC, &t), t.tv_sec * 1000000000 + t.tv_nsec;
}

/// A fiber parked until fd is ready, on its stack: done once it is, -1 if its deadline passed first.
typedef struct Wait { uint64_t fiber; int fd, write, done; long deadline; struct Wait *next; } Wait;
static Wait *waits;

/// What a call that gave r gives: r, or the failure errno is, BOBER_IO_AGAIN for one that would block.
static long result(long r) {
  return r >= 0 ? r : errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? BOBER_IO_AGAIN : -errno;
}

long bober_io_ready(int fd, int write, long timeout) {
  long t = bober_mono();  // a deadline past the largest long saturates to it, which never comes
  Wait w = {bober_self(), fd, write, 0, !timeout ? 0 : timeout > LONG_MAX - t ? LONG_MAX : t + timeout, waits};
  waits = &w;
  while (!w.done) bober_park();
  return w.done < 0 ? -ETIMEDOUT : 0;
}

int bober_io_wait(void) {
  static struct pollfd *fds;
  static int cap;
  int n = 0;
  long soonest = 0, t = bober_mono();
  for (Wait *w = waits; w; w = w->next, n++) {
    if (n == cap && !(fds = realloc(fds, (cap = cap ? 2 * cap : 64) * sizeof *fds))) fputs("out of memory\n", stderr), exit(1);
    fds[n] = (struct pollfd){w->fd, w->write ? POLLOUT : POLLIN, 0};
    if (w->deadline && (!soonest || w->deadline < soonest)) soonest = w->deadline;
  }
  if (!n) return 0;
  // In ms rounded up, at most poll's int; a later deadline is polled for again.
  long ms = !soonest ? -1 : soonest <= t ? 0 : (soonest - t - 1) / 1000000 + 1;
  if (poll(fds, n, ms > INT_MAX ? INT_MAX : ms) < 0 && errno != EINTR) perror("poll"), exit(1);
  t = bober_mono();
  // bober_wake only queues, so the list keeps the order fds has.
  Wait **p = &waits;
  for (int i = 0; *p; i++) {
    Wait *w = *p;
    if (fds[i].revents || (w->deadline && w->deadline <= t))
      w->done = fds[i].revents ? 1 : -1, *p = w->next, bober_wake(w->fiber);
    else p = &w->next;
  }
  return 1;
}

void bober_io_cancel(void) { waits = 0; }

const char *bober_io_kind(long r) {
  switch (-r) {
  case ENOENT: return "NotFound";
  case EEXIST: return "AlreadyExists";
  case EACCES: case EPERM: return "PermissionDenied";
  case EISDIR: return "IsADirectory";
  case ENOTDIR: return "NotADirectory";
  case ENOTEMPTY: return "NotEmpty";
  case EPIPE: return "BrokenPipe";
  case EBADF: return "BadResource";
  case ECONNREFUSED: return "ConnectionRefused";
  case ECONNRESET: return "ConnectionReset";
  case ECONNABORTED: return "ConnectionAborted";
  case ENOTCONN: return "NotConnected";
  case EADDRINUSE: return "AddrInUse";
  case EADDRNOTAVAIL: return "AddrNotAvailable";
  case ETIMEDOUT: return "TimedOut";
  case EPROTO: return "Protocol";
  case EMSGSIZE: return "TooLarge";
  case EINVAL: return "InvalidInput";
  case EMFILE: case ENFILE: case ENOBUFS: case ENOMEM: return "OutOfResources";
  case EPROTONOSUPPORT: case EAFNOSUPPORT: return "Unsupported";
  default: return "Other";
  }
}

long bober_io_open(const char *path, const char *mode) {
  int flags = mode[1] == '+' ? O_RDWR : mode[0] == 'r' ? O_RDONLY : O_WRONLY;
  flags |= mode[0] == 'w' ? O_CREAT | O_TRUNC : mode[0] == 'a' ? O_CREAT | O_APPEND : mode[0] == 'x' ? O_CREAT | O_EXCL : 0;
  return result(open(path, flags | O_CLOEXEC, 0666));
}

long bober_io_close(int fd) {
  // What waits on fd wakes to find it closed, before another descriptor can take its number.
  for (Wait **p = &waits; *p;)
    if ((*p)->fd == fd) (*p)->done = 1, bober_wake((*p)->fiber), *p = (*p)->next;
    else p = &(*p)->next;
  return result(close(fd));
}
long bober_io_read(int fd, void *buf, unsigned long n) { return result(read(fd, buf, n)); }
long bober_io_write(int fd, const void *buf, unsigned long n) { return result(write(fd, buf, n)); }
long bober_io_sync(int fd) { return result(fsync(fd)); }
long bober_io_mkdir(const char *path) { return result(mkdir(path, 0777)); }
long bober_io_rename(const char *from, const char *to) { return result(rename(from, to)); }

long bober_io_readlink(const char *path, void *buf, unsigned long n) { return result(readlink(path, buf, n)); }

long bober_io_remove(const char *path) {
  struct stat s;
  return result(lstat(path, &s) ? -1 : S_ISDIR(s.st_mode) ? rmdir(path) : unlink(path));
}

static const char *kind(unsigned mode) {
  return S_ISREG(mode) ? "file" : S_ISDIR(mode) ? "directory" : S_ISLNK(mode) ? "symlink" : "other";
}

static long ns(long s, long n) { return s * 1000000000 + n; }

bober_stat bober_io_stat(const char *path, int follow) {
#if defined(__linux__)  // only statx tells when a file was born
  struct statx x;
  if (statx(AT_FDCWD, path, follow ? 0 : AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS | STATX_BTIME, &x)) return (bober_stat){-errno};
  return (bober_stat){0, x.stx_size, ns(x.stx_mtime.tv_sec, x.stx_mtime.tv_nsec), ns(x.stx_atime.tv_sec, x.stx_atime.tv_nsec),
                     x.stx_mask & STATX_BTIME ? ns(x.stx_btime.tv_sec, x.stx_btime.tv_nsec) : 0, kind(x.stx_mode),
                     x.stx_mode & 07777};
#else
#if defined(__APPLE__)
#define TIME(x) s.st_##x##timespec.tv_sec, s.st_##x##timespec.tv_nsec
#else
#define TIME(x) s.st_##x##tim.tv_sec, s.st_##x##tim.tv_nsec
#endif
  struct stat s;
  if ((follow ? stat : lstat)(path, &s)) return (bober_stat){-errno};
  return (bober_stat){0, s.st_size, ns(TIME(m)), ns(TIME(a)), ns(TIME(birth)), kind(s.st_mode), s.st_mode & 07777};
#endif
}

long bober_io_list(const char *path, char *buf, unsigned long n) {
  DIR *d = opendir(path);
  if (!d) return -errno;
  unsigned long at = 0;
  for (struct dirent *e; (e = readdir(d));) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    const char *k = e->d_type == DT_REG ? "file" : e->d_type == DT_DIR ? "directory" : e->d_type == DT_LNK ? "symlink" : "other";
    size_t a = strlen(e->d_name) + 1, b = strlen(k) + 1;
    if (at + a + b <= n) memcpy(buf + at, e->d_name, a), memcpy(buf + at + a, k, b);
    at += a + b;
  }
  closedir(d);
  return at;
}

/// The sockaddr of host, an IP or a Unix socket's path, and port, into sa: its length, 0 if host is neither.
static socklen_t address(const char *host, int port, struct sockaddr_storage *sa) {
  struct sockaddr_in *v4 = (void *)sa;
  struct sockaddr_in6 *v6 = (void *)sa;
  struct sockaddr_un *u = (void *)sa;
  memset(sa, 0, sizeof *sa);
  if (*host == '/')
    return strlen(host) < sizeof u->sun_path ? (u->sun_family = AF_UNIX, strcpy(u->sun_path, host), sizeof *u) : 0;
  if (inet_pton(AF_INET, host, &v4->sin_addr) == 1) return v4->sin_family = AF_INET, v4->sin_port = htons(port), sizeof *v4;
  if (inet_pton(AF_INET6, host, &v6->sin6_addr) == 1)
    return v6->sin6_family = AF_INET6, v6->sin6_port = htons(port), sizeof *v6;
  return 0;
}

/// The IP, or Unix socket's path, of sa into host, BOBER_IO_HOST bytes: its port.
static int text(const struct sockaddr_storage *sa, char *host) {
  const struct sockaddr_in *v4 = (const void *)sa;
  const struct sockaddr_in6 *v6 = (const void *)sa;
  const struct sockaddr_un *u = (const void *)sa;
  *host = 0;
  switch (sa->ss_family) {
  case AF_INET: return inet_ntop(AF_INET, &v4->sin_addr, host, BOBER_IO_HOST), ntohs(v4->sin_port);
  case AF_INET6:  // a v4 peer of a v6 socket is ::ffff:a.b.c.d
    if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) inet_ntop(AF_INET, &v6->sin6_addr.s6_addr[12], host, BOBER_IO_HOST);
    else inet_ntop(AF_INET6, &v6->sin6_addr, host, BOBER_IO_HOST);
    return ntohs(v6->sin6_port);
  case AF_UNIX: snprintf(host, BOBER_IO_HOST, "%.*s", (int)sizeof u->sun_path, u->sun_path); return 0;
  default: return 0;
  }
}

/// Makes fd non-blocking and CLOEXEC, a TCP one without Nagle, so that a small write after another is not held
/// ~40 ms for the peer's delayed ack; on any other, that fails harmlessly.
static long unblocked(long fd) {
  int on = 1;
  if (fd < 0) return -errno;
  fcntl(fd, F_SETFD, FD_CLOEXEC), fcntl(fd, F_SETFL, O_NONBLOCK);
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
  return fd;
}

/// Closes fd, giving the failure errno is.
static long failed(int fd) {
  long r = -errno;
  close(fd);
  return r;
}

long bober_io_dial(const char *net, const char *host, int port) {
  struct sockaddr_storage sa;
  socklen_t n = address(host, port, &sa);
  if (!n) return -EINVAL;
  long fd = unblocked(socket(sa.ss_family, strcmp(net, "udp") ? SOCK_STREAM : SOCK_DGRAM, 0));
  return fd >= 0 && connect(fd, (void *)&sa, n) && errno != EINPROGRESS ? failed(fd) : fd;
}

long bober_io_connected(int fd) {
  int err = 0;
  socklen_t n = sizeof err;
  return getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &n) ? -errno : -err;
}

long bober_io_listen(const char *net, const char *host, int port, int backlog) {
  struct sockaddr_storage sa;
  socklen_t n = address(host, port, &sa);
  if (!n) return -EINVAL;
  int stream = strcmp(net, "udp"), ip = sa.ss_family != AF_UNIX, on = 1, off = 0;
  long fd = unblocked(socket(sa.ss_family, stream ? SOCK_STREAM : SOCK_DGRAM, 0));
  // A v6 socket takes v4 too, whatever net.ipv6.bindv6only says. SO_REUSEADDR on UDP would let any user share the
  // port, so only a stream listener has it. SO_REUSEPORT lets the user's sockets share a port, so it is only for one
  // asked for: port 0 could get one another of them holds.
  if (fd >= 0 && ((ip && stream && setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on))
                  || (ip && port && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on))
                  || (sa.ss_family == AF_INET6 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off))
                  || bind(fd, (void *)&sa, n) || (stream && listen(fd, backlog))))
    return failed(fd);
  return fd;
}

long bober_io_accept(int fd) {
  long c = accept(fd, 0, 0);
  if (c >= 0) return unblocked(c);
  // An abort or network error is the taken connection's, not the listener's, so accept(2) retries it as EAGAIN.
  switch (errno) {
  case ECONNABORTED: case ENETDOWN: case EPROTO: case ENOPROTOOPT: case EHOSTDOWN: case ENONET: case EHOSTUNREACH:
  case EOPNOTSUPP: case ENETUNREACH: return BOBER_IO_AGAIN;
  }
  return result(c);
}

long bober_io_recvfrom(int fd, void *buf, unsigned long n, char *host, int *port) {
  struct sockaddr_storage sa = {0};
  socklen_t len = sizeof sa;
  long r = recvfrom(fd, buf, n, 0, (void *)&sa, &len);
  if (r >= 0) *port = text(&sa, host);
  return result(r);
}

long bober_io_sendto(int fd, const void *buf, unsigned long n, const char *host, int port) {
  struct sockaddr_storage sa;
  socklen_t len = address(host, port, &sa);
  return len ? result(sendto(fd, buf, n, 0, (void *)&sa, len)) : -EINVAL;
}

long bober_io_shutdown(int fd, int both) { return result(shutdown(fd, both ? SHUT_RDWR : SHUT_WR)); }

int bober_io_idle(int fd) {
  char c;
  return recv(fd, &c, 1, MSG_PEEK) < 0 && errno == EWOULDBLOCK;
}

long bober_io_name(int fd, int peer, char *host) {
  struct sockaddr_storage sa = {0};
  socklen_t n = sizeof sa;
  return (peer ? getpeername : getsockname)(fd, (void *)&sa, &n) ? -errno : text(&sa, host);
}

int bober_io_ip(const char *text, unsigned char *ip) {
  return inet_pton(AF_INET, text, ip) == 1 ? 4 : inet_pton(AF_INET6, text, ip) == 1 ? 16 : 0;
}

void bober_io_ip_text(const unsigned char *ip, int n, char *text) {
  inet_ntop(n == 4 ? AF_INET : AF_INET6, ip, text, BOBER_IO_HOST);
}

long bober_io_run(const char *file, char *const *argv, const char *out) {
  extern char **environ;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  if (out)
    posix_spawn_file_actions_addopen(&fa, 1, out, O_WRONLY | O_CREAT | O_TRUNC, 0666),
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
  pid_t pid;
  int err = posix_spawnp(&pid, file, &fa, 0, argv, environ), status;
  posix_spawn_file_actions_destroy(&fa);
  if (err) return -err;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR) return -errno;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
