//! Mbed TLS on a socket of std/net's, for std/tls: a context of its config, session and certificates, whose BIO
//! callbacks wait through the run time's I/O (src/rt.h), parking the fiber inside C. A function that fails returns
//! Mbed TLS's code; a BIO that fails makes it MBEDTLS_ERR_NET_RECV_FAILED or MBEDTLS_ERR_NET_SEND_FAILED, the errno
//! kept for bober_tls_errno.

#include "../../src/rt.h"
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  mbedtls_ssl_config conf;
  mbedtls_ssl_context ssl;
  mbedtls_x509_crt own;
  mbedtls_pk_context key;
  // sending while a send is parked in the BIO; broken, the errno of a send that failed, as Mbed TLS would resend the
  // record it left before anything else, so every send after fails the same, as Go's; notified once close_notify is
  // sent, ended once the peer's is read or bober_tls_close has cut the reads
  int fd, err, nalpn, sending, broken, notified, ended;
  long timeout;
  size_t nip;
  unsigned char ip[16];  // a client's host if an IP, its nip bytes, checked by bober_tls_ip
  const char *alpn[9];  // null-ended
} bober_tls;

/// A read into, or if write a write of, the n bytes of buf on t's socket, parking until it is ready if it would block.
static long bober_tls_io(bober_tls *t, int write, void *buf, size_t n) {
  for (long r;;) {
    if ((r = write ? bober_io_write(t->fd, buf, n) : bober_io_read(t->fd, buf, n)) != BOBER_IO_AGAIN) return r;
    if ((r = bober_io_ready(t->fd, write, t->timeout))) return r;
  }
}

static int bober_tls_send(void *ctx, const unsigned char *buf, size_t n) {
  bober_tls *t = ctx;
  // A second send while one is parked, as a read's alert during a write, would flush the parked record's bytes again.
  if (t->sending) return t->err = EPROTO, MBEDTLS_ERR_NET_SEND_FAILED;
  if (t->broken) return t->err = t->broken, MBEDTLS_ERR_NET_SEND_FAILED;
  t->sending = 1;
  long r = bober_tls_io(t, 1, (void *)buf, n);
  t->sending = 0;
  return r < 0 ? (t->err = t->broken = -r, MBEDTLS_ERR_NET_SEND_FAILED) : r;
}

static int bober_tls_recv(void *ctx, unsigned char *buf, size_t n) {
  bober_tls *t = ctx;
  long r = bober_tls_io(t, 0, buf, n);
  return r < 0 ? (t->err = -r, MBEDTLS_ERR_NET_RECV_FAILED) : r;
}

