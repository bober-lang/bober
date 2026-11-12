//! The run time that compiled code and std link against: memory, the stack, the program and I/O. What has a body
//! expressible in bober is in builtin/, which imports this as a C header.

#include <stdint.h>

/// The operating system and CPU this is built for, as std/os names them.
#if defined(__APPLE__)
#define BOBER_OS "macos"
#elif defined(__FreeBSD__)
#define BOBER_OS "freebsd"
#elif defined(_WIN32)
#define BOBER_OS "windows"
#else
#define BOBER_OS "linux"
#endif
#if defined(__aarch64__)
#define BOBER_ARCH "arm64"
#else
#define BOBER_ARCH "x86_64"
#endif

/// Declares s and s_end, of type type[], the bounds of section s, which the linker defines: on Mach-O of section s of
/// segment __DATA, on ELF __start_s and __stop_s, weak for a program without s.
#if defined(__APPLE__)
#define BOBER_SECTION(type, s)                      \
  extern type s[] __asm("section$start$__DATA$" #s); \
  extern type s##_end[] __asm("section$end$__DATA$" #s)
#else
#define BOBER_SECTION(type, s)                                 \
  extern type s[] __asm("__start_" #s) __attribute__((weak)); \
  extern type s##_end[] __asm("__stop_" #s) __attribute__((weak))
#endif

// Memory, which the collector (src/gc.c) frees and moves.

/// Where the refs in a value are: its size, a multiple of 8, and a bit for each 8 bytes of it, set for a ref.
typedef struct { int64_t size; uint64_t refs[]; } bober_layout;
/// n new zero bytes, 16-aligned, of values laid out as l, or of none holding a ref if l is null: a ref, which may
/// point into them anywhere. It may collect, so that refs held across it move.
void *bober_alloc(int64_t n, const bober_layout *l);
/// The bytes of a page of the heap. An object of more has pages of its own, from one's start, and never moves; once it
/// is not reached, they are mapped anew, which unmaps what bober_io_map mapped there but its first.
#define BOBER_PAGE (1 << 16)
/// Marks the card of place p, where a ref may have been stored, as the write barrier of compiled code does.
void bober_written(void *p);
/// A number, from 1, that is p's alone while what p points into lives, however it moves; 0 for null.
uint64_t bober_id(void *p);
/// n bytes from C's malloc, which the collector neither moves nor frees, for C to hold; exits if out of memory.
void *bober_malloc(uint64_t n);
/// p, from bober_malloc, moved to n bytes as C's realloc does; exits if out of memory.
void *bober_realloc(void *p, uint64_t n);
/// Frees p, from bober_malloc; nothing if p is null.
void bober_free(void *p);

// Types (src/type.c).

/// A string as bober holds it, 24 bytes (builtin/string.bob): here a long one, its length after the 0xff of its first
/// byte and its bytes at the address of its second word, its ref null.
typedef struct { uint64_t head; const char *bytes; void *ref; } bober_string;
/// A field of a class, object or tuple type, or of the struct a value of one refers to: its type's mods as bc's
/// (Readonly 8, Embed 16) and its offset, which its type's completion sets.
typedef struct { bober_string name, doc; const struct bober_type *type; int64_t mods, offset, ntags; const struct bober_type **tags; } bober_field;
/// A method by name, called by a thunk of its receiver, args and result each as a union holds them (src/emit.bob).
typedef struct { bober_string name; void *fn; } bober_method;
/// A type's descriptor (§4.11), whose address is a union's tag. emit makes one for each type a module names, a weak
/// global listed in section bober_types, and bober_type_of those a generic's code forms, so that a type has one. Its
/// kind is Type.kind's index in builtin/type.bob; elem a fixed array's, array's or literal's; parts a union's members,
/// a function's params and result, or an instance's type args; a tuple's elements are its fields. How a value of it is
/// held (held, size, align and layout) and the struct one refers to (ssize, slayout and the fields' offsets) are
/// completed from those.
typedef struct bober_type {
  int64_t id;  // the TypeId of a primitive, else 0, as rt.c shows a thrown value by
  int64_t kind;
  bober_string key, name, doc, kindName;
  const struct bober_type *elem, *base;
  int64_t length;      // a fixed array's; a function's number of params; a bool or int literal's value; the byte a packed union's tag is in, -1 for none
  bober_string value;   // a string literal's
  int64_t nparts;
  const struct bober_type **parts;
  int64_t nfields;
  bober_field *fields;
  int64_t nmethods;
  const bober_method *methods;
  int64_t held;  // the TypeId of the primitive it is held as, or -1 a ref, -2 a union of any type, -3 in place (a fixed array), -4 a packed union (§4.4)
  int64_t size, align, ssize;
  const bober_layout *layout, *slayout;
  int64_t ntags;  // a class's or interface's (§5.3)
  const struct bober_type **tags;
  const struct bober_type *canon;  // the type a value of it is held and tagged as, its literals their base; emit's, or complete's of one formed
  uint64_t hash, head;  // src/bc/program.bob's: its hash, and that of its kind and data, which with its parts' makes it
} bober_type;
/// A value as a union holds it: the address of its type's descriptor, its bits, and its ref.
typedef struct { const struct bober_type *tag; uint64_t bits; void *ref; } bober_union;
/// Completes the descriptors emit made, which bober_start does first.
void bober_types_complete(void);
/// t with each of the n params in it, descriptors of kind 16 that emit makes of type params, as the arg at its index:
/// the type a generic's code forms of its type args.
const bober_type *bober_type_of(const bober_type *t, int64_t n, const bober_type *const *params, const bober_type *const *args);
/// Loads the value of type t at p into *u, but for one held in place, a string or fixed array: its size, which the
/// caller boxes, else 0.
int64_t bober_load(const bober_type *t, const void *p, bober_union *u);
/// Stores the value of type t that u holds at p.
void bober_store(const bober_type *t, void *p, const bober_union *u);
/// The zero value of type t (§5.1) into *u, a string or fixed array boxed; of none if t is null, as an Of type's before
/// its static local is set. It may collect.
void bober_zero(const bober_type *t, bober_union *u);
/// Whether the value u holds is of type t, as Is tests it: one of a union's members, or a literal's value.
int64_t bober_is(const bober_type *t, const bober_union *u);
/// The length of the array, fixed array or string u holds.
int64_t bober_length(const bober_union *u);
/// The place of element i of the array or fixed array u holds, its type in *type; traps out of range.
void *bober_element(const bober_union *u, int64_t i, const bober_type **type, const char *file, int32_t at);
/// The offset of field name of the struct a value of t refers to, its type in *type, or -1 if it has none.
int64_t bober_field_of(const bober_type *t, const char *name, int64_t n, const bober_type **type);
/// The thunk of t's method name, or null.
void *bober_method_of(const bober_type *t, const char *name, int64_t n);
/// Number ops on the bits of values as a union holds them, both of t: op is bc's opcode, its index among Eq through Shr.
uint64_t bober_number(int64_t op, const bober_type *t, uint64_t a, uint64_t b, const char *file, int32_t at);
/// The bits of a number of type from converted to type to, as Cast does.
uint64_t bober_convert(const bober_type *to, const bober_type *from, uint64_t bits);

// The stack. A function that can trap takes the module's source path and the byte offset in it last.

/// Prints what and, given a file, where, and exits, or in a test fails just that test.
_Noreturn void bober_trap(const char *what, const char *file, int32_t at);
/// Traps as an assert that failed, with how, a string, saying how.
_Noreturn void bober_fail(const uint8_t *how, const char *file, int32_t at);
/// Runs the function value f as the subtest name of the running test; a trap in it fails it and the test, which goes on.
void bober_subtest(const char *name, uint64_t f);
/// Notes that the running test is at the statement at its source position at, which a trap elsewhere says it was called
/// at.
void bober_mark(int32_t at);
/// Raises a value as a union holds it, its tag, bits and ref, noting where it was first thrown, a rethrow passing a null
/// file; returns if nothing catches it, for the thrower to call bober_uncaught.
void bober_throw(int64_t tag, int64_t bits, void *ref, const char *file, int32_t at);
/// Traps with the value nothing caught, as a union holds it, where it was first thrown: as shown, a string, if the
/// thrower made one, else by its primitive kind (§8.2), which its tag's descriptor holds.
_Noreturn void bober_uncaught(int64_t tag, int64_t bits, void *ref, const uint8_t *shown);

/// The program's argument i, 0 its name, null past the last.
const char *bober_arg(int32_t i);
/// The value of the environment variable name, null if it is unset.
const char *bober_env(const char *name);

// Fibers (src/fiber.c), each on a stack of its own, taking turns: one runs until it parks. builtin/fiber.bob builds
// futures on them.

/// Runs f, the address of a function value taking and giving nothing, on a new fiber at once; once that parks, the
/// caller runs again first.
void bober_fiber(uint64_t f);
/// The fiber running, as bober_wake takes it.
uint64_t bober_self(void);
/// Parks the fiber running until bober_wake readies it, running those ready meanwhile; traps if none is.
void bober_park(void);
/// Readies fiber f, which parked, to run after those readied before it, unless it is ready already.
void bober_wake(uint64_t f);

// What differs between runs is fixed in tests, so that a test's result depends on its code alone and can be cached.

/// The time in ns since the epoch, in tests a fixed one that does not advance.
int64_t bober_now(void);
/// The local time zone as POSIX's TZ gives it, 0 if TZ is unset; in tests "UTC".
const char *bober_tz(void);
/// 64 random bits, not for keys; in tests the same ones in each test.
uint64_t bober_random(void);
/// The time in ns since a fixed point, which nothing sets back or forward: for deadlines. In tests, bober_logical: it
/// starts at 1, as 0 is none to deadlines, and only a wait moves it, once no socket is ready, to the soonest deadline
/// or timer of their network (src/net.h); what another process sends is not waited for.
int64_t bober_mono(void);
/// The logical time in tests, -1 elsewhere.
extern int64_t bober_logical;

// I/O, through which std makes every system call, so that it needs no header of the system's: src/io.c translates
// each for where it runs, through its libc. A call gives a result >= 0 or a failure < 0, which only bober_io_kind
// reads. Only sockets wait: a call on one that would block gives BOBER_IO_AGAIN, and the caller parks until it is ready
// (bober_io_ready) and calls again. No call parks, so buffers may be in the heap, which the collector moves meanwhile.
// Paths, hosts, modes and nets are C strings; a timeout is in ns, 0 for none.

/// What a call on a socket gives that would block, below every failure.
#define BOBER_IO_AGAIN (-0x10000)
/// The bytes a host takes, an IP or a Unix socket's path, with its NUL.
#define BOBER_IO_HOST 128

/// The kind of IoError (std/io) failure r is.
const char *bober_io_kind(long r);
/// Parks until the socket fd is ready to read or, if write, to write, or is shut down or closed: 0, or a failure once
/// timeout passes. The kernel watches fd from its first wait until bober_io_close.
long bober_io_ready(int fd, int write, long timeout);
/// Parks for ns.
void bober_io_sleep(long ns);
/// Waits for the kernel to find a socket fibers wait on ready, or for their soonest deadline, and wakes those fibers;
/// what bober_park does once no fiber is ready. 0, at once, if none waits: every fiber is parked, which bober_park then
/// traps on.
int bober_io_wait(void);
/// Forgets the fibers waiting, before they are dropped.
void bober_io_cancel(void);
/// A descriptor of the file at path, CLOEXEC, opened as fopen's mode, "r", "w", "a" or "x", and "+", opens it.
long bober_io_open(const char *path, const char *mode);
/// Closes fd, waking what waits on it. One waited on closes only so: the kernel watches by number, which another takes.
long bober_io_close(int fd);
/// Up to n bytes into buf; 0 at the end.
long bober_io_read(int fd, void *buf, unsigned long n);
/// Maps the n bytes of fd from its start at at, a page's address, in place of what is there: privately and read-only.
long bober_io_map(int fd, void *at, unsigned long n);
/// Writes up to n bytes of buf, without SIGPIPE: how many.
long bober_io_write(int fd, const void *buf, unsigned long n);
long bober_io_sync(int fd);
long bober_io_mkdir(const char *path);
/// Removes a file or an empty directory.
long bober_io_remove(const char *path);
long bober_io_rename(const char *from, const char *to);
/// What the symlink at path points to, n bytes of it at most, in buf; its length.
long bober_io_readlink(const char *path, void *buf, unsigned long n);
/// The path of the running program's file, symlinks resolved, as a C string in buf, of PATH_MAX bytes; its length.
long bober_io_exe(char *buf);
/// What stat finds: err is 0 or a failure; times are in ns since the epoch, born 0 if unknown; kind is "file",
/// "directory", "symlink" or "other"; perm the permission bits.
typedef struct {
  long err, size, modified, accessed, born;
  const char *kind;
  unsigned perm;
} bober_stat;
/// Of path, not following a last symlink unless follow.
bober_stat bober_io_stat(const char *path, int follow);
/// The entries of the directory at path, without . and .., into the n bytes of buf, each its name, a NUL, its kind as
/// bober_stat has it and a NUL, if they fit: the bytes they take, which are more than n if they do not.
long bober_io_list(const char *path, char *buf, unsigned long n);
// Sockets, of a net, "tcp" for a stream or "udp" for datagrams, at a host and port: an IP, or a Unix socket's path,
// starting with /, and port 0. Each is non-blocking and CLOEXEC, a TCP one with TCP_NODELAY.

/// A socket connecting to host and port, until it is ready to write: its descriptor.
long bober_io_dial(const char *net, const char *host, int port);
/// 0 if fd, ready to write, connected, else the failure.
long bober_io_connected(int fd);
/// A socket bound to host and port, an IP's with SO_REUSEPORT unless port 0, and but for "udp" listening, an IP's
/// with SO_REUSEADDR: its descriptor.
long bober_io_listen(const char *net, const char *host, int port, int backlog);
/// A connection to the listening fd: its descriptor.
long bober_io_accept(int fd);
/// A datagram of up to n bytes into buf, its sender's host into host and port into *port: the bytes.
long bober_io_recvfrom(int fd, void *buf, unsigned long n, char *host, int *port);
/// Sends the n bytes of buf as a datagram to host and port: how many.
long bober_io_sendto(int fd, const void *buf, unsigned long n, const char *host, int port);
/// Ends fd's writes, or, if both, its reads too, waking what waits on fd; the peer reads an end.
long bober_io_shutdown(int fd, int both);
/// Closes the connected fd with a reset: the peer's calls fail with ConnectionReset.
long bober_io_reset(int fd);
/// 1 if the connected fd has nothing to read and its peer has not ended, else 0.
int bober_io_idle(int fd);
/// The host of fd, or of its peer, into host: its port.
long bober_io_name(int fd, int peer, char *host);
/// The bytes of the IP text is into ip: 4, 16, or 0 if text is none.
int bober_io_ip(const char *text, unsigned char *ip);
/// The text of the n, 4 or 16, bytes of an IP into text.
void bober_io_ip_text(const unsigned char *ip, int n, char *text);
/// Runs the program file, found in PATH unless it holds a /, with the null-ended argv and the process's environment,
/// its output and errors to the file out unless it is null, and waits for it, blocking every fiber: its exit status,
/// or 128 + the signal that ended it.
long bober_io_run(const char *file, char *const *argv, const char *out);
