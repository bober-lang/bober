//! The run time that code from src/emit.bob links against, but for its collector, src/gc.c; what std imports of it is
//! declared in rt.h. A thrown value is its tag, bits and ref after an _Unwind_Exception header, raised with
//! _Unwind_RaiseException and caught by the landing pad bober_personality finds; bobcc, which takes any C function to
//! unwind, compiles it. A trap prints where it happened and exits, or in a test fails just that test. A function that
//! can trap takes the module's source path and the byte offset in it last, and reports them as path:line:col.

#define _GNU_SOURCE  // REG_RSP
#include "rt.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/ucontext.h>  // <ucontext.h> is an #error on macOS without _XOPEN_SOURCE
#include <unistd.h>
#include <unwind.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

typedef struct {
  struct _Unwind_Exception header;
  int64_t tag, bits;
  void *ref;
  const char *file;
  int32_t at;
  void *show;
} Thrown;
/// The exception bober_personality last landed with, for bober_caught.
static _Thread_local Thrown *caught;
/// Where the last throw was, for bober_uncaught: as nothing caught it, caught is not it.
static _Thread_local const char *thrown_file;
static _Thread_local int32_t thrown_at;

static int argc_;
static char **argv_;
static bool tests;
static _Thread_local uint64_t seed;  // random's state

// In tests, now starts when Obergefell v. Hodges made same-sex marriage a right in all of the US, and random is seeded
// with the day Stonewall began.
#define OBERGEFELL 1435327200000000000
#define STONEWALL 0x19690628

static _Thread_local sigjmp_buf *trap_jmp;  // where a trap in a test or an isolate goes, null on main outside tests
static _Thread_local const char *testFile;  // the running test's source
static _Thread_local int32_t testAt;        // where in it the statement running is, -1 before the first
static int cap = -1, saved[2] = {1, 2};  // in tests, the file the output and errors go to, and where they went
static const char *tmp;  // in tests, the directory in which each gets one of its own, by number, if any: BOBER_TMP
/// What src/io.c knows of the tests: the running one's directory, the files they read and whether they are tainted.
extern char bober_tmp[], *bober_reads;
extern long bober_nreads;
extern int bober_tainted;
/// Reserves the heap (src/gc.c), before any code runs that may store to it; at is the frame pointer of the program's
/// entry, whose frames the collector walks up to.
void bober_gc_start(void **at);
void bober_fibers_init(void);
/// Drops every fiber but the first (src/fiber.c), as once a trap jumped back to it.
void bober_fibers_reset(void);
static _Thread_local char msg[4096];
static _Thread_local bool in_init;
static void (*lets_init)(void);

/// Runs the top-level lets' initializers, this isolate's own.
static void init_lets(void) {
  in_init = true;
  lets_init();
  in_init = false;
}

void bober_set_lets_init(void (*fn)(void)) { lets_init = fn, init_lets(); }

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
  position(testFile, trap_jmp ? testAt : -1, &testLine, &testCol);
  char called[512] = "";  // the test's statement running, noted on what's first line if the trap is not on its line
  if (testLine && (!file || strcmp(file, testFile) || line != testLine))
    snprintf(called, sizeof called, " (called at %s:%d)", testFile, testLine);
  int n = line ? snprintf(msg, sizeof msg, "%s:%d:%d: ", file, line, col)
          : file ? snprintf(msg, sizeof msg, "%s: ", file) : 0;
  int first = strcspn(what, "\n");
  snprintf(msg + n, sizeof msg - n, "trap: %.*s%s%s", first, what, called, what + first);
  if (trap_jmp) siglongjmp(*trap_jmp, 1);
  put(saved[1], msg, strlen(msg)), put(saved[1], "\n", 1);
  exit(1);
}

#if defined(__APPLE__) && defined(__aarch64__)
#define SP(c) (c)->uc_mcontext->__ss.__sp
#elif defined(__APPLE__)
#define SP(c) (c)->uc_mcontext->__ss.__rsp
#elif defined(__aarch64__)
#define SP(c) (c)->uc_mcontext.sp
#else
#define SP(c) (c)->uc_mcontext.gregs[REG_RSP]
#endif

