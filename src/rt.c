//! The run time that code from src/emit.bob links against, but for its collector, src/gc.c; what std imports of it is
//! declared in rt.h. A thrown value is its tag, bits and ref after an _Unwind_Exception header, raised with
//! _Unwind_RaiseException and caught by the landing pad bober_personality finds; bobcc, which takes any C function to
//! unwind, compiles it. A trap prints where it happened and exits, or in a test fails just that test. A function that
//! can trap takes the module's source path and the byte offset in it last, and reports them as path:line:col.

#include "rt.h"
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

typedef struct { struct _Unwind_Exception header; int64_t tag, bits; void *ref; } Thrown;
/// The exception bober_personality last landed with, for bober_caught.
static Thrown *caught;

static int argc_;
static char **argv_;
static bool tests;
static uint64_t seed;  // random's state

// In tests, now is when Obergefell v. Hodges made same-sex marriage a right in all of the US, and random is seeded
// with the day Stonewall began.
#define OBERGEFELL 1435327200000000000
#define STONEWALL 0x19690628

static jmp_buf *test;          // where a trap in the running test or subtest goes, null outside one
static const char *testFile;  // the running test's source
static int32_t testAt;        // where in it the statement running is, -1 before the first
static int cap = -1, saved[2] = {1, 2};  // in tests, the file the output and errors go to, and where they went
/// Reserves the heap (src/gc.c), before any code runs that may store to it.
void bober_gc_start(void);
/// Drops every fiber but the first (src/fiber.c), as once a trap jumped back to it.
void bober_fibers_reset(void);
static char msg[4096];

void *bober_malloc(uint64_t n) { return bober_realloc(0, n); }

void *bober_realloc(void *p, uint64_t n) {
  if (!(p = realloc(p, n ? n : 1))) fputs("out of memory\n", stderr), exit(1);
  return p;
}

void bober_free(void *p) { free(p); }

/// Writes the n bytes at p to fd; whether all were.
static bool put(int fd, const char *p, int64_t n) {
  for (long w; n > 0; p += w, n -= w)
    if ((w = bober_io_write(fd, p, n)) <= 0) return false;
  return true;
}

/// The line and column of byte at of the file, or 0s if it cannot be read.
static void position(const char *file, int32_t at, int *line, int *col) {
  *line = *col = 0;
  long fd = file && at >= 0 ? bober_io_open(file, "r") : -1;
  if (fd < 0) return;
  char *p = bober_malloc(at);
  long n = 0;
  for (long r; n < at && (r = bober_io_read(fd, p + n, at - n)) > 0; n += r) {}
  bober_io_close(fd);
  *line = *col = 1;
  for (long i = 0; i < n; i++) p[i] == '\n' ? (*line)++, *col = 1 : (*col)++;
  free(p);
}

_Noreturn void bober_trap(const char *what, const char *file, int32_t at) {
  int line, col, testLine, testCol;
  position(file, at, &line, &col);
  position(testFile, test ? testAt : -1, &testLine, &testCol);
  char called[512] = "";  // the test's statement running, noted on what's first line if the trap is not on its line
  if (testLine && (!file || strcmp(file, testFile) || line != testLine))
    snprintf(called, sizeof called, " (called at %s:%d)", testFile, testLine);
  int n = line ? snprintf(msg, sizeof msg, "%s:%d:%d: ", file, line, col)
          : file ? snprintf(msg, sizeof msg, "%s: ", file) : 0;
  int first = strcspn(what, "\n");
  snprintf(msg + n, sizeof msg - n, "trap: %.*s%s%s", first, what, called, what + first);
  if (test) longjmp(*test, 1);
  put(saved[1], msg, strlen(msg)), put(saved[1], "\n", 1);
  exit(1);
}

/// A value of primitive kind (§8.2) held in bits as text, in buf; a string is boxed in ref, laid out as
/// builtin/string.bob says.
static int show(char *buf, int n, int32_t kind, int64_t bits, const uint8_t *ref) {
  switch (kind) {
  case 1: return snprintf(buf, n, "null");
  case 2: return snprintf(buf, n, bits & 1 ? "true" : "false");
  case 3: return snprintf(buf, n, "%d", (int8_t)bits);
  case 4: return snprintf(buf, n, "%d", (int16_t)bits);
  case 5: return snprintf(buf, n, "%d", (int32_t)bits);
  case 6: return snprintf(buf, n, "%lld", (long long)bits);
  case 7: return snprintf(buf, n, "%u", (uint8_t)bits);
  case 8: return snprintf(buf, n, "%u", (uint16_t)bits);
  case 9: return snprintf(buf, n, "%u", (uint32_t)bits);
  case 10: return snprintf(buf, n, "%llu", (unsigned long long)bits);
  case 11: { float f; int32_t i = bits; memcpy(&f, &i, 4); return snprintf(buf, n, "%g", f); }
  case 12: { double d; memcpy(&d, &bits, 8); return snprintf(buf, n, "%g", d); }
  case 13: {
    const uint8_t *s = ref;
    bool far = s[0] == 0xff;
    int64_t len = far ? *(const uint64_t *)s >> 8 : s[0];
    return snprintf(buf, n, "%.*s", (int)len, far ? *(const char **)(s + 16) + *(const uint64_t *)(s + 8) : (const char *)s + 1);
  }
  default: return snprintf(buf, n, "<value>");
  }
}

