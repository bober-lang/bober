//! The collector of what bober_alloc gives out: precise, finding the refs of frames by LLVM's stack maps, generational,
//! and moving what is young. The heap is pages of 64 KiB, each holding objects of one size (a BiBoP), so that where an
//! object starts follows from any pointer into it. An object starts young, bumped to in a page of its size; a
//! collection traces from the roots, moving each young object it reaches into an old page, and then frees the young
//! pages. An object of over 8 KiB has pages of its own and never moves: it is new until its first collection, which
//! marks it as an old one, and frees it if not reached. An object has no header: a bit for each word of the heap says
//! where refs are, set from its layout when allocated; a bit for each 16 bytes, at an object's start, whether it is
//! marked, or if young moved, its new place then in its first word. Objects without refs are in pages of their own,
//! which no collection scans.
//!
//! A minor collection, once 32 MiB are allocated, neither marks nor frees what is old: its roots are the globals in
//! section bober_roots, the refs the stack maps say the frames of the fibers that ran since the last collection hold
//! (src/fiber.c), and the refs of old objects on dirty cards. A card is 512 bytes of the heap, a byte after its 64
//! GiB, which the write barrier sets for each place a ref is stored to (src/backend/backend.bob); so an old object holds a ref
//! to a young or new one only where its card is dirty, and a fiber that has not run since a collection none. A major
//! collection, once the old objects take twice the bytes live after the last, and over 64 MiB, traces from every fiber
//! and marks each old object it reaches in place, freeing those left unmarked onto the free lists promotion takes
//! from. Each cleans every card. BOBER_GC_VERIFY=1 checks after each minor's trace that no old object or fiber left
//! still holds a ref to a young object, as one that a missed barrier hides would.

#include "rt.h"
#include <libunwind.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE BOBER_PAGE
#define PAGES (1 << 20)  // of address space, 64 GiB
#define CARD 9           // the bits of a card's size, as the write barrier has it
#define SMALL 8192       // the size of the largest object of a size class
#define CLASSES 36       // those of objects with refs; class c + CLASSES is c's size, of objects without

enum { FREE, YOUNG, OLD, BIG, REST };
/// A page: its kind, its size class if young or old, or if big whether with refs, the offset bumped to if young or
/// whether new if big, and the pages of a big object if its first, or else how far back that is.
typedef struct { uint8_t kind, class; uint32_t used, pages; } Page;

char *bober_heap;  // as the write barrier finds it, and the cards after it
#define heap bober_heap
static uint8_t *cards;
static uint64_t *refs;   // a bit for each word of the heap, set if it holds a ref
static uint64_t *marks;  // a bit for each 16 bytes, set at an object's start if marked, or moved
static Page pages[PAGES];
static uint32_t top = 1;   // the pages ever used, from 1, so that 0 is no page
static uint32_t hint = 1;  // where the first free page may be
static uint32_t sizes[2 * CLASSES], classOf[SMALL / 16 + 1];
static uint32_t bumped[2 * CLASSES];  // the young page of each class being bumped through
static char *spare[2 * CLASSES];      // the old slots of each class free, each linked through its first word
static int64_t young, every = 32 << 20;  // what has been allocated since the last collection, and how much collects
static int64_t olds, major = 64 << 20;   // the bytes of old objects, the dead since the last major too, and how many
                                         // make the next collection a major one
static bool minor, verify, verifying;
static int64_t allocs, allocated, collections, majors;  // in all, for BOBER_GC_STATS

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
static void or64(uint64_t *map, uint64_t i, uint64_t b) {
  map[i / 64] |= b << i % 64;
  if (i % 64) map[i / 64 + 1] |= b >> (64 - i % 64);
}

/// What BOBER_GC_STATS=1 prints at exit: the objects and bytes allocated, the collections, and the heap at its peak.
static void stats(void) {
  fprintf(stderr, "gc: %lld allocs, %lld MiB, %lld collections, %lld major, %lld MiB peak\n", (long long)allocs,
          (long long)(allocated + young) >> 20, (long long)collections, (long long)majors, (long long)top * PAGE >> 20);
}

/// The 16 sizes up to 256 bytes, then 4 for each doubling, of up to SMALL; the heap's address space, its cards after,
/// and one past them that the barrier marks for a place outside the heap; and its bits: bober_start's first (src/rt.c).
void bober_gc_start(void) {
  for (int c = 0; c < CLASSES; c++)
    sizes[c] = sizes[c + CLASSES] = c < 16 ? 16 * (c + 1) : (64 << (c - 16) / 4) * (5 + (c - 16) % 4);
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
  const char *e = getenv("BOBER_GC_EVERY");
  if (e) every = atoll(e);
  verify = getenv("BOBER_GC_VERIFY");
  if (getenv("BOBER_GC_STATS")) atexit(stats);
}