/// Traps on a write to a frozen value; ends the process as an uncaught trap on a fault by a frame stepping off the end
/// of its stack, onto the guard below, which may be in the C library holding its state; any other fault ends it as it
/// would have without.
/// Gives fault a stack of its own, as the faulting one is full, anew in a forked child, which macOS gives none.
static void altStack(void) {
  static char alt[1 << 16];
  sigaltstack(&(stack_t){.ss_sp = alt, .ss_size = sizeof alt}, 0);
}

static void fault(int sig, siginfo_t *info, void *c) {
  uintptr_t sp = SP((ucontext_t *)c), at = (uintptr_t)info->si_addr;
  if (bober_frozen((void *)at)) {
    const char *what = "trap: cannot write frozen value";
    size_t n = strlen(what);
    memcpy(msg, what, n + 1);
    if (trap_jmp) siglongjmp(*trap_jmp, 1);
    put(saved[1], what, n), put(saved[1], "\n", 1), _exit(1);
  }
  if (at - sp + (1 << 16) < 1 << 17) put(saved[1], "trap: stack overflow\n", 21), _exit(1);  // within a frame of sp
  signal(sig, SIG_DFL), raise(sig);
}

/// String s as text, in buf; s is boxed, laid out as builtin/string.bob says.
static int show(char *buf, int n, const uint8_t *s) {
  bool far = s[0] == 0xff;
  int64_t len = far ? *(const uint64_t *)s >> 8 : s[0];
  return snprintf(buf, n, "%.*s", (int)len, far ? *(const char **)(s + 16) + *(const uint64_t *)(s + 8) : (const char *)s + 1);
}

_Noreturn void bober_fail(const uint8_t *how, const char *file, int32_t at) {
  char what[2048] = "assert failed: ";
  show(what + 15, sizeof what - 15, how);
  bober_trap(what, file, at);
}

void bober_throw(int64_t tag, int64_t bits, void *ref, const char *file, int32_t at, void *show) {
  thrown_file = file, thrown_at = at;
  free(caught), caught = 0;  // A landing pad loads what it caught at once, so the last throw's is done with.
  Thrown *e = bober_malloc(sizeof(Thrown));
  *e = (Thrown){.tag = tag, .bits = bits, .ref = ref, .file = file, .at = at, .show = show};
  memcpy(&e->header.exception_class, "bober\0\0\0", 8);
  _Unwind_RaiseException(&e->header);
}

_Noreturn void bober_uncaught(const uint8_t *shown) {
  char what[2048] = "uncaught throw: ";
  show(what + 16, sizeof what - 16, shown);
  bober_trap(what, thrown_file, thrown_at);
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

/// In tests, the pool of tokens of the processes that run them, read from its first socket and given back to its second:
/// a process takes one to run, and gives it back as it exits, or its parent does if it could not.
static int jobs[2];

/// Makes a pool of tokens, one per processor, into fd.
static void pool(int *fd) {
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd)) fputs("cannot make the pool of test processes\n", stderr), exit(1);
  for (long n = sysconf(_SC_NPROCESSORS_ONLN); n > 0; n--) put(fd[1], "+", 1);
}

void bober_jobs(void) {
  int fd[2];
  char s[32];
  pool(fd), snprintf(s, sizeof s, "%d %d", fd[0], fd[1]), setenv("BOBER_JOBS", s, 1);
}

/// Takes a token of the pool, waiting for one if wait: whether it did.
static bool take(bool wait) {
  char t;
  long n;
  while ((n = recv(jobs[0], &t, 1, wait ? 0 : MSG_DONTWAIT)) < 0 && errno == EINTR) {}
  return n == 1;
}

static void give(void) { put(jobs[1], "+", 1); }

/// Where the tag, bits and ref of what a landing pad caught are.
void *bober_caught(void) { return &caught->tag; }

