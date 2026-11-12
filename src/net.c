//! src/net.h on lwIP (vendor/lwip): no OS, its loopback alone, its time bober_mono(). A socket is a descriptor of
//! /dev/null, a number no other descriptor has, which indexes the Sock that holds its pcb.

#define WEAK  // net.h's functions, defined here
#include "net.h"
#include "rt.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "lwip/init.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"

/// What a socket received and is yet to read, in order: a TCP stream's pbufs, a UDP socket's datagrams with whom they
/// came from, or a listener's accepted Socks.
typedef struct Item { void *p; ip_addr_t ip; u16_t port; struct Item *next; } Item;
/// Its pcb, none once it failed, err the errno why, or was closed or shut down both ways (shut 2), or only to write
/// (shut 1); the ways it became ready since the last net_poll, which wakes what waits for that, once no fiber is
/// ready, as a kernel's readiness is found.
typedef struct { void *pcb; int fd, udp, listening, connected, fin, shut, err, edges; Item *in, **end; } Sock;

static Sock **socks;
static long nsocks;

u32_t sys_now(void) { return bober_mono() / 1000000; }

static Sock *sock(int fd) { return (unsigned)fd < nsocks ? socks[fd] : 0; }

static void push(Sock *s, void *p, const ip_addr_t *ip, u16_t port) {
  Item *i = calloc(1, sizeof *i);
  *i = (Item){p, *ip, port};
  *s->end = i, s->end = &i->next;
}

static void pop(Sock *s) {
  Item *i = s->in;
  if (!(s->in = i->next)) s->end = &s->in;
  free(i);
}

static Sock *made(void *pcb, int udp) {
  Sock *s = calloc(1, sizeof *s);
  *s = (Sock){pcb, -1, udp};
  return s->end = &s->in, s;
}

/// Gives s its descriptor: it, or the failure, as when out of them.
static long held(Sock *s) {
  int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -errno;
  if (fd >= nsocks) {
    long n = 2 * fd + 64;
    socks = realloc(socks, n * sizeof *socks), memset(socks + nsocks, 0, (n - nsocks) * sizeof *socks), nsocks = n;
  }
  return socks[s->fd = fd] = s, fd;
}

/// Lets go of s's pcb: a connection is reset if reset, never accepted, or with unread input, as a kernel's.
static void release(Sock *s, int reset) {
  reset |= s->fd < 0 || s->in && !s->listening && !s->udp;
  for (; s->in; pop(s))
    if (s->listening) release(s->in->p, 0), free(s->in->p);
    else pbuf_free(s->in->p);
  if (!s->pcb) return;
  if (s->udp) udp_remove(s->pcb);
  else if (s->listening) tcp_close(s->pcb);
  else {
    tcp_arg(s->pcb, 0), tcp_recv(s->pcb, 0), tcp_sent(s->pcb, 0), tcp_err(s->pcb, 0);
    if (reset || tcp_close(s->pcb) != ERR_OK) tcp_abort(s->pcb);
  }
  s->pcb = 0;
}

static long failure(err_t e) {
  switch (e) {
  case ERR_MEM: case ERR_BUF: return -ENOMEM;
  case ERR_USE: return -EADDRINUSE;
  case ERR_RST: return -ECONNRESET;
  case ERR_ABRT: return -ECONNABORTED;
  case ERR_CONN: return -ENOTCONN;
  case ERR_CLSD: return -EPIPE;
  default: return -EINVAL;
  }
}

static err_t received(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t e) {
  Sock *s = arg;
  if (!p) s->fin = 1;
  else push(s, p, IP4_ADDR_ANY, 0);
  return s->edges |= 1, ERR_OK;
}

static err_t sent(void *arg, struct tcp_pcb *pcb, u16_t n) { return ((Sock *)arg)->edges |= 2, ERR_OK; }

static void failed(void *arg, err_t e) {
  Sock *s = arg;
  s->pcb = 0, s->err = e == ERR_RST && !s->connected ? ECONNREFUSED : -failure(e);
  s->edges = 3;
}

static void serve(Sock *s) {
  tcp_arg(s->pcb, s), tcp_recv(s->pcb, received), tcp_sent(s->pcb, sent), tcp_err(s->pcb, failed);
  tcp_nagle_disable((struct tcp_pcb *)s->pcb);  // as src/io.c's unblocked
}

static err_t connected(void *arg, struct tcp_pcb *pcb, err_t e) {
  Sock *s = arg;
  return s->connected = 1, s->edges |= 2, ERR_OK;
}

/// Queues the connection; it is given its descriptor once accepted, which tells if they ran out.
static err_t accepted(void *arg, struct tcp_pcb *pcb, err_t e) {
  if (!pcb) return ERR_MEM;
  Sock *l = arg, *s = made(pcb, 0);
  s->connected = 1, serve(s);
  return push(l, s, IP4_ADDR_ANY, 0), l->edges |= 1, ERR_OK;
}

static void datagram(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *ip, u16_t port) {
  push(arg, p, ip, port), ((Sock *)arg)->edges |= 1;
}

