//! The I/O of src/rt.h, through the libc of the target: the subset of POSIX that Linux, macOS and FreeBSD share, and
//! epoll or kqueue. Only sockets wait: they are non-blocking, a call on one that would block gives BOBER_IO_AGAIN, and
//! bober_io_ready parks its fiber until the kernel finds the socket ready; once no fiber is ready, bober_park asks it
//! (bober_io_wait). The kernel watches a descriptor from its first wait until its close, edge-triggered both ways, so a
//! wait costs no system call: an edge marks it ready that way until a call on it would block, and a wait that finds it
//! marked has the kernel look again, as the edge may be stale. Without src/fiber.c, as in boot, the weak bober_park below
//! asks in place.

#define _GNU_SOURCE  // statx
#include "rt.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

/// A fiber parked until fd is ready, on its stack: done once it is, -1 if its deadline passed first; at its index in
/// the heap of deadlines, if it has one.
typedef struct Wait { uint64_t fiber; int fd, write, done; long deadline, at; struct Wait *next; } Wait;
/// A descriptor's, by way, read then write: who waits, in order, and whether it was ready since a call would block;
/// whether the kernel watches it.
typedef struct { Wait *waits[2]; char ready[2], watched; } Fd;
static Fd *fds;
static Wait **heap;  // the soonest deadline first
static long nfds, deadlines, room, waiting;

static void edge(int fd, int write);

/// Has the kernel watch fd (ADD), look at it again (MOD), or forget it (DEL): 0, or -1 if it cannot, as epoll cannot a file.
enum { ADD, MOD, DEL };
static int q = -1;  // the kernel's queue
#if defined(__linux__)
#include <sys/epoll.h>
static int queue(void) { return epoll_create1(EPOLL_CLOEXEC); }
static int watch(int fd, int how) {
  struct epoll_event e = {EPOLLIN | EPOLLOUT | EPOLLET, {.fd = fd}};
  return epoll_ctl(q, how == ADD ? EPOLL_CTL_ADD : how == MOD ? EPOLL_CTL_MOD : EPOLL_CTL_DEL, fd, &e);
}

/// Gives edge what the kernel finds ready within ms, -1 for no limit.
static void events(long ms) {
  struct epoll_event e[256];
  int n = epoll_wait(q, e, 256, ms);
  if (n < 0 && errno != EINTR) perror("epoll_wait"), exit(1);
  for (int i = 0; i < n; i++) {
    if (e[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) edge(e[i].data.fd, 0);
    if (e[i].events & (EPOLLOUT | EPOLLHUP | EPOLLERR)) edge(e[i].data.fd, 1);
  }
}
#else
#include <sys/event.h>
static int queue(void) { return kqueue(); }
static int watch(int fd, int how) {  // close forgets it; adding again looks again
  struct kevent k[2];
  EV_SET(&k[0], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, 0);
  EV_SET(&k[1], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, 0);
  return how == DEL ? 0 : kevent(q, k, 2, 0, 0, 0);
}

static void events(long ms) {
  struct kevent e[256];
  struct timespec t = {ms / 1000, ms % 1000 * 1000000};
  int n = kevent(q, 0, 0, e, 256, ms < 0 ? 0 : &t);
  if (n < 0 && errno != EINTR) perror("kevent"), exit(1);
  for (int i = 0; i < n; i++) edge(e[i].ident, e[i].filter == EVFILT_WRITE);
}
#endif

/// p, an array of *n items of size bytes, grown to hold item i, zero.
static void *fit(void *p, long *n, long i, size_t size) {
  long m = i < 32 ? 64 : 2 * i;
  if (i < *n) return p;
  if (!(p = realloc(p, m * size))) fputs("out of memory\n", stderr), exit(1);
  return memset((char *)p + *n * size, 0, (m - *n) * size), *n = m, p;
}

/// Puts w at i in the heap and moves it up or down to where its deadline belongs.
static void sift(Wait *w, long i) {
  for (long up; i && heap[up = (i - 1) / 2]->deadline > w->deadline; i = up) (heap[i] = heap[up])->at = i;
  for (long c; (c = 2 * i + 1) < deadlines; i = c) {
    if (c + 1 < deadlines && heap[c + 1]->deadline < heap[c]->deadline) c++;
    if (heap[c]->deadline >= w->deadline) break;
    (heap[i] = heap[c])->at = i;
  }
  (heap[i] = w)->at = i;
}

/// Wakes w, done, taking it from its descriptor's waits and the heap.
static void wake(Wait *w, int done) {
  Wait **p = &fds[w->fd].waits[w->write];
  while (*p != w) p = &(*p)->next;
  *p = w->next;
  if (w->deadline && w->at < --deadlines) sift(heap[deadlines], w->at);
  w->done = done, waiting--, bober_wake(w->fiber);
}

/// fd has been ready to read or write: what waits for that wakes.
static void edge(int fd, int write) {
  Fd *f = &fds[fd];
  for (f->ready[write] = 1; f->waits[write];) wake(f->waits[write], 1);
}

/// What a call that gave r gives: r, or the failure errno is, BOBER_IO_AGAIN for one that would block.
static long result(long r) {
  return r >= 0 ? r : errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? BOBER_IO_AGAIN : -errno;
}

/// What a call on fd to read or write that gave r gives, as result; one that would block ends fd's readiness that way.
static long result_on(int fd, int write, long r) {
  if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && (unsigned)fd < nfds) fds[fd].ready[write] = 0;
  return result(r);
}