void bober_start(int argc, char **argv, int32_t tested) {
  in_init = true;
  bober_gc_start(*(void ***)__builtin_frame_address(0));  // the frame pointer of the entry, which calls this
  bober_fibers_init();
  argc_ = argc, argv_ = argv, tests = tested;
  if (tests) {
    FILE *f = tmpfile();
    seed = STONEWALL, bober_logical = 1, cap = f ? fileno(f) : -1, saved[0] = dup(1), saved[1] = dup(2);
    dup2(cap, 1), dup2(cap, 2);
    tmp = getenv("BOBER_TMP") ? realpath(getenv("BOBER_TMP"), 0) : 0;
    const char *j = getenv("BOBER_JOBS");
    if (!j || sscanf(j, "%d %d", jobs, jobs + 1) != 2) pool(jobs);
    unsetenv("BOBER_TMP"), unsetenv("BOBER_JOBS");  // not for the programs tests run
  } else getentropy(&seed, sizeof seed);
  signal(SIGPIPE, SIG_IGN);  // a write to a socket the peer closed fails with EPIPE instead
  altStack();
  struct sigaction a = {.sa_sigaction = fault, .sa_flags = SA_SIGINFO | SA_ONSTACK};
  sigaction(SIGSEGV, &a, 0), sigaction(SIGBUS, &a, 0);
  bober_types_complete();
}

const char *bober_arg(int32_t i) { return i < argc_ ? argv_[i] : 0; }

const char *bober_env(const char *name) {
  return !tests ? getenv(name) : !strcmp(name, "TMPDIR") && *bober_tmp ? bober_tmp : 0;
}

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

/// Appends to l, escaped, at most the last 4 KiB of what file fd holds from byte from, and a newline if it ends none.
static void tail(Level *l, int fd, long from) {
  char buf[4096], *b = buf;
  long at = lseek(fd, 0, SEEK_END);
  bool cut = at - from > (long)sizeof buf;
  long n = pread(fd, buf, sizeof buf, cut ? at - (long)sizeof buf : from);
  while (cut && n > 0 && (*b & 0xC0) == 0x80) b++, n--;  // a character cut off
  add(l, b, n > 0 ? n : 0, true);
  if (n > 0 && b[n - 1] != '\n') raw(l, "\\n");
}

/// Ends l, which trapped with msg if trapped: adds its value to its parent's entries, "ok" if it and its subtests
/// passed, else what it printed, at most the last 4 KiB and with its subtests' output, and msg, or if subtests failed
/// their entries, and that as "trap", which a subtest may also be named.
static void end(Level *l, bool trapped) {
  Level *up = l->up;
  if (trapped) {
    Level *to = l->failed ? (key(l, "trap"), l) : up;
    raw(to, "\""), tail(to, cap, l->from), add(to, msg, strlen(msg), true), raw(to, "\"");
  }
  if (l->failed) raw(up, "{"), add(up, l->kids, l->n, false), raw(up, "}");
  else if (!trapped) raw(up, "\"ok\"");
  up->failed |= l->failed || trapped;
  free(l->kids);
}

/// In a test's process, its helpers: each runs the test as it does, but of its subtests only those it claims, so that
/// one of many subtests runs on as many processors as are free, each with the file its result goes to. A helper's
/// result is whether it failed, whether it is tainted, and the files it read.
static struct { pid_t pid; FILE *res; } helpers[64];
static int nhelpers;
static bool helper, alone;  // whether this process is a helper, and whether it runs every subtest, with no helpers
static int claimed[2];      // of the test's subtests, the first not claimed, a datagram its processes pass on
static long next, from, to;  // the next subtest of the test, and those this process claimed last

/// How many threads this process has, 0 if unknown. It forks a helper only while it has one, as another's lock taken
/// would stay so in the fork, as LLVM's threads' may.
static int threads(void) {
#ifdef __APPLE__
  struct proc_taskinfo t;
  return proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &t, sizeof t) == sizeof t ? t.pti_threadnum : 0;
