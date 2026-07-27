#ifndef MUTEX_H
#define MUTEX_H

#include "spinlock.h"

struct task;

#define MUTEX_MAX_WAITERS 16

typedef struct {
    volatile int locked;
    struct task *waiters[MUTEX_MAX_WAITERS];
    int waiter_count;
    spinlock_t guard; /* protects locked + the waiter list themselves --
                          a very short critical section, unlike the
                          mutex's own (potentially long) hold time */
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);   /* blocks (via the scheduler, not spinning) if contended */
void mutex_unlock(mutex_t *m);
int mutex_try_lock(mutex_t *m); /* returns 1 if acquired, 0 if already locked -- never blocks */

#endif
