//! The collector of what bober_alloc gives out: precise, finding the refs of frames by LLVM's stack maps, generational,
//! and moving what is young. The heap is pages of 64 KiB, each holding objects of one size (a BiBoP), so that where an
//! object starts follows from any pointer into it. An object starts young, bumped to in a page of its size; a
//! collection traces from the roots, moving each young object it reaches into an old page, and then frees the young
//! pages. An object of over 8 KiB has pages of its own and never moves: it is new until its first collection, which
//! marks it as an old one, and frees it if not reached. An object has no header: a bit for each word of the heap says
//! where refs are, set from its layout when allocated; a bit for each 16 bytes, at an object's start, whether it is
//! marked, or if young moved, its new place then in its first word. Objects without refs are in pages of their own,
//! which no collection scans, and strings' buffers in others of their own.
//!
//! Isolates (spawn) share the heap's address space, so that the write barrier stays a load of a global, but each page
//! is one isolate's, which alone allocates, promotes and collects in it; only taking or freeing a page takes page_mu.
//! Before bober_freeze, the consts are made in the heap of isolate 0, whose roots are the globals in section
//! bober_roots; bober_freeze collects it, makes each of its pages FROZEN, read-only and shared by all, and starts
//! isolate 1, the program's. Each isolate has a copy of its own of the lets, of section bober_lets.
//!
//! A minor collection, once 32 MiB are allocated, neither marks nor frees what is old: its roots are the isolate's
//! lets, the refs the stack maps say the frames of its fibers that ran since the last collection hold (src/fiber.c),
//! and the refs of its old objects on dirty cards. A card is 512 bytes of the heap, a byte after its 64
//! GiB, which the write barrier sets for each place a ref is stored to (src/backend/backend.bob); so an old object holds a ref
//! to a young or new one only where its card is dirty, and a fiber that has not run since a collection none. A major
//! collection, once the old objects take over 64 MiB and two to four times the bytes live after the last, the more the
//! less of what became old before the last had died by it, traces from every fiber and marks each old object it reaches
//! in place, freeing those left unmarked onto the free lists promotion takes from. Each cleans every card.
//! BOBER_GC_VERIFY=1 checks after each minor's trace that no old object or fiber left still holds a ref to a young
//! object, as one that a missed barrier hides would.

#include "rt.h"
#include <libunwind.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE BOBER_PAGE
#define PAGES (1 << 20)  // of address space, 64 GiB
#define CARD 9           // the bits of a card's size, as the write barrier has it
#define SMALL 8192       // the size of the largest object of a size class
#define CLASSES 36       // those of objects with refs; class c + CLASSES is c's size, of objects without, and
                         // c + 2 * CLASSES of strings' buffers

enum { FREE, YOUNG, OLD, BIG, REST, FROZEN };
/// A page: its kind, its size class if young or old, or if big that of class 0 of its family, the offset bumped to if
/// young or whether new if big, the pages of a big object if its first, or else how far back that is, and the isolate.
typedef struct { uint8_t kind, class; uint32_t used, pages, owner; } Page;

char *bober_heap;  // as the write barrier finds it, and the cards after it
#define heap bober_heap
static uint8_t *cards;
static uint64_t *refs;   // a bit for each word of the heap, set if it holds a ref
static uint64_t *marks;  // a bit for each 16 bytes, set at an object's start if marked, or moved
static Page pages[PAGES];
static uint32_t next[PAGES];  // of a first page, the next of its isolate's
static pthread_mutex_t page_mu = PTHREAD_MUTEX_INITIALIZER;  // of taking and freeing pages, and of the totals
static uint32_t top = 1;       // the pages ever used, from 1, so that 0 is no page
static uint32_t hint = 1;      // where the first free page may be
static uint32_t frozen_top;    // top once bober_freeze froze isolate 0's pages, 0 before
static uint32_t isolates = 1;  // the last isolate's id: 0 the consts', 1 the program's, then those spawned
static uint32_t sizes[3 * CLASSES], classOf[SMALL / 16 + 1];
static int64_t every = 32 << 20;  // how much allocated collects
static bool verify;
static int64_t allocs, allocated, collections, majors;  // in all, for BOBER_GC_STATS, allocs of an isolate once it ends

typedef struct { void *global; const bober_layout *layout; } Root;
BOBER_SECTION(const Root, bober_roots);
/// A let: where its offset in an isolate's lets goes, its size and layout, 32 bytes, as LLVM aligns each to 16.
typedef struct { int64_t *offset; int64_t size; const bober_layout *layout; int64_t pad; } Let;
BOBER_SECTION(const Let, bober_lets);
static int64_t lets_size;
/// bober_static's places, each after its key, descriptor, size, layout and set word, chained by key and descriptor.
typedef struct Static {
  struct Static *next;
  const void *key, *t;
  int64_t size;
  const bober_layout *layout;
  int64_t set;
} Static;
typedef struct { char *young, *old; } Id;

