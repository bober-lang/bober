//! The collector of what bober_alloc gives out: precise, finding the refs of frames by LLVM's stack maps, and moving
//! what is young. The heap is pages of 64 KiB, each holding objects of one size (a BiBoP), so that where an object
//! starts follows from any pointer into it. An object starts young, bumped to in a page of its size; a collection
//! traces from the roots, the refs the stack maps say the frames of each fiber hold and the globals in section
//! bober_roots, moving each young object it reaches into an old page and marking each old one in place. It then frees
//! the young pages, and the old objects left unmarked onto the free lists promotion takes from. An object of over 8 KiB
//! is old from the start, in pages of its own. Each object has a header of 16 bytes: its layout, and its new place once
//! moved or, if old, whether it is marked.

#include "rt.h"
#include <libunwind.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE (1 << 16)
#define PAGES (1 << 20)  // of address space, 64 GiB
#define SMALL 8192       // the size, its header in, of the largest object of a size class
#define CLASSES 36
#define MARKED 1
#define SPARE 2  // an old slot on a free list

typedef struct { const bober_layout *layout; uintptr_t moved; } Header;
enum { FREE, YOUNG, OLD, BIG, REST };
/// A page: its kind, its size class if young or old, the offset bumped to if young, and the pages of a big object if
/// its first, or else how far back that is.
typedef struct { uint8_t kind, class; uint32_t used, pages; } Page;

static char *heap;
static Page pages[PAGES];
static uint32_t top = 1;   // the pages ever used, from 1, so that 0 is no page
static uint32_t hint = 1;  // where the first free page may be
static uint32_t sizes[CLASSES], classOf[SMALL / 16 + 1];
static uint32_t bumped[CLASSES];  // the young page of each class being bumped through
static Header *spare[CLASSES];    // the old slots of each class free, through their layout
static int64_t young, every = 64 << 20;  // what has been allocated since the last collection, and how much collects
static bool fixed;                       // whether BOBER_GC_EVERY fixed every
static int64_t allocs, allocated, collections;  // in all, for BOBER_GC_STATS

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

/// What BOBER_GC_STATS=1 prints at exit: the objects and bytes allocated, the collections, and the heap at its peak.
static void stats(void) {
  fprintf(stderr, "gc: %lld allocs, %lld MiB, %lld collections, %lld MiB peak\n", (long long)allocs,
          (long long)(allocated + young) >> 20, (long long)collections, (long long)top * PAGE >> 20);
}

