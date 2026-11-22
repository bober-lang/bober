//! Fibers: each runs on a stack of its own and takes turns with the others, one running until it parks. A switch pushes
//! the callee-saved registers onto the stack it leaves, keeps its stack pointer in the fiber, and pops them from the
//! stack it enters. A fiber's first frame returns to 0, from frame pointer 0, where a throw's unwinding and the
//! collector's walk stop. The collector walks each parked fiber's frames from the frame pointer its switch pushed.
//! Each fiber is on one of two lists, of those that ran since the last collection and of the others, each doubly
//! linked, so that a fiber resumed moves to the first and one that ends leaves either, at once. A minor collection
//! walks only the frames of the first (src/gc.c), as the others hold no young ref. Each isolate has fibers of its own.

#ifndef BOBER_ARCH
#include "rt.h"
#endif
#include <stdlib.h>
#include <sys/mman.h>

#define STACK (1 << 20)  // of address space, committed as used
#define GUARD (1 << 16)  // at its bottom, more than a frame takes, so that none steps over it

typedef struct Link { struct Link *prev, *next; } Link;
typedef struct Fiber {
  Link link;  // in ran or idle
  void *sp;
  char *stack;  // null for the first fiber, on the thread's stack
  void *run;  // the function value a new fiber runs, until it starts
  struct Fiber *later;  // in the queue of those ready; 0 if not queued or last
  int dropped;          // by bober_fibers_reset; bober_wake ignores it
} Fiber;

typedef struct {
  Link ran, idle;  // those that ran since the last collection, the running one among them, and the others
  Fiber first, *running, *ready, *last, *dead;
} Fibers;

static _Thread_local Fibers fb;

void bober_fibers_init(void) {
  fb = (Fibers){0};
  fb.ran = (Link){&fb.first.link, &fb.first.link};
  fb.idle = (Link){&fb.idle, &fb.idle};
  fb.first.link = (Link){&fb.ran, &fb.ran};
  fb.running = &fb.first;
}

/// Takes f off its list.
static void off(Fiber *f) { f->link.prev->next = f->link.next, f->link.next->prev = f->link.prev; }

/// Puts f first on list l.
static void onto(Fiber *f, Link *l) { f->link = (Link){l, l->next}, l->next->prev = &f->link, l->next = &f->link; }

#if defined(__APPLE__)
#define SYM(s) "_" #s
#else
#define SYM(s) #s
#endif

/// Saves the callee-saved registers and the stack pointer in *from, and resumes the fiber whose stack pointer is to.
void bober_switch(void **from, void *to);
#if defined(__aarch64__)
__asm__(".globl " SYM(bober_switch) "\n" SYM(bober_switch) ":\n"
        "  sub sp, sp, #160\n"
        "  stp x19, x20, [sp, #0]\n  stp x21, x22, [sp, #16]\n  stp x23, x24, [sp, #32]\n"
        "  stp x25, x26, [sp, #48]\n  stp x27, x28, [sp, #64]\n  stp x29, x30, [sp, #80]\n"
        "  stp d8, d9, [sp, #96]\n  stp d10, d11, [sp, #112]\n  stp d12, d13, [sp, #128]\n  stp d14, d15, [sp, #144]\n"
        "  mov x2, sp\n  str x2, [x0]\n  mov sp, x1\n"
        "  ldp x19, x20, [sp, #0]\n  ldp x21, x22, [sp, #16]\n  ldp x23, x24, [sp, #32]\n"
        "  ldp x25, x26, [sp, #48]\n  ldp x27, x28, [sp, #64]\n  ldp x29, x30, [sp, #80]\n"
        "  ldp d8, d9, [sp, #96]\n  ldp d10, d11, [sp, #112]\n  ldp d12, d13, [sp, #128]\n  ldp d14, d15, [sp, #144]\n"
        "  add sp, sp, #160\n  ret\n"
        // A new fiber's: calls x19 with a return address of 0.
        SYM(bober_enter) ":\n  mov x30, xzr\n  br x19\n");
#define PUSHED_FP 10  // where among what it pushed x29 is
#else
__asm__(".globl " SYM(bober_switch) "\n" SYM(bober_switch) ":\n"
        "  push %rbp\n  push %rbx\n  push %r12\n  push %r13\n  push %r14\n  push %r15\n"
        "  mov %rsp, (%rdi)\n  mov %rsi, %rsp\n"
        "  pop %r15\n  pop %r14\n  pop %r13\n  pop %r12\n  pop %rbx\n  pop %rbp\n  ret\n");
