# std/http

Why the stack beneath std/http is shaped as it is. What each module does and its API are in its `//!` header and
signatures.

## What it skips

The shape is fetch's and Hono's, not Go's: one `Request`, one `Response`, and a handler is a function from one to
the other. A server runs a handler; a client is one; middleware wraps one; a test calls one. On purpose, there is

- no `ResponseWriter` beside `Response`, so no status written after a body, no Hijack;
- no `Body.Close` to forget: a small body arrives whole, and a `Headers` is a class, not a map that miscases keys;
- no `async` coloring and no `AbortController`: a read parks the fiber, so `res.text()` is a plain call, and every
  I/O has a deadline, a number, with a default;
- no data races in handlers: an isolate shares only frozen data; and one isolate per core, each with its own ring
  and listener, rather than a lock around `accept`;
- no unbounded defaults: headers, bodies, connections and redirects are limited, and a CR or LF in a header value
  cannot split a message;
- two error types in the whole stack, `IoError` and `ParseError`, both checked. A 404 is a `Response`, as in fetch;
  only failing to get one throws.

## Layers

| Module | Holds | Phase |
|---|---|---|
| src/io.c, src/rt.h | the scheduler: fibers park on ring completions; socket ops with deadlines | 1 |
| std/net | `dial`, `listen`, `Conn`, `Listener` | 1 |
| std/url | `Url`, `Query`, parsing, reference resolution | 1 |
| std/http | `Request`, `Response`, `Headers`, `Body`, `Handler`, `Client`, `Server` | 1 |
| std/http/h1 | HTTP/1.1 on a `Conn`, both directions; private | 1 |
| std/http/router | `Router`, patterns, middleware | 1 |
| std/tls | `Conn` over Mbed TLS, client and server | 2 |
| std/net/dns | a resolver in bober over the ring, /etc/hosts and resolv.conf, a TTL cache per isolate | 3 |
| std/http/h2, hpack | HTTP/2 streams on one TLS connection, chosen by ALPN, under the same Request and Response | 3 |
| std/http/websocket | `accept` and `connect` over the 101 upgrade hook | 3 |
| std/http/cookie | `Cookie`, `Jar` as a Client middleware | 3 |
| std/http/multipart | streaming parts, `form`, `encode` | 3 |
| std/compress, std/http/compress | deflate and gzip; `decompress` for a Client, `compress` for a Router | 3 |

Still to come, changing no API: isolates per core; a `Server.close` deadline, which wants a timer op; an h2 write queue,
as the reader fiber writes its acks itself and a peer that stops reading stalls it until the timeout; a header
timeout on h2; `file` responses over `splice`; multishot accept and provided buffer rings (`IOSQE_BUFFER_SELECT`), so
that an idle connection owns no buffer at all, where Go holds a goroutine and a buffer per idle connection.

## The run time

Before this, an I/O call submitted one operation and blocked the process on it. A server needs one in flight per
connection, so a call submits with `user_data` naming its fiber and parks; `bober_park`, when no fiber is ready, waits
on the ring instead of trapping, and each completion wakes its fiber. It traps only when nothing is ready and nothing
is in flight. Nothing in std changed shape: `File.read` still returns bytes, and now lets other fibers run while it
waits.

A parked operation must not point into the heap, which the collector moves, and a C frame's pointer is not updated
when it does. So a write's bytes are copied into malloc'd memory before it waits, and a read's buffer must be malloc'd
too. One memcpy of at most 64 KiB per operation is noise beside the system call. Paths are safe as heap strings only
because the kernel copies them during the submit call, which is why submission is eager.

A timeout is a linked `LINK_TIMEOUT`; the pair of completions it brings folds into one wake, `-ETIMEDOUT`. The non-
waiting socket calls are in rt.h too, as a `sockaddr` does not cross C's binding cleanly.

## Ownership on the client

There are no finalizers, so a response that holds a connection is a resource like a `File`. To keep the common case
free of one, a response body of known length up to 64 KiB arrives whole with the head, and only a longer or chunked
one streams from the connection, holding it until read to its end or closed. The pool keeps up to poolSize idle
connections, of all origins together, with no reader fiber for them; one found reset is dropped and an idempotent
request with a string body sent once more, as Go does.

The default client lives in a top-level `let`, so each isolate has a pool of its own and a frozen `const` never holds
a socket.

## Every core

`listen` sets SO_REUSEPORT, and the spec freezes a top-level `const` with its captures, so a program spawns an isolate
per core, each calling `serve(app.handle)` on the same frozen router, and nothing is shared that is not frozen. spawn
is not lowered yet, so this waits.

## Tests

A handler is a function, so most tests call it with a `Request` and no socket. Those that need one listen on port 0.
Tests see a frozen `now` and never wait on a timeout, which the kernel keeps in real time.
