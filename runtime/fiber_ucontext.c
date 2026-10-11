/* fiber_ucontext.c - native fibers (tests / desktop build) */
#define _GNU_SOURCE
#include "fiber.h"
#include <stdlib.h>
#include <ucontext.h>
#include <sys/mman.h>

struct Fiber { ucontext_t uc; void *stack; int id; };
static Fiber g_main; static Fiber *g_cur; static int g_n;
#define STACK (8u << 20)

Fiber *fiber_self(void) { if (!g_cur) g_cur = &g_main; return g_cur; }
int fiber_id(Fiber *f) { return f->id; }
void fiber_restart(Fiber *f, void (*entry)(void)) {
  getcontext(&f->uc);
  f->uc.uc_stack.ss_sp = f->stack; f->uc.uc_stack.ss_size = STACK; f->uc.uc_link = NULL;
  makecontext(&f->uc, entry, 0);
}
Fiber *fiber_new(void (*entry)(void)) {
  Fiber *f = calloc(1, sizeof *f);
  f->stack = mmap(NULL, STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  f->id = ++g_n;
  fiber_restart(f, entry);
  return f;
}
void fiber_switch(Fiber *to) {
  Fiber *me = fiber_self();
  if (to == me) return;
  g_cur = to;
  swapcontext(&me->uc, &to->uc);
}
