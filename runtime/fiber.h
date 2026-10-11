/* fiber.h - cooperative host fibers, one per guest thread (+ one for interrupt handlers).
 * Native: ucontext (fiber_ucontext.c). Browser: one Web Worker per fiber over shared memory
 * (fiber_worker.c + web/). Only one fiber runs at any time. */
#pragma once
typedef struct Fiber Fiber;
Fiber *fiber_self(void);
Fiber *fiber_new(void (*entry)(void));             /* starts at entry() when first switched to */
void fiber_restart(Fiber *f, void (*entry)(void)); /* discard f's state; entry() runs on next switch */
void fiber_switch(Fiber *to);                      /* suspend the current fiber, run 'to' */
int fiber_id(Fiber *f);