/// The 16 sizes up to 256 bytes, then 4 for each doubling, of up to SMALL; and the heap's address space.
static void start(void) {
  for (int c = 0; c < CLASSES; c++) sizes[c] = c < 16 ? 16 * (c + 1) : (64 << (c - 16) / 4) * (5 + (c - 16) % 4);
  for (int n = 0, c = 0; n <= SMALL / 16; n++) {
    while (16 * n > sizes[c]) c++;
    classOf[n] = c;
  }
  heap = mmap(0, (int64_t)PAGES * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (heap == MAP_FAILED) fputs("out of memory\n", stderr), exit(1);
  const char *e = getenv("BOBER_GC_EVERY");
  if (e) every = atoll(e), fixed = true;
  if (getenv("BOBER_GC_STATS")) atexit(stats);
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

/// Frees n pages from p, zeroing them.
static void release(uint32_t p, int n) {
  if (n > 1) madvise(at(p), (int64_t)n * PAGE, MADV_DONTNEED);
  else memset(at(p), 0, PAGE);
  for (int i = 0; i < n; i++) pages[p + i].kind = FREE;
  if (p < hint) hint = p;
}

/// An old slot of class c, from its free list, which a new page refills.
static Header *promoted(int c) {
  if (!spare[c]) {
    uint32_t p = take(1, OLD);
    pages[p].class = c;
    for (uint32_t o = 0; o + sizes[c] <= PAGE; o += sizes[c]) {
      Header *h = (Header *)(at(p) + o);
      *h = (Header){(const bober_layout *)spare[c], SPARE};
      spare[c] = h;
    }
  }
  Header *h = spare[c];
  spare[c] = (Header *)h->layout;
  return h;
}

// Tracing.

static Header **gray;
static int64_t grays, grayCap;

/// The header of the object p points into, and its page, or null if p is not into the heap, as into a frame, a
/// constant or null. p is past its object's header, maybe at its end, as an empty object's or a slice's to the end is,
/// so its last byte before p says which object it is.
static Header *object(void *p, Page **page) {
  uintptr_t a = (uintptr_t)p - (uintptr_t)heap - 1;
  if (a >= (uintptr_t)top * PAGE) return 0;
  uint32_t i = a / PAGE;
  if (pages[i].kind == REST) i -= pages[i].pages;
  Page *g = *page = &pages[i];
  if (g->kind == FREE) return 0;  // no ref, but a number a packed union holds where another member holds one (§4.4)
  return (Header *)(g->kind == BIG ? at(i) : at(i) + a % PAGE / sizes[g->class] * sizes[g->class]);
}

/// The place of what p points into once traced: a young object moved, an old one marked, each first reached put on
/// gray. p not into the heap stays.
static void *trace(void *p) {
  Page *g;
  Header *h = object(p, &g);
  if (!h) return p;
  intptr_t into = (char *)p - (char *)h;
  if (g->kind == YOUNG) {
    if (h->moved) return (char *)h->moved + into;
    Header *n = promoted(g->class);
    memcpy(n, h, sizes[g->class]);
    h->moved = (uintptr_t)n;
    h = n;
  } else if (h->moved == MARKED) {
    return p;
  }
  h->moved = MARKED;
  if (grays == grayCap && !(gray = realloc(gray, (grayCap = 2 * grayCap + 1024) * sizeof *gray))) abort();
  gray[grays++] = h;
  return (char *)h + into;
}

/// trace of the ref in p's low 7 bytes, its top byte kept, where a packed union may keep its tag (§4.4).
static void *traced(void *p) {
  uintptr_t top = (uintptr_t)p >> 56 << 56;
  return (void *)((uintptr_t)trace((void *)((uintptr_t)p - top)) + top);
}

/// Traces the refs in the bytes at p, values laid out as l; if l's low bit is set, an array's cell: its length,
/// capacity and data, a ref, then its elements, unless push moved them out.
static void scan(void **p, int64_t bytes, const bober_layout *l) {
  if ((uintptr_t)l & 1) {
    p[2] = traced(p[2]);
    if (p[2] != p + 3) return;
    p += 3, bytes -= 24, l = (const bober_layout *)((uintptr_t)l - 1);
  }
  if (!l) return;
  for (int64_t v = 0; v + l->size <= bytes; v += l->size)
    for (int64_t w = 0; w < l->size / 8; w += 64)
      for (uint64_t bits = l->refs[w / 64]; bits; bits &= bits - 1) {
        void **r = &p[v / 8 + w + __builtin_ctzll(bits)];
        *r = traced(*r);
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
/// Calls visit with where each fiber but the running one parked (src/fiber.c).
void bober_parked(void (*visit)(unw_context_t *));
typedef struct { void *global; const bober_layout *layout; } Root;
BOBER_SECTION(const Root, bober_roots);

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
  Header *h = object(p, &g);
  if (!h) return p;
  if (g->kind != YOUNG) return h->moved == MARKED ? p : 0;
  return h->moved ? (char *)h->moved + ((char *)p - (char *)h) : 0;
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

/// Traces from the roots, then frees the young pages and the old objects not reached. Probes bober:gc_start with the
/// bytes allocated since the last collection, and bober:gc_end with those still live.
static void collect(void) {
  PROBE(gc_start, young);
  if (!calls) records();
  memset(bumped, 0, sizeof bumped);
  unw_context_t ctx;
  unw_getcontext(&ctx);
  frames(&ctx);
  bober_parked(frames);
  for (const Root *g = bober_roots; g < bober_roots_end; g++) scan(g->global, g->layout->size, g->layout);
  while (grays) {
    Header *h = gray[--grays];
    Page *g = &pages[((char *)h - heap) / PAGE];
    scan((void **)(h + 1), (g->kind == BIG ? (int64_t)g->pages * PAGE : sizes[g->class]) - sizeof *h, h->layout);
  }
  if (ids) survive();
  memset(spare, 0, sizeof spare);
  int64_t live = 0;
  for (uint32_t p = 1; p < top; p++) {
    Page *g = &pages[p];
    if (g->kind == YOUNG) {
      release(p, 1);
    } else if (g->kind == BIG) {
      Header *h = (Header *)at(p);
      if (h->moved) h->moved = 0, live += (int64_t)g->pages * PAGE;
      else release(p, g->pages);
    } else if (g->kind == OLD) {
      int64_t n = 0;
      for (uint32_t o = 0; o + sizes[g->class] <= PAGE; o += sizes[g->class]) n += ((Header *)(at(p) + o))->moved == MARKED;
      if (!n) {
        release(p, 1);
        continue;
      }
      live += n * sizes[g->class];
      for (uint32_t o = 0; o + sizes[g->class] <= PAGE; o += sizes[g->class]) {
        Header *h = (Header *)(at(p) + o);
        if (h->moved == MARKED) h->moved = 0;
        else *h = (Header){(const bober_layout *)spare[g->class], SPARE}, spare[g->class] = h;
      }
    }
  }
  allocated += young, collections++, young = 0;
  PROBE(gc_end, live);
  if (!fixed) every = 2 * live > 64 << 20 ? 2 * live : 64 << 20;
}

/// What bober_alloc does but bump: starts, collects, and takes a page or a big object's pages.
__attribute__((noinline, cold)) static void *slow(int64_t size, const bober_layout *l) {
  if (!heap) start();
  if ((young += size) > every) collect();
  Header *h;
  if (size > SMALL) {
    h = (Header *)at(take((size + PAGE - 1) / PAGE, BIG));
  } else {
    int c = classOf[size / 16];
    Page *g = &pages[bumped[c]];
    if (!bumped[c] || g->used + sizes[c] > PAGE) {
      bumped[c] = take(1, YOUNG);
      g = &pages[bumped[c]];
      g->class = c;
    }
    h = (Header *)(at(bumped[c]) + g->used);
    g->used += sizes[c];
  }
  h->layout = l;
  return h + 1;
}

/// Bumps through the young page of the size's class, which no page yet is before the heap starts.
void *bober_alloc(int64_t n, const bober_layout *l) {
  int64_t size = (n + sizeof(Header) + 15) & ~15;
  allocs++;
  int c = size <= SMALL ? classOf[size / 16] : 0;
  Page *g = &pages[bumped[c]];
  if (__builtin_expect(size > SMALL || young + size > every || !bumped[c] || g->used + sizes[c] > PAGE, 0))
    return slow(size, l);
  young += size;
  Header *h = (Header *)(at(bumped[c]) + g->used);
  g->used += sizes[c];
  h->layout = l;
  return h + 1;
}