static err_t looped(struct netif *n, struct pbuf *p, const ip4_addr_t *to) { return netif_loop_output(n, p); }

static err_t loopback(struct netif *n) {
  return n->name[0] = 'l', n->name[1] = 'o', n->output = looped, ERR_OK;
}

/// Makes ip, if in 127/8, the loopback's, as a kernel's are all: lwIP's has 127.0.0.1 alone, and a netif of its own
/// for each other.
static void own(const ip_addr_t *ip) {
  struct netif *n;
  if (!IP_IS_V4(ip) || !ip4_addr_isloopback(ip_2_ip4(ip))) return;
  NETIF_FOREACH(n) if (ip4_addr_eq(netif_ip4_addr(n), ip_2_ip4(ip))) return;
  ip4_addr_t one = {PP_HTONL(0xffffffff)};
  netif_add(n = calloc(1, sizeof *n), ip_2_ip4(ip), &one, IP4_ADDR_ANY4, 0, loopback, ip_input);
  netif_set_link_up(n), netif_set_up(n);
}

/// The address of host, an IP, into ip: 0, or the failure. One sent to must be the loopback's, all the network has;
/// the unspecified one is it, as a kernel's. Else :: is any of both.
static long address(const char *host, ip_addr_t *ip, int to) {
  if (!netif_list) lwip_init();  // the network starts at its first socket
  if (!ipaddr_aton(host, ip)) return -EINVAL;
  if (to && ip_addr_isany(ip)) ip_addr_set_loopback(IP_IS_V6(ip), ip);
  else if (IP_IS_V6(ip) && ip_addr_isany(ip)) *ip = *IP_ANY_TYPE;
  if (to && !ip_addr_isloopback(ip)) return -ENETUNREACH;
  return own(ip), 0;
}

/// Delivers what was sent over the loopback, at once, as a kernel's. TIME_WAIT, which waits out a lost segment's
/// copy, would only hold ports, as the loopback loses none: a connection in it, both ways ended, lets its pcb go.
static long flushed(long r) {
  for (struct netif *n = netif_list; n;) n->loop_first ? netif_poll(n), n = netif_list : (n = n->next);
  for (struct tcp_pcb *p; (p = tcp_tw_pcbs); tcp_abort(p))
    if (p->callback_arg) ((Sock *)p->callback_arg)->pcb = 0;
  return r;
}

long net_dial(const char *net, const char *host, int port) {
  ip_addr_t ip;
  long r = address(host, &ip, 1);
  if (r) return r;
  int udp = !strcmp(net, "udp");
  void *pcb = udp ? (void *)udp_new_ip_type(IP_GET_TYPE(&ip)) : tcp_new_ip_type(IP_GET_TYPE(&ip));
  if (!pcb) return -ENOMEM;
  Sock *s = made(pcb, udp);
  long fd = held(s);
  if (fd < 0) return release(s, 0), free(s), fd;
  if (udp) udp_recv(pcb, datagram, s);
  else serve(s);
  err_t e = udp ? udp_connect(pcb, &ip, port) : tcp_connect(pcb, &ip, port, connected);
  return e == ERR_OK ? fd : (net_close(fd, 0), failure(e));  // connected once its fiber waits, as a kernel's
}

long net_connected(int fd) { return -sock(fd)->err; }

long net_listen(const char *net, const char *host, int port) {
  ip_addr_t ip;
  long r = address(host, &ip, 0);
  if (r) return r;
  int udp = !strcmp(net, "udp");
  void *pcb = udp ? (void *)udp_new_ip_type(IP_GET_TYPE(&ip)) : tcp_new_ip_type(IP_GET_TYPE(&ip));
  if (!pcb) return -ENOMEM;
  Sock *s = made(pcb, udp);
  long fd = held(s);
  if (fd < 0) return release(s, 0), free(s), fd;
  if (!udp) ip_set_option((struct tcp_pcb *)pcb, SOF_REUSEADDR);  // as src/io.c's
  err_t e = udp ? udp_bind(pcb, &ip, port) : tcp_bind(pcb, &ip, port);
  if (e != ERR_OK) return net_close(s->fd, 0), failure(e);
  if (udp) udp_recv(pcb, datagram, s);
  // A listener on a port another listens on, which a kernel's SO_REUSEPORT would share, is in use: lwIP has one.
  else if (!(pcb = tcp_listen_with_backlog_and_err(pcb, TCP_DEFAULT_LISTEN_BACKLOG, &e)))
    return net_close(s->fd, 0), failure(e);
  else s->pcb = pcb, s->listening = 1, tcp_arg(pcb, s), tcp_accept(pcb, accepted);
  return s->fd;
}

long net_accept(int fd) {
  Sock *l = sock(fd), *s = l->in ? l->in->p : 0;
  if (!s) return l->pcb ? BOBER_IO_AGAIN : -EINVAL;
  long c = held(s);
  if (c >= 0) pop(l);
  return c;
}

long net_read(int fd, void *buf, unsigned long n) { return net_recvfrom(fd, buf, n, 0, 0); }