#else
  char b[4096], *p;
  int fd = open("/proc/self/status", O_RDONLY);
  long k = fd < 0 ? -1 : read(fd, b, sizeof b - 1);
  close(fd);
  return k > 0 && (b[k] = 0, p = strstr(b, "Threads:")) ? atoi(p + 8) : 0;
#endif
}

/// A subtest of a subtest is its parent's; of a test, once this process is past those it claimed last, it claims the
/// next not claimed, twice as many as last if those took it less than a millisecond, so that claims cost little next to
/// their subtests, having forked a helper first if a token is free.
int32_t bober_claim(void) {
  static long n = 1;
  static int64_t last;
  if (level->up != &all) return true;
  long i = next++;
  if (i >= to) {
    if (!helper && !alone && nhelpers < 64 && take(false)) {
      FILE *res = threads() == 1 ? tmpfile() : 0;
      pid_t pid = res ? (fflush(0), fork()) : -1;
      if (pid < 0) give(), res && fclose(res);
      else if (!pid) helper = true, saved[0] = fileno(res);
      else helpers[nhelpers].pid = pid, helpers[nhelpers++].res = res;
    }
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    int64_t now = t.tv_sec * 1000000000 + t.tv_nsec;
    if (now - last < 1000000 && n < 1 << 16) n *= 2;
    last = now;
    if (recv(claimed[0], &from, sizeof from, 0) < 0) bober_trap("cannot claim a subtest", 0, -1);
    to = from + n;
    if (send(claimed[1], &to, sizeof to, 0) < 0) bober_trap("cannot claim a subtest", 0, -1);
  }
  return i >= from;
}

static void bober_spawn_finish(void);

/// Runs f(env) as the test or subtest name, in the running one if any; a trap fails it. A helper exits at its test's
/// end, with its result.
static void run(const char *name, void (*f)(void *), void *env) {
  key(level, name), flush();
  Level l = {level, 0, 0, false, lseek(cap, 0, SEEK_CUR)};
  sigjmp_buf here, *up = trap_jmp;
  level = &l, trap_jmp = &here;
  if (sigsetjmp(here, 1)) bober_spawn_finish(), bober_fibers_reset(), end(&l, true);
  else f(env), end(&l, false);
  trap_jmp = up, level = l.up;
  if (helper && level == &all) {
    put(saved[0], all.failed ? "1" : "0", 1), put(saved[0], bober_tainted ? "1" : "0", 1);
    put(saved[0], bober_reads, bober_nreads), give(), _exit(0);
  }
  flush();
}

/// The n bytes f holds.
static char *slurp(FILE *f, long *n) {
  char *r = bober_malloc(*n = lseek(fileno(f), 0, SEEK_END));
  *n = pread(fileno(f), r, *n, 0);
  return r;
}

/// Waits for the helpers, taking the files they read and whether they are tainted: whether one failed.
static bool helped(void) {
  bool failed = false;
  for (int i = 0; i < nhelpers; i++) {
    int status;
    long n;
    while (waitpid(helpers[i].pid, &status, 0) < 0 && errno == EINTR) {}
    char *r = slurp(helpers[i].res, &n);
    if (n < 2) give(), failed = true;
    else {
      failed |= r[0] == '1', bober_tainted |= r[1] == '1';
      bober_reads = bober_realloc(bober_reads, bober_nreads + n - 2);
      memcpy(bober_reads + bober_nreads, r + 2, n - 2), bober_nreads += n - 2;
    }
    free(r), fclose(helpers[i].res);
  }
  return failed;
}

/// A test running in a process of its own: its name, source and function, whether it runs alone, the files its output
/// and its result go to, and its entry of the report once it exited.
typedef struct {
  pid_t pid;
  const char *name, *file;
  void (*f)(void);
  bool alone;
  FILE *out, *res;
  Level entry;
} Child;
static Child *children;
static int nchildren, running;

