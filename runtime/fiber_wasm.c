/* fiber_wasm.c - fibers for the browser build. WebAssembly has no stack switching, so every fiber
 * is a wasi-thread (a Web Worker running the same module over the shared memory). Exactly one
 * fiber runs at a time: switching wakes the target with an atomic notify and puts the caller to
 * sleep on its own flag. A restarted fiber unwinds to its start point with longjmp (wasm EH). */
#include "fiber.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Fiber {
  pthread_t th; void (*entry)(void);
  volatile int run;        /* 1 = this fiber owns the CPU */
  volatile int restart;    /* discard the current stack and call entry() again when woken */
  int id, started;
  jmp_buf jb;
};
static Fiber g_main; static int g_n;
static __thread Fiber *t_self;

static void wait_run(Fiber *f) {
  while (__atomic_load_n(&f->run, __ATOMIC_SEQ_CST) == 0)
    __builtin_wasm_memory_atomic_wait32((int *)&f->run, 0, -1);
}
static void wake(Fiber *f) {
  __atomic_store_n(&f->run, 1, __ATOMIC_SEQ_CST);
  __builtin_wasm_memory_atomic_notify((int *)&f->run, 1);
}

Fiber *fiber_self(void) { if (!t_self) { t_self = &g_main; g_main.run = 1; g_main.started = 1; } return t_self; }
int fiber_id(Fiber *f) { return f->id; }

static void *thread_main(void *arg) {
  Fiber *f = arg; t_self = f;
  wait_run(f);
  f->started = 1;
  setjmp(f->jb);              /* fiber_restart lands here */
  f->restart = 0;
  f->entry();
  fprintf(stderr, "[fiber] entry of fiber %d returned\n", f->id);
  abort();
}

Fiber *fiber_new(void (*entry)(void)) {
  Fiber *f = calloc(1, sizeof *f);
  f->entry = entry; f->id = ++g_n;
  pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1 << 20);
  if (pthread_create(&f->th, &at, thread_main, f)) { fprintf(stderr, "[fiber] cannot create thread\n"); abort(); }
  pthread_attr_destroy(&at);
  return f;
}
void fiber_restart(Fiber *f, void (*entry)(void)) { f->entry = entry; if (f->started) f->restart = 1; }

void fiber_switch(Fiber *to) {
  Fiber *me = fiber_self();
  if (to == me) return;
  __atomic_store_n(&me->run, 0, __ATOMIC_SEQ_CST);
  wake(to);
  wait_run(me);
  if (me->restart) longjmp(me->jb, 1);
}
