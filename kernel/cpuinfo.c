/* cpuinfo.c -- CPU identification via the `cpuid` instruction
 *
 * Real hardware info, not a placeholder: the 12-character vendor ID
 * string (leaf 0) is available on every x86 CPU since the early 90s;
 * the human-readable brand string (leaves 0x80000002-4) is available
 * on most real CPUs and, notably, on QEMU/KVM's emulated CPU too, so
 * this correctly reports what's actually running underneath us.
 */

#include "cpuinfo.h"

static inline void cpuid(unsigned int leaf, unsigned int *a, unsigned int *b,
                          unsigned int *c, unsigned int *d) {
    __asm__ volatile ("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf));
}

void cpu_get_vendor(char out[13]) {
    unsigned int a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    /* Vendor ID string is packed EBX:EDX:ECX (that specific order,
     * not alphabetical -- an early Intel design quirk everyone since
     * has had to match for compatibility). */
    *(unsigned int *)(out + 0) = b;
    *(unsigned int *)(out + 4) = d;
    *(unsigned int *)(out + 8) = c;
    out[12] = '\0';
}

int cpu_get_brand(char out[49]) {
    unsigned int a, b, c, d;

    cpuid(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000004u) return 0; /* extended brand leaves not supported */

    unsigned int *dst = (unsigned int *)out;
    cpuid(0x80000002u, &a, &b, &c, &d);
    dst[0] = a; dst[1] = b; dst[2] = c; dst[3] = d;
    cpuid(0x80000003u, &a, &b, &c, &d);
    dst[4] = a; dst[5] = b; dst[6] = c; dst[7] = d;
    cpuid(0x80000004u, &a, &b, &c, &d);
    dst[8] = a; dst[9] = b; dst[10] = c; dst[11] = d;
    out[48] = '\0';
    return 1;
}