_Noreturn void bober_fail(const uint8_t *how, const char *file, int32_t at) {
  char what[2048] = "assert failed: ";
  show(what + 15, sizeof what - 15, 13, 0, how);
  bober_trap(what, file, at);
}

/// Where the value last thrown was first thrown, for bober_uncaught.
static const char *thrownFile;
static int32_t thrownAt;

void bober_throw(int64_t tag, int64_t bits, void *ref, const char *file, int32_t at) {
  if (file) thrownFile = file, thrownAt = at;
  free(caught), caught = 0;  // A landing pad loads what it caught at once, so the last throw's is done with.
  Thrown *e = bober_malloc(sizeof(Thrown));
  *e = (Thrown){.tag = tag, .bits = bits, .ref = ref};
  memcpy(&e->header.exception_class, "bober\0\0\0", 8);
  _Unwind_RaiseException(&e->header);
}

_Noreturn void bober_uncaught(int64_t tag, int64_t bits, void *ref, const uint8_t *shown) {
  char what[2048] = "uncaught throw: ";
  show(what + 16, sizeof what - 16, shown ? 13 : *(const int64_t *)tag, bits, shown ? shown : ref);
  bober_trap(what, thrownFile, thrownAt);
}

/// Reads a value of the language-specific data: a uleb128, or a udata4, the two encodings LLVM uses for call sites.
static uintptr_t dwarf(const uint8_t **p, uint8_t enc) {
  uintptr_t v = 0;
  if (enc == 0x03) return memcpy(&v, *p, 4), *p += 4, v;
  for (int shift = 0;; shift += 7) {
    uint8_t b = *(*p)++;
    v |= (uintptr_t)(b & 0x7f) << shift;
    if (!(b & 0x80)) return v;
  }
}

/// The personality routine of every emitted function: its landing pads all catch everything, so this finds the call
/// site of the throwing instruction and lands on its pad, which takes the exception from caught.
_Unwind_Reason_Code bober_personality(int version, _Unwind_Action actions, uint64_t class, struct _Unwind_Exception *e,
                                     struct _Unwind_Context *ctx) {
  const uint8_t *p = (const uint8_t *)_Unwind_GetLanguageSpecificData(ctx);
  if (!p || memcmp(&e->exception_class, "bober\0\0\0", 8)) return _URC_CONTINUE_UNWIND;
  uintptr_t start = _Unwind_GetRegionStart(ctx), ip = _Unwind_GetIP(ctx) - 1;
  if (*p++ != 0xff) bober_trap("landing pads not relative to the function", "rt.c", -1);  // LLVM split the function
  if (*p++ != 0xff) dwarf(&p, 0x01);  // skip the type table
  uint8_t enc = *p++;
  const uint8_t *end = p + dwarf(&p, 0x01);
  while (p < end) {
    uintptr_t at = start + dwarf(&p, enc), len = dwarf(&p, enc), pad = dwarf(&p, enc);
    dwarf(&p, 0x01);
    if (ip < at) break;
    if (ip >= at + len) continue;
    if (!pad) break;
    if (actions & _UA_SEARCH_PHASE) return _URC_HANDLER_FOUND;
    caught = (Thrown *)e;
    _Unwind_SetIP(ctx, start + pad);
    return _URC_INSTALL_CONTEXT;
  }
  return _URC_CONTINUE_UNWIND;
}

/// Where the tag, bits and ref of what a landing pad caught are.
void *bober_caught(void) { return &caught->tag; }

void bober_start(int argc, char **argv, int32_t tested) {
  bober_gc_start();
  argc_ = argc, argv_ = argv, tests = tested;
  if (tests) {
    FILE *f = tmpfile();
    seed = STONEWALL, bober_logical = 1, cap = f ? fileno(f) : -1, saved[0] = dup(1), saved[1] = dup(2);
    dup2(cap, 1), dup2(cap, 2);
  } else getentropy(&seed, sizeof seed);
  signal(SIGPIPE, SIG_IGN);  // a write to a socket the peer closed fails with EPIPE instead
  bober_types_complete();
}

const char *bober_arg(int32_t i) { return i < argc_ ? argv_[i] : 0; }

const char *bober_env(const char *name) { return getenv(name); }