long bober_io_ready(int fd, int write, long timeout) {
  if (fd < 0) return 0;
  Fd *f = (fds = fit(fds, &nfds, fd, sizeof *fds)) + fd;
  // One the kernel cannot watch, as a file or one closed, is always ready: the call on it tells.
  if (!f->watched || f->ready[write]) {
    if (q < 0 && (q = queue()) < 0) return -errno;
    if (watch(fd, f->watched ? MOD : ADD)) return errno == EPERM || errno == EBADF ? 0 : -errno;
    f->watched = 1, f->ready[write] = 0;
  }
  long t = bober_mono();  // a deadline past the largest long saturates to it, which never comes
  Wait w = {bober_self(), fd, write, 0, !timeout ? 0 : timeout > LONG_MAX - t ? LONG_MAX : t + timeout}, **p;
  for (p = &f->waits[write]; *p;) p = &(*p)->next;
  *p = &w, waiting++;
  if (w.deadline) heap = fit(heap, &room, deadlines, sizeof *heap), sift(&w, deadlines++);
  while (!w.done) bober_park();
  return w.done < 0 ? -ETIMEDOUT : 0;
}

int bober_io_wait(void) {
  if (!waiting) return 0;
  long t = bober_mono(), soonest = deadlines ? heap[0]->deadline : 0;
  // In ms rounded up, at most an int's; a later deadline is waited for again.
  long ms = !soonest ? -1 : soonest <= t ? 0 : (soonest - t - 1) / 1000000 + 1;
  events(ms > INT_MAX ? INT_MAX : ms);
  for (t = bober_mono(); deadlines && heap[0]->deadline <= t;) wake(heap[0], -1);
  return 1;
}

void bober_io_cancel(void) {
  for (long i = 0; i < nfds; i++) fds[i].waits[0] = fds[i].waits[1] = 0;
  deadlines = waiting = 0;
}

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

/// Wakes what waits on fd, ended or closed, at once: before the peer's answer to it, and another descriptor's number.
static void ended(int fd) {
  for (int w = 0; w < 2 && (unsigned)fd < nfds; w++)
    while (fds[fd].waits[w]) wake(fds[fd].waits[w], 1);
}

long bober_io_close(int fd) {
  ended(fd);
  // The kernel forgets fd, which it would not for a dup's sake.
  if ((unsigned)fd < nfds && fds[fd].watched) watch(fd, DEL), fds[fd] = (Fd){0};
  return result(close(fd));
}
long bober_io_read(int fd, void *buf, unsigned long n) { return result_on(fd, 0, read(fd, buf, n)); }
long bober_io_write(int fd, const void *buf, unsigned long n) { return result_on(fd, 1, write(fd, buf, n)); }
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
  return result_on(fd, 0, c);
}

long bober_io_recvfrom(int fd, void *buf, unsigned long n, char *host, int *port) {
  struct sockaddr_storage sa = {0};
  socklen_t len = sizeof sa;
  long r = recvfrom(fd, buf, n, 0, (void *)&sa, &len);
  if (r >= 0) *port = text(&sa, host);
  return result_on(fd, 0, r);
}

long bober_io_sendto(int fd, const void *buf, unsigned long n, const char *host, int port) {
  struct sockaddr_storage sa;
  socklen_t len = address(host, port, &sa);
  return len ? result_on(fd, 1, sendto(fd, buf, n, 0, (void *)&sa, len)) : -EINVAL;
}

long bober_io_shutdown(int fd, int both) {
  long r = result(shutdown(fd, both ? SHUT_RDWR : SHUT_WR));
  if (both) ended(fd);
  return r;
}

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
