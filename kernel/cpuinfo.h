#ifndef CPUINFO_H
#define CPUINFO_H

void cpu_get_vendor(char out[13]); /* always available, 12 chars + NUL */
int cpu_get_brand(char out[49]);    /* returns 0 if unsupported, else fills 48 chars + NUL */

#endif
