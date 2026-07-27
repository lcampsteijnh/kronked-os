/* spinlock.c -- IRQ-safe spinlocks
 *
 * On a single core, the only thing that can interrupt a critical
 * section is an IRQ preempting us mid-way through it -- there's no
 * other CPU to race with. So acquiring disables interrupts (saving
 * the previous state to restore on release) *and* does a real atomic
 * test-and-set loop, which costs nothing extra on one core but means
 * this code is already correct if a second core shows up later.
 */

#include "spinlock.h"

void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
}

unsigned int spinlock_acquire(spinlock_t *lock) {
    unsigned int eflags;
    __asm__ volatile (
        "pushfl\n\t"
        "pop %0\n\t"
        "cli"
        : "=r"(eflags) :: "memory"
    );

    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        /* Won't actually spin more than an instant on a single core --
         * interrupts are off, so nothing else can be running to hold
         * this lock right now, except a genuine bug (double-acquire). */
    }

    return eflags;
}

void spinlock_release(spinlock_t *lock, unsigned int saved_eflags) {
    __sync_lock_release(&lock->locked);
    if (saved_eflags & 0x200) { /* IF was set before we acquired -- restore it */
        __asm__ volatile ("sti");
    }
}