/// An isolate's heap: what it allocates, promotes and collects in, and the roots of it but its frames'.
typedef struct {
  uint32_t id, owned;            // the isolate's, and its first page, which links its others
  uint32_t bumped[3 * CLASSES];  // the young page of each class being bumped through
  char *spare[3 * CLASSES];      // the old slots of each class free, each linked through its first word
  int64_t reserved;  // what young may reach without a collection, as bober_reserve promised
  int64_t young;     // what has been allocated since the last collection
  int64_t olds, kept, major;  // the bytes of old objects, the dead since the last major too, those live after it, and
                              // how many make the next collection a major one
  bool minor, verifying;
  void **bottom;  // the frame pointer of the isolate's entry, where a walk of its first fiber's frames stops
  char **gray;
  int64_t grays, grayCap;
  Static *statics[1024];
  Id *ids;  // the young objects given an old slot, open addressed and at most half full
  uint64_t idMask, idCount;
  char *lets;                // the isolate's lets, at the offsets bober_gc_start gave them
  bober_union *extra_roots;  // the args a spawned isolate's entry is given, by bober_gc_set_roots
  int64_t nextra_roots, allocs;
} Heap;
static _Thread_local Heap h_;

/// A USDT probe bober:name of an int64_t, which perf and bpftrace can attach to: a nop until they do. Its ELF note says
/// where the nop is and in which register the value. perf finds the nop's file offset by that of .stapsdt.base, so
/// this is kept in the code's segment.
#ifdef __ELF__
__asm__(".pushsection .stapsdt.base, \"axR\", \"progbits\"\n_.stapsdt.base: .space 1\n.popsection");
#define PROBE(name, v)                                            \
  __asm__ volatile("990: nop\n"                                   \
                   ".pushsection .note.stapsdt, \"\", \"note\"\n" \
                   ".balign 4\n"                                  \
                   ".4byte 992f - 991f, 994f - 993f, 3\n"         \
                   "991: .asciz \"stapsdt\"\n"                    \
                   "992: .balign 4\n"                             \
                   "993: .8byte 990b, _.stapsdt.base, 0\n"        \
                   ".asciz \"bober\", \"" #name "\", \"-8@%0\"\n"  \
                   "994: .balign 4\n"                             \
                   ".popsection" ::"r"((int64_t)(v)))
#else
#define PROBE(name, v)
#endif

static char *at(uint32_t page) { return heap + (int64_t)page * PAGE; }
/// The index of the word at p in refs, and of the 16 bytes at p in marks.
static uint64_t word(char *p) { return (uint64_t)(p - heap) / 8; }
static uint64_t cell(char *p) { return (uint64_t)(p - heap) / 16; }
static bool bit(const uint64_t *map, uint64_t i) { return map[i / 64] >> i % 64 & 1; }
static void put(uint64_t *map, uint64_t i, bool b) { map[i / 64] = (map[i / 64] & ~(1ull << i % 64)) | (uint64_t)b << i % 64; }
/// The 64 bits of map from bit i on, and setting those of them set in b.
static uint64_t get64(const uint64_t *map, uint64_t i) {
  return map[i / 64] >> i % 64 | (i % 64 ? map[i / 64 + 1] << (64 - i % 64) : 0);
}
/// The next word only if some of b's bits go there: it may be of the next page, another isolate's.
static void or64(uint64_t *map, uint64_t i, uint64_t b) {
  map[i / 64] |= b << i % 64;
  if (i % 64 && b >> (64 - i % 64)) map[i / 64 + 1] |= b >> (64 - i % 64);
}

/// What BOBER_GC_STATS=1 prints at exit: the objects and bytes allocated, the collections, and the heap at its peak.
static void stats(void) {
  fprintf(stderr, "gc: %lld allocs, %lld MiB, %lld collections, %lld major, %lld MiB peak\n",
          (long long)(allocs + h_.allocs), (long long)(allocated + h_.young) >> 20, (long long)collections,
          (long long)majors, (long long)top * PAGE >> 20);
}

/// Starts the running thread's heap, of isolate id, whose entry's frame pointer is at.
static void begin(uint32_t id, void **at) {
  h_ = (Heap){.id = id, .major = 64 << 20, .bottom = at, .lets = calloc(1, lets_size + 1)};
  if (!h_.lets) fputs("out of memory\n", stderr), exit(1);
}

void bober_gc_isolate(void **at) {
  pthread_mutex_lock(&page_mu);
  uint32_t id = ++isolates;
  pthread_mutex_unlock(&page_mu);
  begin(id, at);
}

