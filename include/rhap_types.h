/*
 * rhap_types.h -- fixed-width types and compiler glue.
 *
 * Target: Apple Rhapsody DR2 for x86, egcs 1.1 / gcc 2.7.2, C89.
 * Everything here must compile with a 1998-vintage compiler, so:
 *   - no stdint.h, no <stdbool.h>, no // comments below this line
 *   - no declarations after statements (enforced by -Wdeclaration-after-statement)
 *   - no long long arithmetic in the hot paths
 */

#ifndef _RHAP_TYPES_H
#define _RHAP_TYPES_H

#include <stddef.h>

typedef unsigned char      u8;
typedef signed char        s8;
typedef unsigned short     u16;
typedef signed short       s16;
typedef unsigned int       u32;
typedef signed int         s32;
typedef unsigned long long u64;
typedef signed long long   s64;

/* The UHCI controller can only address 32 bits of physical memory. */
typedef u32 rhap_paddr_t;
typedef u32 rhap_vaddr_t;

#define RHAP_PACKED     __attribute__((packed))
#define RHAP_ALIGN(n)   __attribute__((aligned(n)))
#define RHAP_UNUSED     __attribute__((unused))
#define RHAP_NORETURN   __attribute__((noreturn))

#ifndef RHAP_NULL
#define RHAP_NULL ((void *)0)
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

/*
 * Compiler barrier.  x86 stores are strongly ordered, but the compiler is
 * free to sink a store past the doorbell write that tells the controller to
 * look at it.  Every "fill then activate" sequence in uhci.c needs this.
 */
#define rhap_barrier()      __asm__ __volatile__("" ::: "memory")

/*
 * MMIO-style access helpers.  The frame list, queue heads and transfer
 * descriptors are owned by the controller, so they are always accessed
 * through volatile pointers.
 */
#define rhap_read32(p)      (*(volatile u32 *)(void *)(p))
#define rhap_write32(p, v)  (*(volatile u32 *)(void *)(p) = (u32)(v))
#define rhap_read16(p)      (*(volatile u16 *)(void *)(p))
#define rhap_write16(p, v)  (*(volatile u16 *)(void *)(p) = (u16)(v))
#define rhap_read8(p)       (*(volatile u8 *)(void *)(p))

#define RHAP_MIN(a, b)      ((a) < (b) ? (a) : (b))
#define RHAP_MAX(a, b)      ((a) > (b) ? (a) : (b))

#endif /* _RHAP_TYPES_H */