/// Forks c's test from this process as its module initialized it, so that it sees that state and none of another test,
/// in a directory of its own if tmp. The child writes its entry of the report, then a line of whether it is tainted, or
/// "r" to be run again, alone, as a test whose helpers or itself failed is, for a report as of one process; and the files
/// it read (src/io.c), to a file of its own, which reap reads; it exits 1 if the test failed.
static void spawn(Child *c) {
  static int n;  // directories so far
  if (tmp) snprintf(bober_tmp, PATH_MAX, "%s/%d", tmp, ++n), mkdir(bober_tmp, 0777);
  c->out = tmpfile(), c->res = tmpfile();
  if (!c->out || !c->res) bober_trap("cannot make a test's files", 0, -1);
  fflush(0);
  if ((c->pid = fork()) < 0) bober_trap("cannot fork a test", 0, -1);
  if (c->pid) return;
  seed = STONEWALL, testFile = c->file, testAt = -1, bober_nreads = 0, alone = c->alone;
  all.n = sent = 0, all.failed = false;  // reap's, of tests before or of a test run again
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, claimed) || send(claimed[1], &(long){0}, sizeof(long), 0) < 0)
    bober_trap("cannot share a test's subtests", 0, -1);
  altStack();
  cap = saved[1] = fileno(c->out), saved[0] = fileno(c->res);
  dup2(cap, 1), dup2(cap, 2);
  run(c->name, (void (*)(void *))c->f, 0);
  bool redo = helped() || nhelpers && all.failed;
  put(saved[0], redo ? "\nr" : bober_tainted ? "\n1" : "\n0", 2), put(saved[0], bober_reads, bober_nreads);
  if (!redo) give();  // a redo's its own
  _exit(all.failed);
}