long net_recvfrom(int fd, void *buf, unsigned long n, char *host, int *port) {
  Sock *s = sock(fd);
  if (s->udp) {  // a datagram, cut to n
    if (!s->in) return s->pcb ? BOBER_IO_AGAIN : -s->err;
    struct pbuf *p = s->in->p;
    long k = pbuf_copy_partial(p, buf, n < p->tot_len ? n : p->tot_len, 0);
    if (host) ipaddr_ntoa_r(&s->in->ip, host, BOBER_IO_HOST), *port = s->in->port;
    return pbuf_free(p), pop(s), k;
  }
  unsigned long at = 0;
  while (s->in && at < n) {  // a pbuf's length is 16 bits, so each received holds its own
    struct pbuf *p = s->in->p;
    u16_t k = n - at < p->tot_len ? n - at : p->tot_len;
    pbuf_copy_partial(p, (char *)buf + at, k, 0), at += k;
    if (!(s->in->p = pbuf_free_header(p, k))) pop(s);
    if (s->pcb) tcp_recved(s->pcb, k);
  }
  return flushed(0), at || s->fin || s->shut == 2 ? at : s->pcb ? BOBER_IO_AGAIN : -s->err;
}

long net_write(int fd, const void *buf, unsigned long n) {
  Sock *s = sock(fd);
  if (s->udp) return net_sendto(fd, buf, n, 0, 0);
  if (s->shut || !s->pcb) return s->shut || !s->err ? -EPIPE : -s->err;
  struct tcp_pcb *pcb = s->pcb;
  unsigned long k = n < tcp_sndbuf(pcb) ? n : tcp_sndbuf(pcb);
  if (!s->connected || !k || tcp_write(pcb, buf, k, TCP_WRITE_FLAG_COPY) != ERR_OK) return BOBER_IO_AGAIN;
  return tcp_output(pcb), flushed(k);
}

long net_sendto(int fd, const void *buf, unsigned long n, const char *host, int port) {
  Sock *s = sock(fd);
  struct udp_pcb *pcb = s->pcb;
  ip_addr_t ip;
  long r;
  if (!pcb) return -EPIPE;
  if (host && (r = address(host, &ip, 1))) return r;
  if (host && !IP_ADDR_PCB_VERSION_MATCH(pcb, &ip)) return -EAFNOSUPPORT;
  if (n > 0xffff - 8) return -EMSGSIZE;
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM);
  if (!p) return -ENOMEM;
  pbuf_take(p, buf, n);
  err_t e = host ? udp_sendto(pcb, p, &ip, port) : udp_send(pcb, p);
  return pbuf_free(p), flushed(e == ERR_OK ? (long)n : failure(e));
}

long net_shutdown(int fd, int both) {
  Sock *s = sock(fd);
  if (both || s->listening || s->udp) release(s, 0), s->shut = 2;
  else if (!s->pcb) return -ENOTCONN;
  else if (!s->shut) s->shut = 1, s->edges |= 2, tcp_shutdown(s->pcb, 0, 1);
  return flushed(0);
}

int net_idle(int fd) {
  Sock *s = sock(fd);
  return !s->in && !s->fin && s->pcb;
}

long net_name(int fd, int peer, char *host) {
  Sock *s = sock(fd);
  if (!s || !s->pcb) return -ENOTCONN;
  struct tcp_pcb *t = s->pcb;
  struct udp_pcb *u = s->pcb;
  const ip_addr_t *ip = s->udp ? (peer ? &u->remote_ip : &u->local_ip) : peer ? &t->remote_ip : &t->local_ip;
  if (IP_IS_ANY_TYPE_VAL(*ip)) strcpy(host, "::");
  else ipaddr_ntoa_r(ip, host, BOBER_IO_HOST);
  return s->udp ? (peer ? u->remote_port : u->local_port) : peer ? t->remote_port : t->local_port;
}

long net_close(int fd, int reset) {
  Sock *s = sock(fd);
  release(s, reset), free(s), socks[fd] = 0;
  return flushed(close(fd) ? -errno : 0);
}

int net_ready(int fd, int write) {
  Sock *s = sock(fd);
  if (!s) return -1;
  struct tcp_pcb *t = s->pcb;
  if (!write || !t || s->shut || s->udp) return !write ? s->in || s->fin || !t : 1;
  return s->connected && tcp_sndbuf(t) && tcp_sndqueuelen(t) < TCP_SND_QUEUELEN;
}

long net_poll(void) {
  if (!netif_list) return 0;
  sys_check_timeouts(), flushed(0);
  for (long fd = 0; fd < nsocks; fd++)
    for (int w = 0; socks[fd] && w < 2; w++)
      if (socks[fd]->edges & 1 << w) socks[fd]->edges &= ~(1 << w), net_edge(fd, w);
  // Only a delayed ack moves logical time: the loopback loses nothing to send again, and a read opens a closed window
  // at once (src/net/lwipopts.h), so no other timer does what a fiber waits for.
  for (struct tcp_pcb *p = tcp_active_pcbs; p; p = p->next)
    if (p->flags & TF_ACK_DELAY) return bober_mono() + sys_timeouts_sleeptime() * 1000000L;
  return 0;
}