/// A test or subtest running, or the tests: its children's entries in the report so far, members of a JSON object,
/// whether one failed, and where its output starts in cap.
typedef struct Level { struct Level *up; char *kids; size_t n; bool failed; long from; } Level;
static Level all, *level = &all;
static size_t sent;  // of all's entries, printed

/// Appends the n bytes at s to l's entries, escaped as in a JSON string if esc.
static void add(Level *l, const char *s, size_t n, bool esc) {
  char *p = (l->kids = bober_realloc(l->kids, l->n + 6 * n + 1)) + l->n;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = s[i];
    if (esc && (c == '"' || c == '\\')) *p++ = '\\';
    if (esc && c < 0x20) p += sprintf(p, c == '\n' ? "\\n" : c == '\t' ? "\\t" : "\\u%04x", c);
    else *p++ = c;
  }
  l->n = p - l->kids;
}

static void raw(Level *l, const char *s) { add(l, s, strlen(s), false); }

/// Appends the key name to l's entries.
static void key(Level *l, const char *name) {
  raw(l, l->n ? ",\"" : "\""), add(l, name, strlen(name), true), raw(l, "\":");
}

/// Prints the entries of the tests not yet printed, so that a crash leaves those before it and the key of its test.
static void flush(void) { put(saved[0], all.kids + sent, all.n - sent), sent = all.n; }

/// Ends l, which trapped with msg if trapped: adds its value to its parent's entries, "ok" if it and its subtests
/// passed, else what it printed, at most the last 4 KiB and with its subtests' output, and msg, or if subtests failed
/// their entries, and that as "trap", which a subtest may also be named.
static void end(Level *l, bool trapped) {
  Level *up = l->up;
  if (trapped) {
    Level *to = l->failed ? (key(l, "trap"), l) : up;
    char buf[4096], *b = buf;
    long at = lseek(cap, 0, SEEK_CUR);
    bool cut = at - l->from > (long)sizeof buf;
    long n = pread(cap, buf, sizeof buf, cut ? at - (long)sizeof buf : l->from);
    while (cut && n > 0 && (*b & 0xC0) == 0x80) b++, n--;  // a character cut off
    raw(to, "\""), add(to, b, n > 0 ? n : 0, true);
    if (n > 0 && b[n - 1] != '\n') raw(to, "\\n");
    add(to, msg, strlen(msg), true), raw(to, "\"");
  }
  if (l->failed) raw(up, "{"), add(up, l->kids, l->n, false), raw(up, "}");
  else if (!trapped) raw(up, "\"ok\"");
  up->failed |= l->failed || trapped;
  free(l->kids);
}

/// Runs f(env) as the test or subtest name, in the running one if any; a trap fails it.
static void run(const char *name, void (*f)(void *), void *env) {
  key(level, name), flush();
  Level l = {level, 0, 0, false, lseek(cap, 0, SEEK_CUR)};
  jmp_buf here, *up = test;
  level = &l, test = &here;
  if (setjmp(here)) bober_fibers_reset(), end(&l, true);
  else f(env), end(&l, false);
  test = up, level = l.up;
  flush();
}

void bober_test(const char *name, const char *file, void (*f)(void)) {
  seed = STONEWALL, testFile = file, testAt = -1;
  if (!all.n) put(saved[0], "{", 1);  // not before, as a trap in initializing prints instead
  run(name, (void (*)(void *))f, 0);
  ftruncate(cap, 0), lseek(cap, 0, SEEK_SET);
}

void bober_subtest(const char *name, uint64_t f) { run(name, *(void (**)(void *))f, (void *)(uintptr_t)f); }

void bober_mark(int32_t at) { testAt = at; }

/// Ends the report of the tests, a JSON object of their entries; whether one failed.
int32_t bober_tested(void) {
  put(saved[0], all.n ? "}\n" : "{}\n", all.n ? 2 : 3);
  return all.failed;
}

int64_t bober_now(void) {
  struct timespec t;
  return tests ? OBERGEFELL : (clock_gettime(CLOCK_REALTIME, &t), t.tv_sec * 1000000000 + t.tv_nsec);
}

const char *bober_tz(void) { return tests ? "UTC" : getenv("TZ"); }

/// SplitMix64.
uint64_t bober_random(void) {
  uint64_t z = seed += 0x9e3779b97f4a7c15;
  z = (z ^ z >> 30) * 0xbf58476d1ce4e5b9;
  z = (z ^ z >> 27) * 0x94d049bb133111eb;
  return z ^ z >> 31;
}

long bober_entropy(unsigned char *p, long n) {
  if (tests) for (long i = 0; i < n; i++) p[i] = bober_random();
  else for (long i = 0; i < n; i += 256) if (getentropy(p + i, n - i < 256 ? n - i : 256)) return -errno;
  return 0;
}