/// Waits for a child to exit, child i if i >= 0, else any, if wait, and takes its entry, its taint and the files it read,
/// or if it exited without its entry, the end of its output and how it exited; or runs it again if it asks to: whether
/// it reaped one.
static bool reap(int i, bool wait) {
  int status;
  pid_t pid;
  while ((pid = waitpid(i < 0 ? -1 : children[i].pid, &status, wait ? 0 : WNOHANG)) < 0 && errno == EINTR) {}
  if (!pid) return false;
  if (pid < 0) bober_trap("cannot wait for a test", 0, -1);
  Child *c = children;
  while (c->pid != pid) c++;
  long n;
  char *r = slurp(c->res, &n), *nl = n > 0 ? memchr(r, '\n', n) : 0;
  if (nl && nl + 1 < r + n && WIFEXITED(status) && nl[1] == 'r') {
    free(r), fclose(c->out), fclose(c->res), c->alone = true, spawn(c);
    return true;
  }
  if (nl && nl + 1 < r + n && WIFEXITED(status)) {
    add(&c->entry, r, nl - r, false), bober_tainted |= nl[1] == '1';
    long k = r + n - nl - 2;
    bober_reads = bober_realloc(bober_reads, bober_nreads + k);
    memcpy(bober_reads + bober_nreads, nl + 2, k), bober_nreads += k;
    all.failed |= WEXITSTATUS(status) != 0;
  } else {
    char how[32];
    snprintf(how, sizeof how, WIFEXITED(status) ? "exit %d" : "signal %d",
             WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
    key(&c->entry, c->name), raw(&c->entry, "\""), tail(&c->entry, fileno(c->out), 0);
    raw(&c->entry, how), raw(&c->entry, "\""), all.failed = true, give();
  }
  free(r), fclose(c->out), fclose(c->res), c->pid = 0, running--;
  return true;
}

/// Runs the test name in a child (spawn), as many at once as tokens of the pool are free. Those that exited are reaped
/// first, so that only the files of those running stay open.
void bober_test(const char *name, const char *file, void (*f)(void)) {
  while (running && reap(-1, false)) {}
  while (!take(!running)) reap(-1, true);
  Child c = {0, name, file, f};
  spawn(&c);
  children = bober_realloc(children, ++nchildren * sizeof *children);
  children[nchildren - 1] = c, running++;
}

void bober_subtest(const char *name, uint64_t f) { run(name, *(void (**)(void *))f, (void *)(uintptr_t)f); }

void bober_mark(int32_t at) { testAt = at; }

/// Prints the report of the tests, a JSON object of their entries, in their order, and a line of the files they read,
/// a JSON array, null if they did what else their results may depend on (src/io.c); whether one failed.
int32_t bober_tested(void) {
  for (int i = 0; i < nchildren; i++) {
    while (children[i].pid) reap(i, true);
    raw(&all, i ? "," : ""), add(&all, children[i].entry.kids, children[i].entry.n, false);
  }
  put(saved[0], "{", 1), flush(), put(saved[0], "}\n", 2);
  Level reads = {0};
  raw(&reads, bober_tainted ? "null" : "[");
  for (long i = 0; !bober_tainted && i < bober_nreads; i += strlen(bober_reads + i) + 1)
    raw(&reads, i ? ",\"" : "\""), add(&reads, bober_reads + i, strlen(bober_reads + i), true), raw(&reads, "\"");
  raw(&reads, bober_tainted ? "\n" : "]\n");
  put(saved[0], reads.kids, reads.n);
  return all.failed;
}

int64_t bober_now(void) {
  struct timespec t;
  return tests ? OBERGEFELL + bober_logical : (clock_gettime(CLOCK_REALTIME, &t), t.tv_sec * 1000000000 + t.tv_nsec);
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

// Isolates (spawn, §6.10): each runs on its own OS thread with its own heap, fibers and I/O loop.

void bober_gc_isolate(void **at);
void bober_gc_finish(void);
void bober_gc_set_roots(bober_union *roots, int64_t n);
int bober_io_wake_fd(void);
void bober_io_signal(int fd);
void bober_io_finish(void);

typedef struct bober_transfer bober_transfer;
typedef struct { bober_union u; const bober_layout *layout; int64_t size; } SpawnVal;
typedef struct { bober_union u; bober_transfer *copy; } SpawnSlot;

int bober_export(bober_union u, const bober_layout *hint_layout, int64_t hint_size, bober_transfer **out);
bober_union bober_import(bober_union u, const bober_transfer *t);
void bober_transfer_free(bober_transfer *t);

typedef int32_t (*SpawnEntry)(void *descs, void *args, const uint8_t *given, SpawnVal *out, void *err);
typedef struct { bober_union u; const char *file; int32_t at; void *show; } SpawnThrown;

typedef struct SpawnState SpawnState;
typedef struct Spawn {
  SpawnEntry fn;
  uint64_t *descs;
  int64_t ndescs;
  SpawnSlot *args;
  uint8_t *given;
  int64_t nargs;
  SpawnState *parent;
  uint64_t waiter;
  struct Spawn *next_done;
  bool queued;
  int status;  // 0 = running, 1 = ok, 2 = threw, 3 = trapped
  SpawnSlot result;
  const char *err_file;
  int32_t err_at;
  void *err_show;
  char *trap_msg;
} Spawn;

struct SpawnState {
  pthread_mutex_t mu;
  int refs, wake_fd, pending;
  Spawn *done_head, *done_tail;
};

static _Thread_local SpawnState *spawn_state;

static SpawnState *get_spawn_state(void) {
  if (!spawn_state) {
    spawn_state = bober_malloc(sizeof *spawn_state);
    *spawn_state = (SpawnState){.mu = PTHREAD_MUTEX_INITIALIZER, .refs = 1, .wake_fd = bober_io_wake_fd()};
  }
  return spawn_state;
}

static void release_spawn_state(SpawnState *st) {
  pthread_mutex_lock(&st->mu);
  int left = --st->refs;
  pthread_mutex_unlock(&st->mu);
  if (!left) pthread_mutex_destroy(&st->mu), free(st);
}

static void free_spawn(Spawn *s) {
  for (int64_t i = 0; i < s->nargs; i++) bober_transfer_free(s->args[i].copy);
  free(s->args), free(s->given), free(s->descs);
  bober_transfer_free(s->result.copy), free(s->trap_msg), free(s);
}

static void bober_spawn_finish(void) {
  SpawnState *st = spawn_state;
  if (!st) return;
  spawn_state = 0;
  pthread_mutex_lock(&st->mu);
  st->wake_fd = -1;
  Spawn *d = st->done_head;
  st->done_head = st->done_tail = 0;
  pthread_mutex_unlock(&st->mu);
  for (Spawn *n; d; d = n) n = d->next_done, free_spawn(d);
  release_spawn_state(st);
}

int bober_spawn_pending(void) {
  SpawnState *st = spawn_state;
  if (!st) return 0;
  pthread_mutex_lock(&st->mu);
  int p = st->pending;
  pthread_mutex_unlock(&st->mu);
  return p;
}

int bober_spawn_poll(void) {
  SpawnState *st = spawn_state;
  if (!st) return 0;
  pthread_mutex_lock(&st->mu);
  Spawn *d = st->done_head;
  st->done_head = st->done_tail = 0;
  for (Spawn *s = d; s; s = s->next_done) s->queued = false;
  pthread_mutex_unlock(&st->mu);
  int woke = 0;
  for (Spawn *s = d, *n; s; s = n) {
    n = s->next_done, s->next_done = 0;
    if (s->waiter) bober_wake(s->waiter), woke = 1;
  }
  return woke;
}

/// Builds a bober_string from a C string in the running isolate's heap.
static bober_string make_string(const char *p, int64_t n) {
  bober_string s = {0};
  if (n <= 15) {
    uint8_t *b = (uint8_t *)&s;
    b[0] = (uint8_t)n;
    if (n) memcpy(b + 1, p, n);
    return s;
  }
  void *buf = bober_strbuf(16 + n);
  *(int64_t *)buf = n, *((int64_t *)buf + 1) = n;
  memcpy((char *)buf + 16, p, n);
  return (bober_string){((uint64_t)n << 8) | 0xff, (const char *)(uintptr_t)16, buf};
}

static void *isolate_main(void *arg) {
  Spawn *s = arg;
  char *alt = bober_malloc(1 << 16);
  sigaltstack(&(stack_t){.ss_sp = alt, .ss_size = 1 << 16}, 0);
  bober_gc_isolate(__builtin_frame_address(0));
  bober_fibers_init();
  if (tests) seed = STONEWALL;
  else getentropy(&seed, sizeof seed);

  int status = 1;
  sigjmp_buf here;
  trap_jmp = &here;
  bober_union *volatile in_args = 0;
  if (sigsetjmp(here, 1)) {
    in_init = false;
    bober_gc_set_roots(0, 0);
    status = 3;
    s->trap_msg = strdup(msg);
  } else {
    init_lets();
    if (s->nargs > 0) {
      in_args = calloc(s->nargs, sizeof *in_args);
      if (!in_args) bober_trap("out of memory", 0, -1);
      bober_gc_set_roots(in_args, s->nargs);
      for (int64_t i = 0; i < s->nargs; i++) {
        if (s->given[i]) in_args[i] = bober_import(s->args[i].u, s->args[i].copy);
        bober_transfer_free(s->args[i].copy), s->args[i].copy = 0;
      }
    }
    SpawnVal out = {.size = -1};
    SpawnThrown err = {0};
    int32_t r = s->fn(s->descs, in_args, s->given, &out, &err);
    bober_gc_set_roots(0, 0);
    if (r) out = (SpawnVal){err.u, 0, -1};
    if (!bober_export(out.u, out.layout, out.size, &s->result.copy))
      bober_trap("value cannot cross isolates", err.file, err.at);
    s->result.u = out.u, s->err_file = err.file, s->err_at = err.at, s->err_show = err.show;
    status = r ? 2 : 1;
  }
  free(in_args);
  trap_jmp = 0;
  free(caught), caught = 0;
  bober_fibers_reset();
  bober_spawn_finish();
  bober_io_finish();
  bober_gc_finish();
  sigaltstack(&(stack_t){.ss_flags = SS_DISABLE}, 0);
  free(alt);

  SpawnState *st = s->parent;
  pthread_mutex_lock(&st->mu);
  s->status = status;
  if (st->wake_fd < 0) {
    pthread_mutex_unlock(&st->mu);
    free_spawn(s);
  } else {
    if (s->waiter) {
      if (st->done_tail) st->done_tail->next_done = s;
      else st->done_head = s;
      st->done_tail = s, s->queued = true;
    }
    bober_io_signal(st->wake_fd);
    pthread_mutex_unlock(&st->mu);
  }
  release_spawn_state(st);
  return 0;
}

uint64_t bober_spawn_start(void *fn, const uint64_t *descs, int64_t ndescs, const void *args, const uint8_t *given,
                           int64_t nargs, const char *file, int32_t at) {
  if (in_init) bober_trap("spawn before main", file, at);
  Spawn *s = calloc(1, sizeof *s);
  if (!s) bober_trap("out of memory", file, at);
  s->fn = (SpawnEntry)fn, s->ndescs = ndescs, s->nargs = nargs;
  if (ndescs > 0) {
    s->descs = bober_malloc(ndescs * sizeof *s->descs);
    memcpy(s->descs, descs, ndescs * sizeof *s->descs);
  }
  if (nargs > 0) {
    s->args = memset(bober_malloc(nargs * sizeof *s->args), 0, nargs * sizeof *s->args);
    s->given = memcpy(bober_malloc(nargs), given, nargs);
    const SpawnVal *v = args;
    for (int64_t i = 0; i < nargs; i++) {
      if (!s->given[i]) continue;
      if (!bober_export(v[i].u, v[i].layout, v[i].size, &s->args[i].copy)) {
        free_spawn(s);
        bober_trap("value cannot cross isolates", file, at);
      }
      s->args[i].u = v[i].u;
    }
  }
  SpawnState *st = get_spawn_state();
  pthread_mutex_lock(&st->mu);
  st->refs++, st->pending++;
  pthread_mutex_unlock(&st->mu);
  s->parent = st;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 8 << 20);  // the main thread's, as its first fiber runs on it
  pthread_t th;
  int rc = pthread_create(&th, &attr, isolate_main, s);
  pthread_attr_destroy(&attr);
  if (rc) {
    pthread_mutex_lock(&st->mu);
    st->pending--;
    pthread_mutex_unlock(&st->mu);
    release_spawn_state(st);
    free_spawn(s);
    bober_trap("cannot start isolate", file, at);
  }
  return (uint64_t)(uintptr_t)s;
}

int32_t bober_spawn_join(uint64_t handle, void *out, void *err, void *trap_out) {
  Spawn *s = (Spawn *)(uintptr_t)handle;
  SpawnState *st = s->parent;
  pthread_mutex_lock(&st->mu);
  while (!s->status) {
    s->waiter = bober_self();
    pthread_mutex_unlock(&st->mu);
    bober_park();
    pthread_mutex_lock(&st->mu);
  }
  s->waiter = 0;
  if (s->queued) {
    Spawn **pp = &st->done_head, *prev = 0;
    while (*pp && *pp != s) prev = *pp, pp = &(*pp)->next_done;
    if (*pp == s) {
      *pp = s->next_done;
      if (st->done_tail == s) st->done_tail = prev;
    }
    s->queued = false;
  }
  st->pending--;
  int status = s->status;
  pthread_mutex_unlock(&st->mu);
  if (status == 1) {
    *(bober_union *)out = bober_import(s->result.u, s->result.copy);
  } else if (status == 2) {
    SpawnThrown *e = err;
    e->u = bober_import(s->result.u, s->result.copy);
    e->file = s->err_file, e->at = s->err_at, e->show = s->err_show;
  } else {
    const char *m = s->trap_msg ? s->trap_msg : "trap";
    *(bober_string *)trap_out = make_string(m, (int64_t)strlen(m));
  }
  free_spawn(s);
  return status - 1;
}