void bober_written(void *p) {
  uint64_t card = (uint64_t)((char *)p - heap) >> CARD, spare = (uint64_t)PAGES * PAGE >> CARD;
  cards[card < spare ? card : spare] = 1;
}

/// n free pages in a row, of kind, their memory zero.
static uint32_t take(int n, int kind) {
  uint32_t p = top;
  for (uint32_t i = n == 1 ? hint : 1, run = 0; i < top; i++)
    if (pages[i].kind != FREE) run = 0;
    else if (++run == n) {
      p = i + 1 - n;
      break;
    }
  if (n == 1) hint = p + 1;
  if (p + n > PAGES) fputs("out of memory\n", stderr), exit(1);
  if (p + n > top) top = p + n;
  for (int i = 0; i < n; i++) pages[p + i] = (Page){i ? REST : kind, 0, 0, i ? i : n};
  return p;
}

/// Frees n pages from p, zeroing them and their bits: several by mapping them anew (src/rt.h).
static void release(uint32_t p, int n) {
  if (n > 1) mmap(at(p), (int64_t)n * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
  else memset(at(p), 0, PAGE);
  memset(&refs[word(at(p)) / 64], 0, (int64_t)n * PAGE / 64);
  memset(&marks[cell(at(p)) / 64], 0, (int64_t)n * PAGE / 128);
  for (int i = 0; i < n; i++) pages[p + i].kind = FREE;
  if (p < hint) hint = p;
}

/// An old slot of class c, its bits 0, from its free list, which a new page refills.
static char *promoted(int c) {
  if (!spare[c]) {
    uint32_t p = take(1, OLD);
    pages[p].class = c;
    for (char *h = at(p); h + sizes[c] <= at(p + 1); h += sizes[c]) *(char **)h = spare[c], spare[c] = h;
  }
  char *h = spare[c];
  spare[c] = *(char **)h;
  return h;
}

// Tracing.

static char **gray;
static int64_t grays, grayCap;

/// The start of the object p points into, and its page, or null if p is not into the heap, as into a frame, a
/// constant or null. p may be at its object's end, but that is short of its cell's: an array of no room is 24 bytes,
/// and no struct is of none (src/emit.bob).
static char *object(void *p, Page **page) {
  uintptr_t a = (uintptr_t)p - (uintptr_t)heap;
  if (a >= (uintptr_t)top * PAGE) return 0;
  uint32_t i = a / PAGE;
  if (pages[i].kind == REST) i -= pages[i].pages;
  Page *g = *page = &pages[i];
  return g->kind == BIG ? at(i) : at(i) + a % PAGE / sizes[g->class] * sizes[g->class];
}

/// The place of what p points into once traced: a young object moved, an old one marked unless in a minor, where only
/// a new one is, each first reached with refs put on gray. p not into the heap stays.
static void *trace(void *p) {
  Page *g;
  char *h = object(p, &g);
  if (!h || g->kind != YOUNG && (minor && !g->used || bit(marks, cell(h)))) return p;
  if (verifying) fputs("gc: a young ref where a minor collection does not look\n", stderr), abort();
  intptr_t into = (char *)p - h;
  if (g->kind == YOUNG) {
    if (bit(marks, cell(h))) return *(char **)h + into;
    int64_t size = sizes[g->class];
    char *n = promoted(g->class);
    memcpy(n, h, size);
    for (int64_t w = 0, k = size / 8; g->class < CLASSES && w < k; w += 64)
      or64(refs, word(n) + w, get64(refs, word(h) + w) & (k - w < 64 ? (1ull << (k - w)) - 1 : ~0ull));
    put(marks, cell(h), 1), *(char **)h = n;
    h = n, olds += size;
  }
  if (!minor || g->kind != YOUNG) put(marks, cell(h), 1);
  if (g->class < CLASSES) {
    if (grays == grayCap && !(gray = realloc(gray, (grayCap = 2 * grayCap + 1024) * sizeof *gray))) abort();
    gray[grays++] = h;
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

/// Traces the refs of old objects on a dirty card, or on any card if all.
static void dirty(bool all) {
  for (uint32_t p = 1; p < top; p++) {
    Page *g = &pages[p];
    uint32_t first = g->kind == REST ? p - g->pages : p;
    if (g->kind == FREE || g->kind == YOUNG || pages[first].used) continue;  // a new one is traced whole if reached
    for (char *lo = at(p); lo < at(p + 1); lo += 1 << CARD)
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
/// Calls visit with where each fiber but the running one parked: those that ran since the last collection, or, if
/// all, every one (src/fiber.c).
void bober_parked(void (*visit)(unw_context_t *), int all);
typedef struct { void *global; const bober_layout *layout; } Root;
BOBER_SECTION(const Root, bober_roots);
/// bober_static's places, each after its key, descriptor, size, layout and set word, chained by key and descriptor.
typedef struct Static {
  struct Static *next;
  const void *key, *t;
  int64_t size;
  const bober_layout *layout;
  int64_t set;
} Static;
static Static *statics[1024];

static struct Call { uintptr_t ret; const Record *r; } *calls;  // by return address, open addressed, half full
static uint64_t mask;

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
      uint32_t fns = *(uint32_t *)(p + 4), consts = *(uint32_t *)(p + 8);
      const uint64_t *fn = (const uint64_t *)(p + 16);
      n += *(uint32_t *)(p + 12);
      p += 16 + 24 * fns + 8 * consts;
      for (uint32_t f = 0; f < fns; f++)
        for (uint64_t k = 0; k < fn[3 * f + 2]; k++) {
          const Record *r = (const Record *)p;
          if (pass) *call(fn[3 * f] + r->offset) = (struct Call){fn[3 * f] + r->offset, r};
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

/// The address of a ref spilled in the frame c is at, or null for a constant or a frame's address, which do not move.
static void **spilled(unw_cursor_t *c, const Location *l) {
  unw_word_t r;
  if (l->type == 1) fputs("collect: a ref in a register\n", stderr), abort();
  if (l->type != 3) return 0;
  unw_get_reg(c, l->reg, &r);
  return (void **)(r + l->offset);
}

/// Traces the refs each frame of a stack holds across its call, from where ctx is, each derived one moved with its
/// base, a vector of them, as the SLP vectorizer makes, by each 8 bytes.
static void frames(unw_context_t *ctx) {
  unw_cursor_t c;
  unw_init_local(&c, ctx);
  while (unw_step(&c) > 0) {
    unw_word_t ip;
    unw_get_reg(&c, UNW_REG_IP, &ip);
    const Record *r = call(ip)->r;
    if (!r) continue;
    int from = 3 + r->at[2].offset, n = 0, m = 0;
    for (int i = from; i < r->locations; i++) n += r->at[i].size / 8;
    void **slots[n], *moved[n];
    for (int i = from; i + 1 < r->locations; i += 2) {
      void **base = spilled(&c, &r->at[i]), **derived = base ? spilled(&c, &r->at[i + 1]) : 0;
      for (int k = 0; base && k < r->at[i].size / 8; k++) {
        slots[m] = &base[k], moved[m] = traced(base[k]), m++;
        if (derived) slots[m] = &derived[k], moved[m] = (char *)moved[m - 1] + ((char *)derived[k] - (char *)base[k]), m++;
      }
    }
    while (m--) *slots[m] = moved[m];
  }
}

// Ids. Those bober_id gave out are kept by the address they are of, open addressed and at most half full, and each
// collection moves those of what moved and drops those of what died.

typedef struct { void *at; uint64_t id; } Id;
static Id *ids;
static uint64_t idMask, idCount, lastId;

/// The entry of p, or where it goes.
static Id *entry(void *p) {
  uint64_t i = (uintptr_t)p * 0x9e3779b97f4a7c15 >> 20;
  while (ids[i & idMask].at && ids[i & idMask].at != p) i++;
  return &ids[i & idMask];
}

/// Where p points once traced, or null if what it points into was not reached.
static void *survivor(void *p) {
  Page *g;
  char *h = object(p, &g);
  if (!h) return p;
  bool marked = bit(marks, cell(h));
  if (g->kind != YOUNG) return minor && !g->used || marked ? p : 0;
  return marked ? *(char **)h + ((char *)p - h) : 0;
}

/// Moves the ids into a table of n entries.
static void rekey(uint64_t n) {
  Id *old = ids;
  uint64_t m = idMask;
  if (!(ids = calloc(n, sizeof *ids))) fputs("out of memory\n", stderr), exit(1);
  idMask = n - 1, idCount = 0;
  for (uint64_t i = 0; old && i <= m; i++)
    if (old[i].at) *entry(old[i].at) = old[i], idCount++;
  free(old);
}

/// Moves each id to where what it is of went once traced, dropping those of the dead.
static void survive(void) {
  for (uint64_t i = 0; i <= idMask; i++)
    if (ids[i].at) ids[i].at = survivor(ids[i].at);
  rekey(idMask + 1);
}

uint64_t bober_id(void *p) {
  if (!p) return 0;  // which entry takes for an empty slot
  if (2 * (idCount + 1) > idMask + 1) rekey(ids ? 2 * (idMask + 1) : 64);
  Id *e = entry(p);
  if (!e->at) *e = (Id){p, ++lastId}, idCount++;
  return e->id;
}

/// Traces from the roots, then frees the young pages, the new objects not reached and, in a major, the old ones.
/// Probes bober:gc_start with the bytes allocated since the last collection, and bober:gc_end with those old.
static void collect(void) {
  PROBE(gc_start, young);
  if (!calls) records();
  minor = olds <= major;
  memset(bumped, 0, sizeof bumped);
  unw_context_t ctx;
  unw_getcontext(&ctx);
  frames(&ctx);
  bober_parked(frames, !minor);
  for (const Root *g = bober_roots; g < bober_roots_end; g++) root(g->global, g->layout->size, g->layout);
  for (int i = 0; i < 1024; i++)
    for (Static *s = statics[i]; s; s = s->next) root((void **)(s + 1), s->size, s->layout);
  if (minor) dirty(false);
  while (grays) values(gray[--grays]);
  if (minor && verify) verifying = true, bober_parked(frames, true), dirty(true), verifying = false;
  if (ids) survive();
  if (!minor) memset(spare, 0, sizeof spare);
  int64_t live = 0;
  for (uint32_t p = 1; p < top; p++) {
    Page *g = &pages[p];
    if (g->kind == YOUNG) {
      release(p, 1);
    } else if (minor && !(g->kind == BIG && g->used)) {
      continue;
    } else if (g->kind == BIG) {
      g->used = 0;
      if (bit(marks, cell(at(p)))) put(marks, cell(at(p)), 0), live += (int64_t)g->pages * PAGE;
      else release(p, g->pages);
    } else if (g->kind == OLD) {
      int64_t n = 0, size = sizes[g->class];
      for (char *h = at(p); h + size <= at(p + 1); h += size) n += bit(marks, cell(h));
      if (!n) {
        release(p, 1);
        continue;
      }
      live += n * size;
      for (char *h = at(p); h + size <= at(p + 1); h += size)
        if (bit(marks, cell(h))) put(marks, cell(h), 0);
        else {
          for (int64_t w = 0; g->class < CLASSES && w < size / 8; w++) put(refs, word(h) + w, 0);
          *(char **)h = spare[g->class], spare[g->class] = h;
        }
    }
  }
  memset(cards, 0, (int64_t)top * PAGE >> CARD);
  if (minor) olds += live;
  else olds = live, major = 2 * live > 64 << 20 ? 2 * live : 64 << 20, majors++;
  allocated += young, collections++, young = 0;
  PROBE(gc_end, olds);
}

void *bober_static(const void *key, const void *t, int64_t size, const bober_layout *l) {
  Static **at = &statics[((uintptr_t)key ^ (uintptr_t)t) * 0x9e3779b97f4a7c15 >> 54];
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
  if ((young += size) > every) collect();
  char *h;
  if (size > SMALL) {
    uint32_t p = take((size + PAGE - 1) / PAGE, BIG);
    pages[p].used = 1, pages[p].class = l ? 0 : CLASSES, h = at(p);
  } else {
    Page *g = &pages[bumped[c]];
    if (!bumped[c] || g->used + sizes[c] > PAGE) {
      bumped[c] = take(1, YOUNG);
      g = &pages[bumped[c]];
      g->class = c;
    }
    h = at(bumped[c]) + g->used;
    g->used += sizes[c];
  }
  if (l) lay(h, n, l);
  return h;
}

/// Bumps through the young page of the size's class, which no page yet is before the first allocation.
void *bober_alloc(int64_t n, const bober_layout *l) {
  int64_t size = (n + 15) & ~15;
  allocs++;
  int c = size <= SMALL ? classOf[size / 16] + (l ? 0 : CLASSES) : 0;
  Page *g = &pages[bumped[c]];
  if (__builtin_expect(size > SMALL || young + size > every || !bumped[c] || g->used + sizes[c] > PAGE, 0))
    return slow(n, size, c, l);
  young += size;
  char *h = at(bumped[c]) + g->used;
  g->used += sizes[c];
  if (l && !((uintptr_t)l & 1) && n == l->size && n <= 512) or64(refs, word(h), l->refs[0]);
  else if (l) lay(h, n, l);
  return h;
}