#define PUSHED_FP 5  // where among what it pushed rbp is
#endif

/// Runs fiber f, keeping where the running one parks; frees the stack of one that ended.
static void to(Fiber *f) {
  Fiber *self = fb.running;
  fb.running = f;
  off(f), onto(f, &fb.ran);
  bober_switch(&self->sp, f->sp);
  if (fb.dead) munmap(fb.dead->stack, STACK), free(fb.dead), fb.dead = 0;
}

/// Runs the next fiber ready, waiting on I/O until one is.
static void next(void) {
  while (!fb.ready)
    if (!bober_io_wait()) bober_trap("every fiber is parked", 0, -1);
  Fiber *f = fb.ready;
  if (!(fb.ready = f->later)) fb.last = 0;
  f->later = 0;
  if (f != fb.running) to(f);
}

/// A new fiber's first frame: runs its function value, then ends the fiber.
static void begin(void) {
  Fiber *self = fb.running;
  void *f = self->run;
  self->run = 0;
  (*(void (**)(void *))f)(f);
  off(self);
  fb.dead = self;
  next();
}

void bober_fiber(uint64_t f) {
  Fiber *n = calloc(1, sizeof *n);
  n->stack = mmap(0, STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  // The guard faults, so that a fiber that overflows its stack stops rather than writes over the one below.
  if (!n || n->stack == MAP_FAILED || mprotect(n->stack, GUARD, PROT_NONE)) bober_trap("out of memory", 0, -1);
  void **top = (void **)(n->stack + STACK);
#if defined(__aarch64__)
  extern char bober_enter[];
  n->sp = top - 20;  // x19 to x30 and d8 to d15, as bober_switch pops them
  ((void **)n->sp)[0] = (void *)begin, ((void **)n->sp)[11] = bober_enter;
#else
  *--top = 0;  // begin's return address
  *--top = (void *)begin;
  n->sp = top - 6;  // rbp, rbx and r12 to r15, as bober_switch pops them
#endif
  n->run = (void *)(uintptr_t)f;
  onto(n, &fb.ran);
  fb.running->later = fb.ready, fb.ready = fb.running;  // the caller first, once n parks
  if (!fb.last) fb.last = fb.running;
  to(n);
}

uint64_t bober_self(void) {
  return (uintptr_t)fb.running;
}

void bober_park(void) {
  next();
}

void bober_wake(uint64_t f) {
  Fiber *w = (Fiber *)(uintptr_t)f;
  if (w->dropped || w->later || w == fb.last) return;  // dropped, or queued already
  if (fb.last) fb.last->later = w;
  else fb.ready = w;
  fb.last = w;
}

/// The frame pointer of where f parked.
static void **parked(Fiber *f) { return ((void ***)f->sp)[PUSHED_FP]; }

/// Calls visit with the frame pointer of where each fiber but the running one parked, for the collector: those that ran
/// since the last collection, or, if all, every one; they then count as not run since.
void bober_parked(void (*visit)(void **), int all) {
  for (Link *l = fb.idle.next; all && l != &fb.idle; l = l->next) visit(parked((Fiber *)l));
  for (Link *l = fb.ran.next, *n; l != &fb.ran; l = n) {
    Fiber *f = (Fiber *)l;
    n = l->next;
    if (f != fb.running) visit(parked(f)), off(f), onto(f, &fb.idle);
  }
}

/// Drops every fiber but the first, which runs again, as once a test that trapped jumped back to it. What outlives
/// the test, as a Signal in a global, may still hold one, so its struct is kept, dropped.
void bober_fibers_reset(void) {
  bober_io_cancel();  // their waits are on their stacks
  while (fb.ran.next != &fb.ran || fb.idle.next != &fb.idle) {
    Fiber *f = (Fiber *)(fb.ran.next != &fb.ran ? fb.ran.next : fb.idle.next);
    off(f);
    if (f != &fb.first) munmap(f->stack, STACK), f->dropped = 1;
  }
  if (fb.dead) munmap(fb.dead->stack, STACK), free(fb.dead), fb.dead = 0;
  fb.running = &fb.first, fb.first.later = 0, fb.ready = fb.last = 0;
  onto(&fb.first, &fb.ran);
}
