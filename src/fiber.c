//! Fibers: each runs on a stack of its own and takes turns with the others, one running until it parks. A switch pushes
//! the callee-saved registers onto the stack it leaves, keeps its stack pointer in the fiber, and pops them from the
//! stack it enters. A fiber's first frame returns to 0, where unwinding, and so a throw or the collector, stops. Each
//! fiber but the running one keeps where it parked as libunwind's context, from which the collector walks its frames.

#ifndef BOBER_ARCH
#include "rt.h"
#endif
#include <libunwind.h>
#include <stdlib.h>
#include <sys/mman.h>

#define STACK (1 << 20)  // of address space, committed as used
#define GUARD (1 << 16)  // at its bottom, more than a frame takes, so that none steps over it

typedef struct Fiber {
  void *sp;
  char *stack;  // null for the first fiber, on the process's stack
  unw_context_t parked;
  void *run;  // the function value a new fiber runs, until it starts
  struct Fiber *next, *later;  // in the list of all, and in the queue of those ready
  int dropped;                 // by bober_fibers_reset; bober_wake ignores it
} Fiber;

static Fiber first, *running = &first, *all = &first, *ready, *last, *dead;

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
#else
__asm__(".globl " SYM(bober_switch) "\n" SYM(bober_switch) ":\n"
        "  push %rbp\n  push %rbx\n  push %r12\n  push %r13\n  push %r14\n  push %r15\n"
        "  mov %rsp, (%rdi)\n  mov %rsi, %rsp\n"
        "  pop %r15\n  pop %r14\n  pop %r13\n  pop %r12\n  pop %rbx\n  pop %rbp\n  ret\n");
#endif

/// Runs fiber f, keeping where the running one parks; frees the stack of one that ended.
static void to(Fiber *f) {
  Fiber *self = running;
  running = f;
  unw_getcontext(&self->parked);
  bober_switch(&self->sp, f->sp);
  if (dead) munmap(dead->stack, STACK), free(dead), dead = 0;
}

/// Runs the next fiber ready, waiting on I/O until one is.
static void next(void) {
  while (!ready)
    if (!bober_io_wait()) bober_trap("every fiber is parked", 0, -1);
  Fiber *f = ready;
  if (!(ready = f->later)) last = 0;
  if (f != running) to(f);
}

/// A new fiber's first frame: runs its function value, then ends the fiber.
static void begin(void) {
  Fiber *self = running;
  void *f = self->run;
  self->run = 0;
  (*(void (**)(void *))f)(f);
  Fiber **p = &all;
  while (*p != self) p = &(*p)->next;
  *p = self->next;
  dead = self;
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
  n->run = (void *)(uintptr_t)f, n->next = all, all = n;
  running->later = ready, ready = running;  // the caller first, once n parks
  if (!last) last = running;
  to(n);
}

uint64_t bober_self(void) { return (uintptr_t)running; }

void bober_park(void) { next(); }

void bober_wake(uint64_t f) {
  Fiber *w = (Fiber *)(uintptr_t)f;
  if (w->dropped) return;
  w->later = 0;
  if (last) last->later = w;
  else ready = w;
  last = w;
}

/// Calls visit with where each fiber but the running one parked, for the collector.
void bober_parked(void (*visit)(unw_context_t *)) {
  for (Fiber *f = all; f; f = f->next)
    if (f != running) visit(&f->parked);
}

/// Drops every fiber but the first, which runs again, as once a test that trapped jumped back to it. What outlives
/// the test, as a Signal in a global, may still hold one, so its struct is kept, dropped.
void bober_fibers_reset(void) {
  bober_io_cancel();  // their waits are on their stacks
  while (all != &first) {
    Fiber *f = all;
    all = f->next;
    munmap(f->stack, STACK), f->dropped = 1;
  }
  if (dead) munmap(dead->stack, STACK), free(dead), dead = 0;
  running = &first, first.next = 0, ready = last = 0;
}