/// A context on the socket fd, a server's if server, or null if out of memory.
static void *bober_tls_new(int fd, int server) {
  bober_tls *t = calloc(1, sizeof *t);
  if (!t) return 0;
  psa_crypto_init();
  mbedtls_ssl_config_init(&t->conf), mbedtls_ssl_init(&t->ssl), mbedtls_x509_crt_init(&t->own), mbedtls_pk_init(&t->key);
  t->fd = fd;
  if (mbedtls_ssl_config_defaults(&t->conf, server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                  MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) return free(t), (void *)0;
  mbedtls_ssl_set_bio(&t->ssl, t, bober_tls_send, bober_tls_recv, 0);
  return t;
}

/// A chain parsed from the n bytes of PEM at pem, its NUL included, into *out; never freed.
static int bober_tls_chain(const unsigned char *pem, size_t n, void **out) {
  mbedtls_x509_crt *c = calloc(1, sizeof *c);
  if (!c) return MBEDTLS_ERR_SSL_ALLOC_FAILED;
  mbedtls_x509_crt_init(c);
  int r = mbedtls_x509_crt_parse(c, pem, n);
  if (r < 0) return mbedtls_x509_crt_free(c), free(c), r;
  return *out = c, 0;
}

/// Checks the peer's certificate against chain, or not at all if chain is null.
static void bober_tls_trust(void *ctx, void *chain) {
  bober_tls *t = ctx;
  mbedtls_ssl_conf_authmode(&t->conf, chain ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
  if (chain) mbedtls_ssl_conf_ca_chain(&t->conf, chain, 0);
}

/// The context's own certificate chain and key, each as PEM with its NUL.
static int bober_tls_own(void *ctx, const unsigned char *cert, size_t n, const unsigned char *key, size_t m) {
  bober_tls *t = ctx;
  int r = mbedtls_x509_crt_parse(&t->own, cert, n);
  if (!r) r = mbedtls_pk_parse_key(&t->key, key, m, 0, 0);
  return r ? r : mbedtls_ssl_conf_own_cert(&t->conf, &t->own, &t->key);
}

/// Adds proto to the protocols offered, or accepted, in order; at most 8.
static int bober_tls_alpn(void *ctx, const char *proto) {
  bober_tls *t = ctx;
  if (t->nalpn == 8) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
  t->alpn[t->nalpn++] = strdup(proto);
  return mbedtls_ssl_conf_alpn_protocols(&t->conf, t->alpn);
}

/// Flags the peer's own certificate if no IP address of its names is t's.
static int bober_tls_ip(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
  bober_tls *t = ctx;
  if (depth) return 0;
  for (const mbedtls_x509_sequence *s = &crt->subject_alt_names; s; s = s->next)
    if ((s->buf.tag & MBEDTLS_ASN1_TAG_VALUE_MASK) == MBEDTLS_X509_SAN_IP_ADDRESS && s->buf.len == t->nip &&
        !memcmp(s->buf.p, t->ip, t->nip)) return 0;
  return *flags |= MBEDTLS_X509_BADCERT_CN_MISMATCH, 0;
}

/// Shakes hands, a client's with host, checked against the chain and sent as SNI unless an IP; a server's with null.
static int bober_tls_handshake(void *ctx, const char *host, long timeout) {
  bober_tls *t = ctx;
  t->timeout = timeout;
  int r = mbedtls_ssl_setup(&t->ssl, &t->conf);
  if (!r && host) {
    // SNI must not carry an IP, so bober_tls_ip checks one, and the null hostname skips Mbed TLS's check of names.
    if ((t->nip = mbedtls_x509_crt_parse_cn_inet_pton(host, t->ip))) mbedtls_ssl_set_verify(&t->ssl, bober_tls_ip, t);
    r = mbedtls_ssl_set_hostname(&t->ssl, t->nip ? 0 : host);
  }
  while (!r && (r = mbedtls_ssl_handshake(&t->ssl)) == MBEDTLS_ERR_SSL_WANT_READ) {}
  return r;
}

/// Up to n bytes of what the peer sent into buf, which is outside the collector's heap; 0 once the peer has closed or
/// bober_tls_close has cut the reads, and from then on. A socket ended without close_notify is ECONNRESET: the peer may
/// have been cut off.
static int bober_tls_read(void *ctx, void *buf, size_t n, long timeout) {
  bober_tls *t = ctx;
  t->timeout = timeout;
  while (!t->ended) {
    int r = mbedtls_ssl_read(&t->ssl, buf, n);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) t->ended = 1;
    else if (r || t->ended) return r;
    else return t->err = ECONNRESET, MBEDTLS_ERR_NET_RECV_FAILED;
  }
  return 0;
}

/// Sends all n bytes at buf: n, or a code.
static int bober_tls_write(void *ctx, const void *buf, size_t n, long timeout) {
  bober_tls *t = ctx;
  t->timeout = timeout;
  // buf may be in the collector's heap, which moves while the BIO parks, so a copy, as src/io.c makes for a write.
  unsigned char *copy = malloc(n + 1);
  memcpy(copy, buf, n);
  int r = 0;
  for (size_t at = 0; at < n;) {
    r = mbedtls_ssl_write(&t->ssl, copy + at, n - at);
    if (r >= 0) at += r;
    else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) break;
  }
  free(copy);
  return r < 0 ? r : (int)n;
}

/// Tells the peer the connection is closing, once.
static int bober_tls_shutdown(void *ctx, long timeout) {
  bober_tls *t = ctx;
  if (t->notified) return 0;
  t->notified = 1, t->timeout = timeout;
  int r;
  while ((r = mbedtls_ssl_close_notify(&t->ssl)) == MBEDTLS_ERR_SSL_WANT_WRITE) {}
  return r;
}

/// Tells the peer if it can, so that it reads an end rather than a cut, then ends the reads and writes in flight.
static void bober_tls_close(void *ctx, long timeout) {
  bober_tls *t = ctx;
  t->ended = 1;
  bober_tls_shutdown(t, timeout);
  bober_io_shutdown(t->fd, 1);
}

/// The protocol agreed on, or null.
static const char *bober_tls_alpn_of(void *ctx) { return mbedtls_ssl_get_alpn_protocol(&((bober_tls *)ctx)->ssl); }

/// The errno of the last BIO failure.
static int bober_tls_errno(void *ctx) { return ((bober_tls *)ctx)->err; }

static void bober_tls_free(void *ctx) {
  bober_tls *t = ctx;
  mbedtls_ssl_free(&t->ssl), mbedtls_ssl_config_free(&t->conf), mbedtls_x509_crt_free(&t->own), mbedtls_pk_free(&t->key);
  for (int i = 0; i < t->nalpn; i++) free((char *)t->alpn[i]);
  free(t);
}

/// What Mbed TLS's code means, until the next call.
static const char *bober_tls_error(int code) {
  static char buf[128];
  mbedtls_strerror(code, buf, sizeof buf);
  return buf;
}
