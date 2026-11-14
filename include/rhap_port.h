/*
 * rhap_port.h -- the complete operating-system interface of this driver.
 *
 * This is the *only* header that knows anything about Rhapsody.  Two dozen
 * functions, listed below.  src/port_dr2.c has a skeleton for each with notes
 * on which DR2 kernel primitive to use; host/port_host.c has a working
 * implementation used by the unit tests.
 *
 * Design rule: the driver runs fully polled, driven by one kernel timer.
 * It therefore needs no IRQ allocation, no interrupt-service plumbing, no
 * power-management hooks and no IOKit-style workloops.  That is the single
 * reason this is portable to DR2 at all.
 */

#ifndef _RHAP_PORT_H
#define _RHAP_PORT_H

#include "rhap_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * 1. DMA memory.
 *
 * Must return physically contiguous memory below the 4 GB line (UHCI is
 * a 32-bit bus master) and the physical address of its first byte.
 * On DR2: kmem_alloc_contig() + pmap_extract(), or IOMallocContiguous().
 * ------------------------------------------------------------------ */
void        *rhap_dma_alloc(u32 bytes, u32 align, rhap_paddr_t *pa_out);
void         rhap_dma_free(void *va, u32 bytes);

/* Ordinary kernel memory for driver state (not visible to the controller).
 * On DR2: IOMalloc()/IOFree(), or kalloc()/kfree(). */
void        *rhap_mem_alloc(u32 bytes);
void         rhap_mem_free(void *va, u32 bytes);

/* Kernel virtual -> physical, for buffers the caller already owns.
 * On DR2: pmap_extract(kernel_pmap, (vm_offset_t)va).  Must cope with
 * kernel stacks and kmem_alloc'd memory. */
rhap_paddr_t rhap_kvtophys(void *va);

/* ------------------------------------------------------------------ *
 * 2. Port I/O (the UHCI register file lives in PCI I/O space, BAR0).
 * ------------------------------------------------------------------ */
u8           rhap_in8(u16 port);
void         rhap_out8(u16 port, u8 val);
u16          rhap_in16(u16 port);
void         rhap_out16(u16 port, u16 val);
u32          rhap_in32(u16 port);
void         rhap_out32(u16 port, u32 val);

/* ------------------------------------------------------------------ *
 * 3. PCI configuration space, mechanism #1 (0xCF8/0xCFC).
 * rhap_pci_find() returns the number of matches written to out[].
 * ------------------------------------------------------------------ */
struct rhap_pci_dev {
    u8  bus;
    u8  slot;
    u8  func;
    u16 vendor;
    u16 device;
    u8  irq;
};

int  rhap_pci_find(u8 base_class, u8 sub_class, u8 prog_if,
                   struct rhap_pci_dev *out, int max);
u32  rhap_pci_read32(const struct rhap_pci_dev *d, u8 off);
u16  rhap_pci_read16(const struct rhap_pci_dev *d, u8 off);
void rhap_pci_write32(const struct rhap_pci_dev *d, u8 off, u32 val);
void rhap_pci_write16(const struct rhap_pci_dev *d, u8 off, u16 val);

/* ------------------------------------------------------------------ *
 * 4. Time and locking.
 * ------------------------------------------------------------------ */
u64  rhap_uptime_us(void);
void rhap_udelay(u32 usec);

typedef volatile u32 rhap_lock_t;    /* 0 = free, 1 = held */

void rhap_lock_init(rhap_lock_t *l);
void rhap_lock(rhap_lock_t *l);
void rhap_unlock(rhap_lock_t *l);

/* ------------------------------------------------------------------ *
 * 5. Timers.  One repeating timer drives the whole stack.
 * On DR2: a kernel timeout()/bsd_timeout() that re-arms itself, or a
 * kthread that nanosleep()s.  Must run at a safe (non-interrupt) context.
 * ------------------------------------------------------------------ */
typedef void (*rhap_timer_fn)(void *arg);
void *rhap_timer_start(u32 period_us, rhap_timer_fn fn, void *arg);
void  rhap_timer_stop(void *timer);

/* ------------------------------------------------------------------ *
 * 6. Logging.  level: 0 = error, 1 = info, 2 = debug.
 * ------------------------------------------------------------------ */
void rhap_log(int level, const char *fmt, ...);

/* ------------------------------------------------------------------ *
 * 7. Input injection, in the form the upper layers of Rhapsody expect.
 *
 * Rhapsody's event system is ADB-centric even on x86: the PS/2 driver
 * translates scancodes into the classic Apple keycodes before queueing
 * events.  So a USB keyboard driver for this OS must produce ADB keycodes
 * (0x00 = A ... 0x7E = up arrow), not ASCII and not HID usages.
 * hid_boot.c does that translation; the port layer only has to push the
 * code into the event queue (ev_keymap / ADB kbd queue / PS2 glue).
 *
 * buttons: bit 0 = left, bit 1 = right, bit 2 = middle.
 * dx/dy are raw relative deltas, +y = away from the user (USB convention);
 * negate dy in the port layer if your event driver uses screen coordinates.
 * ------------------------------------------------------------------ */
void rhap_input_key(u8 adb_keycode, int down);
void rhap_input_mouse(s32 dx, s32 dy, u8 buttons);

#ifdef __cplusplus
}
#endif

#endif /* _RHAP_PORT_H */
