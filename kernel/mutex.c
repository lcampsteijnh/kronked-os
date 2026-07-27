/* mutex.c -- a real blocking mutex
 *
 * Spinlocks (spinlock.c) are the right tool for the short critical
 * sections that exist so far in this kernel (the heap allocator, the
 * PMM, the task list) -- holding interrupts off for a few dozen
 * instructions costs nothing meaningful. A mutex is for anything that
 * might hold a lock for a while: instead of burning CPU spinning (or
 * worse, spinning with interrupts disabled, which is what a spinlock
 * would mean for any non-trivial hold time), a contended mutex_lock()
 * puts the calling task to sleep via the scheduler and only wakes it
 * back up when the lock is actually released.
 *
 * The mutex's own tiny bit of state (the locked flag and the waiter
 * list) is still protected by a spinlock internally -- that critical
 * section is only ever a few instructions long, so a spinlock is
 * handles *that* part.
 */

#include "mutex.h"
#include "task.h"

void mutex_init(mutex_t *m) {
    m->locked = 0;
    m->waiter_count = 0;
    spinlock_init(&m->guard);
}

void mutex_lock(mutex_t *m) {
    for (;;) {
        unsigned int flags = spinlock_acquire(&m->guard);

        if (!m->locked) {
            m->locked = 1;
            spinlock_release(&m->guard, flags);
            return;
        }

        /* Contended: register as a waiter, then actually sleep. If
         * another task manages to squeeze in and grab the lock
         * between us releasing the guard and task_block() actually
         * switching us out, that's fine -- we'll just get woken by
         * whichever unlock() reaches us next and loop back around to
         * recheck, exactly like a spurious-wakeup-tolerant condition
         * variable. */
        if (m->waiter_count < MUTEX_MAX_WAITERS) {
            m->waiters[m->waiter_count++] = task_get_current();
        }
        spinlock_release(&m->guard, flags);

        task_block();
        /* resumes here once some unlock() call wakes us; loop back
         * and try to actually acquire it -- we're not guaranteed to
         * be the one who gets it if multiple tasks were waiting. */
    }
}

int mutex_try_lock(mutex_t *m) {
    unsigned int flags = spinlock_acquire(&m->guard);
    int got_it = 0;
    if (!m->locked) {
        m->locked = 1;
        got_it = 1;
    }
    spinlock_release(&m->guard, flags);
    return got_it;
}

void mutex_unlock(mutex_t *m) {
    unsigned int flags = spinlock_acquire(&m->guard);

    m->locked = 0;

    struct task *to_wake = 0;
    if (m->waiter_count > 0) {
        to_wake = m->waiters[0];
        for (int i = 1; i < m->waiter_count; i++) m->waiters[i - 1] = m->waiters[i];
        m->waiter_count--;
    }

    spinlock_release(&m->guard, flags);

    /* Wake outside the guarded section -- task_unblock() just flips a
     * state flag (cheap, and doesn't touch anything the guard
     * protects), but there's no reason to hold the spinlock (and
     * therefore interrupts disabled) any longer than necessary. */
    if (to_wake) task_unblock(to_wake);
}
