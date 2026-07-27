#ifndef SPINLOCK_H
#define SPINLOCK_H

typedef struct {
    volatile int locked;
} spinlock_t;

void spinlock_init(spinlock_t *lock);
unsigned int spinlock_acquire(spinlock_t *lock); /* returns saved eflags, pass to release */
void spinlock_release(spinlock_t *lock, unsigned int saved_eflags);

#endif