/// The 16 sizes up to 256 bytes, then 4 for each doubling, of up to SMALL; the heap's address space, its cards after,
/// and one past them that the barrier marks for a place outside the heap; and its bits: bober_start's first (src/rt.c),
/// with the frame pointer of the program's entry.
void bober_gc_start(void **at) {
  for (int c = 0; c < CLASSES; c++)
    sizes[c] = sizes[c + CLASSES] = sizes[c + 2 * CLASSES] =
      c < 16 ? 16 * (c + 1) : (64 << (c - 16) / 4) * (5 + (c - 16) % 4);
  for (int n = 0, c = 0; n <= SMALL / 16; n++) {
    while (16 * n > sizes[c]) c++;
    classOf[n] = c;
  }
  int64_t n = (int64_t)PAGES * PAGE;
  heap = mmap(0, n + (n >> CARD) + 8 + n / 64 + n / 128, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (heap == MAP_FAILED) fputs("out of memory\n", stderr), exit(1);
  // Above 4 GiB, so that a number below 2^32 a packed union holds in its refs' word is no ref (§4.4).
  if ((uintptr_t)heap >> 32 == 0) fputs("gc: the heap below 4 GiB\n", stderr), abort();
  cards = (uint8_t *)heap + n;
  refs = (uint64_t *)(cards + (n >> CARD) + 8), marks = refs + n / 512;
  for (const Let *l = bober_lets; l < bober_lets_end; l++)
    if (l->offset) *l->offset = lets_size, lets_size += (l->size + 15) & ~15;
  begin(0, at);
  const char *e = getenv("BOBER_GC_EVERY");
  if (e) every = atoll(e);
  verify = getenv("BOBER_GC_VERIFY");
  if (getenv("BOBER_GC_STATS")) atexit(stats);
}

char *bober_lets_base(void) { return h_.lets; }

int bober_frozen(void *p) {
  uintptr_t a = (uintptr_t)p - (uintptr_t)heap;
  return a < (uintptr_t)frozen_top * PAGE && pages[a / PAGE].kind == FROZEN;
}

void bober_written(void *p) {
  uint64_t card = (uint64_t)((char *)p - heap) >> CARD, spare = (uint64_t)PAGES * PAGE >> CARD;
  cards[card < spare ? card : spare] = 1;
}

/// n free pages in a row, of kind, their memory zero, the running isolate's.
static uint32_t take(int n, int kind) {
  pthread_mutex_lock(&page_mu);
  uint32_t p = top;
  for (uint32_t i = n == 1 ? hint : 1, run = 0; i < top; i++)
    if (pages[i].kind != FREE) run = 0;
    else if (++run == n) {
      p = i + 1 - n;
      break;
    }
  if (n == 1) hint = p + 1;
  if (p + n > PAGES) fputs("out of memory\n", stderr), exit(1);
  if (p + n > top) __atomic_store_n(&top, p + n, __ATOMIC_RELEASE);
  for (int i = 0; i < n; i++) pages[p + i] = (Page){i ? REST : kind, 0, 0, i ? i : n, h_.id};
  next[p] = h_.owned;
  pthread_mutex_unlock(&page_mu);
  return h_.owned = p;
}

/// Frees n pages from p, zeroing them and their bits: several by mapping them anew (src/rt.h). The caller takes them
/// off its isolate's.
static void release(uint32_t p, int n) {
  if (n > 1) mmap(at(p), (int64_t)n * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
  else memset(at(p), 0, PAGE);
  memset(&refs[word(at(p)) / 64], 0, (int64_t)n * PAGE / 64);
  memset(&marks[cell(at(p)) / 64], 0, (int64_t)n * PAGE / 128);
  pthread_mutex_lock(&page_mu);
  for (int i = 0; i < n; i++) pages[p + i].kind = FREE;
  if (p < hint) hint = p;
  pthread_mutex_unlock(&page_mu);
}

/// An old slot of class c, its bits 0, counted in olds, from its free list, which a new page refills.
static char *promoted(int c) {
  if (!h_.spare[c]) {
    uint32_t p = take(1, OLD);
    pages[p].class = c;
    for (char *h = at(p); h + sizes[c] <= at(p + 1); h += sizes[c]) *(char **)h = h_.spare[c], h_.spare[c] = h;
  }
  char *h = h_.spare[c];
  h_.spare[c] = *(char **)h, h_.olds += sizes[c];
  return h;
}

/// The start of the object p points into, and its page, or null if p is not into the running isolate's heap, as into
/// a frame, a constant, a frozen object, isolate 0's, or null. p may be at its object's end, but that is short of its
/// cell's: an array of no room is 24 bytes, and no struct is of none (src/emit.bob).
static char *object(void *p, Page **page) {
  uintptr_t a = (uintptr_t)p - (uintptr_t)heap;
  if (a >= (uintptr_t)__atomic_load_n(&top, __ATOMIC_RELAXED) * PAGE) return 0;
  uint32_t i = a / PAGE;
  if (pages[i].kind == REST) i -= pages[i].pages;
  Page *g = *page = &pages[i];
  if (g->owner != h_.id) return 0;
  return g->kind == BIG ? at(i) : at(i) + a % PAGE / sizes[g->class] * sizes[g->class];
}

// Ids. An object's id is where it is once old, which it then never leaves: an old or big one's own address, and a young
// one's the old slot taken for it when first asked, which the next collection moves it into. A slot whose object dies
// young is left unmarked, and so freed by the next major collection, which its bytes count toward as if it had moved.
// The id is that place's offset from the heap, the same wherever the heap is mapped.


/// The entry of young object h, or where it goes.
static Id *entry(char *h) {
  uint64_t i = (uintptr_t)h * 0x9e3779b97f4a7c15 >> 20;
  while (h_.ids[i & h_.idMask].young && h_.ids[i & h_.idMask].young != h) i++;
  return &h_.ids[i & h_.idMask];
}

/// The old slot young object h was given, taking one of class c if none.
static char *slot(char *h, int c) {
  if (2 * (h_.idCount + 1) > h_.idMask + 1) {
    Id *old = h_.ids;
    uint64_t m = h_.idMask;
    h_.idMask = old ? 2 * m + 1 : 63;
    if (!(h_.ids = calloc(h_.idMask + 1, sizeof *h_.ids))) fputs("out of memory\n", stderr), exit(1);
    for (uint64_t i = 0; old && i <= m; i++)
      if (old[i].young) *entry(old[i].young) = old[i];
    free(old);
  }
  Id *e = entry(h);
  if (!e->young) *e = (Id){h, promoted(c)}, h_.idCount++;
  return e->old;
}

/// The old slot young object h was given, or null.
static char *given(char *h) { return h_.idCount ? entry(h)->old : 0; }

uint64_t bober_id(void *p) {
  Page *g;
  char *h = object(p, &g);
  return (uintptr_t)(h && g->kind == YOUNG ? slot(h, g->class) + ((char *)p - h) : p) - (uintptr_t)heap;
}

// Tracing.

/// The place of what p points into once traced: a young object moved, an old one marked unless in a minor, where only
/// a new one is, each first reached with refs put on gray. p not into the heap stays.
static void *trace(void *p) {
  Page *g;
  char *h = object(p, &g);
  if (!h || g->kind != YOUNG && (h_.minor && !g->used || bit(marks, cell(h)))) return p;
  if (h_.verifying) fputs("gc: a young ref where a minor collection does not look\n", stderr), abort();
  intptr_t into = (char *)p - h;
  if (g->kind == YOUNG) {
    if (bit(marks, cell(h))) return *(char **)h + into;
    int64_t size = sizes[g->class];
    char *n = given(h);
    if (!n) n = promoted(g->class);
    memcpy(n, h, size);
    for (int64_t w = 0, k = size / 8; g->class < CLASSES && w < k; w += 64)
      or64(refs, word(n) + w, get64(refs, word(h) + w) & (k - w < 64 ? (1ull << (k - w)) - 1 : ~0ull));
    put(marks, cell(h), 1), *(char **)h = n;
    h = n;
  }
  if (!h_.minor || g->kind != YOUNG) put(marks, cell(h), 1);
  if (g->class < CLASSES) {
    if (h_.grays == h_.grayCap && !(h_.gray = realloc(h_.gray, (h_.grayCap = 2 * h_.grayCap + 1024) * sizeof *h_.gray)))
      abort();
    h_.gray[h_.grays++] = h;
  }
  return h + into;
}

/// trace of the ref in p's low 7 bytes, its top byte kept, where a packed union may keep its tag (§4.4).
static void *traced(void *p) {
  uintptr_t top = (uintptr_t)p >> 56 << 56;
  return (void *)((uintptr_t)trace((void *)((uintptr_t)p - top)) + top);
}

/// Traces the refs in the bytes at p, outside the heap, values laid out as l.
static void root(void **p, int64_t bytes, const bober_layout *l) {
  for (int64_t v = 0; l && v + l->size <= bytes; v += l->size)
    for (int64_t w = 0; w < l->size / 8; w += 64)
      for (uint64_t bits = l->refs[w / 64]; bits; bits &= bits - 1) {
        void **r = &p[v / 8 + w + __builtin_ctzll(bits)];
        *r = traced(*r);
      }
}

/// Traces the refs of the heap from lo to hi.
static void scan(char *lo, char *hi) {
  for (uint64_t i = word(lo), end = word(hi); i < end; i = (i | 63) + 1) {
    uint64_t bits = refs[i / 64] >> i % 64;
    if (end - i < 64) bits &= (1ull << (end - i)) - 1;
    for (; bits; bits &= bits - 1) {
      void **r = (void **)heap + i + __builtin_ctzll(bits);
      *r = traced(*r);
    }
  }
}

/// Traces the refs of object h.
static void values(char *h) {
  Page *g = &pages[(h - heap) / PAGE];
  scan(h, h + (g->kind == BIG ? (int64_t)g->pages * PAGE : sizes[g->class]));
}

/// Traces the refs of the running isolate's old objects on a dirty card, or on any card if all.
static void dirty(bool all) {
  for (uint32_t p = h_.owned; p; p = next[p]) {
    Page *g = &pages[p];
    if (g->kind == YOUNG || g->used) continue;  // a new big one is traced whole if reached
    for (char *lo = at(p), *hi = at(p + (g->kind == BIG ? g->pages : 1)); lo < hi; lo += 1 << CARD)
      if (all || cards[(lo - heap) >> CARD]) scan(lo, lo + (1 << CARD));
  }
}

// The stack maps (llvm.org/docs/StackMaps.html), version 3, each a header, functions, constants
// and records; a record is a call's return address, as an offset into its function, and where the refs live across
// it are. Those of a statepoint are its calling convention, flags and deopts, then a base and derived ref in pairs,
// then, uncounted, the frame's addresses among them, which do not move. Section bober_stackmaps lists them by address,
// as ELF has no bounds of section .llvm_stackmaps: each object's of LLVM, each function's of TPDE.

typedef struct { uint8_t type, reserved; uint16_t size, reg, reserved2; int32_t offset; } Location;
typedef struct { uint64_t id; uint32_t offset; uint16_t reserved, locations; Location at[]; } Record;

BOBER_SECTION(const uint8_t *const, bober_stackmaps);
#if defined(__APPLE__) || defined(_WIN32)
#define IMAGE 0
#else
/// The image's first byte, which src/link writes the stack maps' addresses as offsets from (Format.maps); 0 where lld
/// links, as it rebases them.
extern const char bober_image[] __attribute__((weak));
#define IMAGE ((uintptr_t)bober_image)
#endif

/// Calls visit with the frame pointer of where each fiber but the running one parked: those that ran since the last
/// collection, or, if all, every one (src/fiber.c).
void bober_parked(void (*visit)(void **), int all);

/// A call's record and its frame's size, and how far below its frame pointer its stack pointer is, 0 until known, as
/// never so for a frame with refs spilled; by return address, open addressed, half full.
static struct Call { uintptr_t ret; const Record *r; int32_t size, below; } *calls;
static uint64_t mask;
static pthread_once_t indexed = PTHREAD_ONCE_INIT;

/// The entry of return address ret, or where it goes.
static struct Call *call(uintptr_t ret) {
  uint64_t i = ret * 0x9e3779b97f4a7c15 >> 20;
  while (calls[i & mask].ret && calls[i & mask].ret != ret) i++;
  return &calls[i & mask];
}

static const uint8_t *align8(const uint8_t *p) { return (const uint8_t *)(((uintptr_t)p + 7) & ~7); }

/// Indexes the records of the stack maps, in two passes, the first counting them.
static void records(void) {
  uint64_t n = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (const uint8_t *const *m = bober_stackmaps; m < bober_stackmaps_end; m++) {
      const uint8_t *p = *m;
      if (!p) continue;
      uint32_t fns = *(uint32_t *)(p + 4), consts = *(uint32_t *)(p + 8);
      const uint64_t *fn = (const uint64_t *)(p + 16);
      n += *(uint32_t *)(p + 12);
      p += 16 + 24 * fns + 8 * consts;
      for (uint32_t f = 0; f < fns; f++)
        for (uint64_t k = 0; k < fn[3 * f + 2]; k++) {
          const Record *r = (const Record *)p;
          uintptr_t ret = IMAGE + fn[3 * f] + r->offset;
          if (pass) *call(ret) = (struct Call){ret, r, fn[3 * f + 1]};
          p = align8((const uint8_t *)&r->at[r->locations]);
          p = align8(p + 4 + 4 * *(uint16_t *)(p + 2));
        }
    }
    if (!pass) {
      for (mask = 1; mask < 2 * n; mask *= 2) {}
      calls = calloc(mask--, sizeof *calls);
    }
  }
}

/// The DWARF numbers of the frame and the stack pointer, by which the stack maps place the refs of a frame: TPDE's by
/// the frame pointer, which every function keeps (src/backend/llvm.bob), LLVM's by the stack pointer.
#if defined(__aarch64__)
#define FP 29
#define SP 31
#else
#define FP 6
#define SP 7
#endif

/// The stack pointer at call c, whose callee's frame pointer is fp: on x86 just past the return address; on arm64 the
/// frame's size below its top, which libunwind finds once for each call.
static char *sp(struct Call *c, void **fp) {
#if defined(__aarch64__)
  if (!c->below) {
    unw_context_t ctx;
    unw_cursor_t u;
    unw_word_t top;
    unw_proc_info_t info;
    unw_getcontext(&ctx), unw_init_local(&u, &ctx);
    unw_set_reg(&u, FP, (uintptr_t)*fp), unw_set_reg(&u, UNW_REG_IP, c->ret - 1);  // in the call, which may end it
    // The step to a caller libunwind does not know, as main's in the C library, ends the stack (0) but is taken; this
    // frame it must know.
    if (unw_get_proc_info(&u, &info) || unw_step(&u) < 0)
      fputs("collect: a frame libunwind does not know\n", stderr), abort();
    unw_get_reg(&u, UNW_REG_SP, &top);
    c->below = (char *)*fp - ((char *)top - c->size);
  }
  return (char *)*fp - c->below;
#else
  return (char *)(fp + 2);
#endif
}

/// The address of a ref spilled in the frame at call c, whose callee's frame pointer is fp, or null for a constant or a
/// frame's address, which do not move.
static void **spilled(struct Call *c, void **fp, const Location *l) {
  if (l->type == 1 || (l->type == 3 && l->reg != FP && l->reg != SP))
    fputs("collect: a ref in a register\n", stderr), abort();
  if (l->type != 3) return 0;
  return (void **)((l->reg == FP ? (char *)*fp : sp(c, fp)) + l->offset);
}

/// Traces the refs each frame of a stack holds across its call, each derived one moved with its base, a vector of them,
/// as the SLP vectorizer makes, by each 8 bytes. From the frame of frame pointer fp, each frame's caller's is where its
/// own points, with the return address into it after, up to the program's entry or a fiber's first frame, whose
/// caller's is 0.
static void frames(void **fp) {
  for (; fp && fp != h_.bottom; fp = *fp) {
    struct Call *c = call((uintptr_t)fp[1]);
    const Record *r = c->r;
    if (!r) continue;
    int from = 3 + r->at[2].offset, n = 0, m = 0;
    for (int i = from; i < r->locations; i++) n += r->at[i].size / 8;
    void **slots[n], *moved[n];
    for (int i = from; i + 1 < r->locations; i += 2) {
      void **base = spilled(c, fp, &r->at[i]), **derived = base ? spilled(c, fp, &r->at[i + 1]) : 0;
      for (int k = 0; base && k < r->at[i].size / 8; k++) {
        slots[m] = &base[k], moved[m] = traced(base[k]), m++;
        if (derived) slots[m] = &derived[k], moved[m] = (char *)moved[m - 1] + ((char *)derived[k] - (char *)base[k]), m++;
      }
    }
    while (m--) *slots[m] = moved[m];
  }
}

/// Traces from the running isolate's roots, then frees its young pages, the new objects not reached and, in a major,
/// the old ones; freezing, only from bober_roots. Probes bober:gc_start with the bytes allocated since the last
/// collection, and bober:gc_end with those old.
static void collect(bool freezing) {
  PROBE(gc_start, h_.young);
  pthread_once(&indexed, records);
  h_.minor = !freezing && h_.olds <= h_.major;
  memset(h_.bumped, 0, sizeof h_.bumped);
  for (const Root *g = bober_roots; !frozen_top && g < bober_roots_end; g++)
    if (g->global) root(g->global, g->layout->size, g->layout);
  if (!freezing) {
    frames(__builtin_frame_address(0));
    bober_parked(frames, !h_.minor);
    for (const Let *l = bober_lets; l < bober_lets_end; l++)
      if (l->offset) root((void **)(h_.lets + *l->offset), l->size, l->layout);
    for (int i = 0; i < 1024; i++)
      for (Static *s = h_.statics[i]; s; s = s->next) root((void **)(s + 1), s->size, s->layout);
    for (int64_t i = 0; i < h_.nextra_roots; i++) h_.extra_roots[i].ref = traced(h_.extra_roots[i].ref);
  }
  if (h_.minor) dirty(false);
  while (h_.grays) values(h_.gray[--h_.grays]);
  if (h_.minor && verify) h_.verifying = true, bober_parked(frames, true), dirty(true), h_.verifying = false;
  free(h_.ids), h_.ids = 0, h_.idMask = h_.idCount = 0;
  if (!h_.minor) memset(h_.spare, 0, sizeof h_.spare);
  int64_t live = 0;
  for (uint32_t *pp = &h_.owned, p; (p = *pp);) {
    Page *g = &pages[p];
    if (g->kind == YOUNG) {
      *pp = next[p], release(p, 1);
      continue;
    } else if (h_.minor && !(g->kind == BIG && g->used)) {
    } else if (g->kind == BIG) {
      int64_t size = (int64_t)g->pages * PAGE;
      if (!bit(marks, cell(at(p)))) {
        *pp = next[p], release(p, g->pages);
        continue;
      }
      put(marks, cell(at(p)), 0), live += size, h_.olds += g->used ? size : 0, g->used = 0;
    } else if (g->kind == OLD) {
      int64_t n = 0, size = sizes[g->class];
      for (char *h = at(p); h + size <= at(p + 1); h += size) n += bit(marks, cell(h));
      if (!n) {
        *pp = next[p], release(p, 1);
        continue;
      }
      live += n * size;
      for (char *h = at(p); h + size <= at(p + 1); h += size)
        if (bit(marks, cell(h))) put(marks, cell(h), 0);
        else {
          for (int64_t w = 0; g->class < CLASSES && w < size / 8; w++) put(refs, word(h) + w, 0);
          *(char **)h = h_.spare[g->class], h_.spare[g->class] = h;
        }
    }
    memset(cards + ((int64_t)p * PAGE >> CARD), 0, (int64_t)(g->kind == BIG ? g->pages : 1) * PAGE >> CARD);
    pp = &next[p];
  }
  if (!h_.minor) {
    // The next major once what becomes old, dying as what became old since the last did, leaves as many bytes dead as
    // live: twice live if all of it died, at most four times if little did.
    double dead = h_.olds - live, since = h_.olds - h_.kept;
    int64_t more = dead * 3 <= since ? 3 * live : dead >= since ? live : live * since / dead;
    h_.kept = h_.olds = live, h_.major = live + more > 64 << 20 ? live + more : 64 << 20;
  }
  pthread_mutex_lock(&page_mu);
  allocated += h_.young, collections++, majors += !h_.minor;
  pthread_mutex_unlock(&page_mu);
  h_.young = h_.reserved = 0;
  PROBE(gc_end, h_.olds);
}

/// Frees what the running isolate's heap holds but its pages, counting its allocations in all.
static void drop(void) {
  free(h_.gray), free(h_.lets), free(h_.ids);
  for (int i = 0; i < 1024; i++)
    for (Static *s = h_.statics[i], *n; s; s = n) n = s->next, free(s);
  pthread_mutex_lock(&page_mu);
  allocs += h_.allocs, allocated += h_.young;
  pthread_mutex_unlock(&page_mu);
}

void bober_freeze(void) {
  collect(true);
  for (uint32_t p = h_.owned; p; p = next[p]) {
    int n = pages[p].kind == BIG ? pages[p].pages : 1;
    int64_t size = pages[p].kind == BIG ? n * PAGE : sizes[pages[p].class];
    for (char *h = at(p); pages[p].class >= 2 * CLASSES && h + size <= at(p + n); h += size)
      ((int64_t *)h)[1] = -1;  // a string's buffer, used up to past its end, so that none appends to it
    for (int i = 0; i < n; i++) pages[p + i].kind = FROZEN;
    mprotect(at(p), (int64_t)n * PAGE, PROT_READ);
  }
  frozen_top = top;
  void **bottom = h_.bottom;
  drop(), begin(1, bottom);
}

/// Frees the running isolate's heap, as it ends.
void bober_gc_finish(void) {
  for (uint32_t p = h_.owned, n; p; p = n) n = next[p], release(p, pages[p].kind == BIG ? pages[p].pages : 1);
  drop();
}

void *bober_static(const void *key, const void *t, int64_t size, const bober_layout *l) {
  Static **at = &h_.statics[((uintptr_t)key ^ (uintptr_t)t) * 0x9e3779b97f4a7c15 >> 54];
  for (Static *s = *at; s; s = s->next)
    if (s->key == key && s->t == t) return s + 1;
  Static *s = bober_malloc(sizeof *s + size);
  *s = (Static){*at, key, t, size, l, 0}, *at = s;
  return memset(s + 1, 0, size);
}

/// Sets the bits of the refs of the n bytes at p, values laid out as l, of which n is a multiple; if l's low bit is set,
/// an array's cell: its length, capacity and data, a ref, then its elements, which push zeroes once it moves them
/// (builtin/array.bob).
__attribute__((noinline)) static void lay(char *p, int64_t n, const bober_layout *l) {
  if ((uintptr_t)l & 1) put(refs, word(p) + 2, 1), p += 24, n -= 24, l = (const bober_layout *)((uintptr_t)l - 1);
  if (!l) return;
  uint64_t k = l->size / 8, i = word(p), end = word(p + n);  // the words of a value, and where they are
  if (k > 64) {
    for (; i < end; i += k)
      for (uint64_t w = 0; w < k; w += 64) or64(refs, i + w, l->refs[w / 64]);
    return;
  }
  // Each word of bits made in a register of the values that overlap it, which start every k bits from i.
  for (uint64_t w = i / 64 * 64; w < end; w += 64) {
    uint64_t bits = 0;
    for (uint64_t s = w > i ? w - (w - i) % k : i; s < w + 64 && s < end; s += k)
      bits |= s >= w ? l->refs[0] << (s - w) : l->refs[0] >> (w - s);
    refs[w / 64] |= bits;
  }
}

/// What bober_alloc does but bump: collects, and takes a page or a big object's pages, new.
__attribute__((noinline, cold)) static void *slow(int64_t n, int64_t size, int c, const bober_layout *l) {
  if ((h_.young += size) > every && h_.young > h_.reserved) collect(false);
  char *h;
  if (size > SMALL) {
    uint32_t p = take((size + PAGE - 1) / PAGE, BIG);
    pages[p].used = 1, pages[p].class = c, h = at(p);
  } else {
    Page *g = &pages[h_.bumped[c]];
    if (!h_.bumped[c] || g->used + sizes[c] > PAGE) {
      h_.bumped[c] = take(1, YOUNG);
      g = &pages[h_.bumped[c]];
      g->class = c;
    }
    h = at(h_.bumped[c]) + g->used;
    g->used += sizes[c];
  }
  if (l) lay(h, n, l);
  return h;
}

/// Collects now if allocating n more bytes would, so that slow does not while they are allocated.
void bober_reserve(int64_t n) {
  if (h_.young + n > every) collect(false);
  h_.reserved = h_.young + n;
}

/// Bumps through the young page of class c of the size, which no page yet is before the first allocation.
static inline void *bump(int64_t n, int c, const bober_layout *l) {
  int64_t size = (n + 15) & ~15;
  h_.allocs++;
  PROBE(alloc, n);
  c += size <= SMALL ? classOf[size / 16] : 0;
  Page *g = &pages[h_.bumped[c]];
  if (__builtin_expect(size > SMALL || h_.young + size > every || !h_.bumped[c] || g->used + sizes[c] > PAGE, 0))
    return slow(n, size, c, l);
  h_.young += size;
  char *h = at(h_.bumped[c]) + g->used;
  g->used += sizes[c];
  if (l && !((uintptr_t)l & 1) && n == l->size && n <= 512) or64(refs, word(h), l->refs[0]);
  else if (l) lay(h, n, l);
  return h;
}

void *bober_alloc(int64_t n, const bober_layout *l) { return bump(n, l ? 0 : CLASSES, l); }

void *bober_strbuf(int64_t n) { return bump(n, 2 * CLASSES, 0); }

void bober_gc_set_roots(bober_union *roots, int64_t n) { h_.extra_roots = roots, h_.nextra_roots = n; }

// Isolates' values (spawn, §6.10): a value crosses as a copy of its object's bytes, of which a ref may be only to what
// no isolate writes, as a frozen object or a constant, to the object itself, a string's to its buffer, whose bytes go
// with it, or an array's to the block of its elements, copied with it but no further.

/// A ref of a copy to fix once imported: at at, to off into the object if bytes and sub are null, else to a new buffer
/// of the len bytes, or to the copy sub of another object; the tag its top byte.
typedef struct { int64_t at, off, len; uint8_t tag; char *bytes; struct bober_transfer *sub; } Fix;
typedef struct bober_transfer {
  const bober_layout *layout;
  bober_layout *made;  // the layout made of the object's bits, if no hint gave one
  int64_t size, total;  // total: what it allocates once imported, those it holds too
  char *data;
  Fix *fixes;
  int64_t nfixes;
} bober_transfer;

void bober_transfer_free(bober_transfer *t) {
  if (!t) return;
  for (int64_t i = 0; i < t->nfixes; i++) free(t->fixes[i].bytes), bober_transfer_free(t->fixes[i].sub);
  free(t->fixes), free(t->data), free(t->made), free(t);
}

static bober_transfer *whole(char *h, Page *g, int nested);

/// A copy of the size bytes at p, values laid out as l, or null if a ref of them may not cross; nested if the copy
/// of an object another holds, whose refs may not be to others.
static bober_transfer *copy(char *p, int64_t size, const bober_layout *l, bober_layout *made, int nested) {
  bober_transfer *t = bober_malloc(sizeof *t);
  *t = (bober_transfer){l, made, size, (size + 15) & ~15, bober_malloc(size + 1)};
  uint64_t *w = memcpy(t->data, p, size);
  for (int64_t k = 0; l && k < size / 8; k += 64)
    for (uint64_t bits = l->refs[k / 64]; bits; bits &= bits - 1) {
      int64_t i = k + __builtin_ctzll(bits);
      uint8_t tag = w[i] >> 56;
      char *r = (char *)(w[i] << 8 >> 8);
      Page *g;
      char *h = object(r, &g);
      Fix f = {i * 8, r - p, 0, tag};
      if (!h) continue;
      if (r >= p && r < p + size) {
      } else if (g->class >= 2 * CLASSES && i >= 2 && (w[i - 2] & 0xff) == 0xff) {
        f.len = w[i - 2] >> 8, f.bytes = bober_malloc(f.len + 1), memcpy(f.bytes, r + w[i - 1], f.len);
        t->total += (16 + f.len + 15) & ~15;
      } else if (!nested && h == r && g->class < 2 * CLASSES && (f.sub = whole(h, g, 1))) {
        t->total += f.sub->total;
      } else {
        bober_transfer_free(t);
        return 0;
      }
      t->fixes = bober_realloc(t->fixes, (t->nfixes + 1) * sizeof *t->fixes);
      t->fixes[t->nfixes++] = f, w[i] = 0;
    }
  return t;
}

/// A copy of object h of page g, laid out as the collector's bits of it say.
static bober_transfer *whole(char *h, Page *g, int nested) {
  int64_t size = g->kind == BIG ? (int64_t)g->pages * PAGE : sizes[g->class];
  bober_layout *made = 0;
  if (g->class < CLASSES) {
    made = calloc(1, sizeof *made + (size / 512 + 1) * 8);
    made->size = size;
    for (int64_t k = 0; k < size / 8; k += 64)
      made->refs[k / 64] = get64(refs, word(h) + k) & (size / 8 - k < 64 ? (1ull << (size / 8 - k)) - 1 : ~0ull);
  }
  return copy(h, size, made, made, nested);
}

/// Whether u may cross, a copy of it made in out: hint_size bytes of it laid out as hint_layout, else its object, -2 if an
/// array's (src/emit.bob).
int bober_export(bober_union u, const bober_layout *hint_layout, int64_t hint_size, bober_transfer **out) {
  Page *g;
  char *h = object(u.ref, &g);
  *out = 0;
  if (!h) return 1;
  if (hint_size >= 0) return !!(*out = copy(u.ref, hint_size, hint_layout, 0, 1));
  if (h != u.ref || g->class >= 2 * CLASSES) return 0;
  return !!(*out = whole(h, g, hint_size != -2));
}

/// The object t is a copy of, and those it holds, allocated new, as bober_import reserved.
static char *placed(const bober_transfer *t) {
  char *o = bober_alloc(t->size, t->layout);
  memcpy(o, t->data, t->size);
  for (int64_t i = 0; i < t->nfixes; i++) {
    const Fix *f = &t->fixes[i];
    char *r = o + f->off;
    if (f->sub) r = placed(f->sub);
    if (f->bytes) {
      r = bober_strbuf(16 + f->len);
      ((int64_t *)r)[0] = ((int64_t *)r)[1] = f->len;
      memcpy(r + 16, f->bytes, f->len);
      *(int64_t *)(o + f->at - 8) = 16;
    }
    *(uintptr_t *)(o + f->at) = (uintptr_t)r | (uintptr_t)f->tag << 56;
    bober_written(o + f->at);
  }
  return o;
}

bober_union bober_import(bober_union u, const bober_transfer *t) {
  if (t) bober_reserve(t->total), u.ref = placed(t);
  return u;
}
