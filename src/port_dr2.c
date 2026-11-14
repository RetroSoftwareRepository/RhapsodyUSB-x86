/*
 * port_dr2.c -- the Rhapsody DR2 side of rhap_port.h.
 *
 * Only this file contains OS-specific code.  It is deliberately boring and
 * small: port I/O, PCI configuration space, DMA allocation, a timer and the
 * input-injection hook.
 *
 * WHAT IS CERTAIN HERE
 *   - the x86 port I/O and PCI mechanism #1 sequences
 *   - the atomic used for rhap_lock_t
 *
 * WHAT YOU MUST VERIFY AGAINST YOUR DDK
 *   - IOMallocContiguous / IOFreeContiguous signatures
 *   - pmap_extract() return type
 *   - the event driver entry point used by rhap_input_key/mouse
 *   - whether module code may call kprintf()
 *
 * Build with -DRHAP_DR2_BUILD.  Without it this file compiles to nothing,
 * which is what the host test build wants.
 */

#include "rhap_types.h"
#include "rhap_port.h"

#if defined(RHAP_DR2_BUILD)

#include <stdarg.h>

/* ---------------------------------------------------------------------- *
 * Kernel primitives.
 *
 * Declared here rather than via headers so that the file does not depend on
 * the exact layout of the DR2 SDK.  If your DDK already declares one of
 * these, drop the line -- the compiler will tell you.
 * ---------------------------------------------------------------------- */
extern void  kprintf(const char *fmt, ...);
extern void  delay(int usec);
extern void  IOSleep(unsigned int ms);

/* struct timeval is two longs on 32-bit BSD; declared locally so this file
 * does not depend on <sys/time.h> being visible to kernel modules. */
struct rhap_timeval {
    long tv_sec;
    long tv_usec;
};
extern void  microuptime(struct rhap_timeval *tv);
extern int   vsnprintf(char *buf, size_t size, const char *fmt,
                       va_list ap);

typedef struct pmap *pmap_t;
extern pmap_t kernel_pmap;
extern unsigned long pmap_extract(pmap_t pmap, unsigned long vaddr);

extern void *IOMalloc(unsigned int size);
extern void  IOFree(void *addr, unsigned int size);
extern void *IOMallocContiguous(unsigned int size, unsigned int alignment,
                                unsigned long *physical);
extern void  IOFreeContiguous(void *addr, unsigned int size);

typedef void (*rhap_timeout_fcn_t)(void *);
extern void timeout(rhap_timeout_fcn_t fcn, void *arg, int ticks);
extern void untimeout(rhap_timeout_fcn_t fcn, void *arg);
extern int  hz;

/* The event driver.  Replace with the real prototypes once located. */
extern void kbd_enqueue_event(unsigned char keycode, int keydown);
extern void mouse_enqueue_event(int dx, int dy, unsigned char buttons);

/* ---------------------------------------------------------------------- *
 * Port I/O
 * ---------------------------------------------------------------------- */
u8 rhap_in8(u16 port)
{
    u8 v;

    __asm__ __volatile__("inb %w1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

void rhap_out8(u16 port, u8 val)
{
    __asm__ __volatile__("outb %0,%w1" :: "a"(val), "Nd"(port));
}

u16 rhap_in16(u16 port)
{
    u16 v;

    __asm__ __volatile__("inw %w1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

void rhap_out16(u16 port, u16 val)
{
    __asm__ __volatile__("outw %0,%w1" :: "a"(val), "Nd"(port));
}

u32 rhap_in32(u16 port)
{
    u32 v;

    __asm__ __volatile__("inl %w1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

void rhap_out32(u16 port, u32 val)
{
    __asm__ __volatile__("outl %0,%w1" :: "a"(val), "Nd"(port));
}

/* ---------------------------------------------------------------------- *
 * PCI configuration space, mechanism #1.
 * ---------------------------------------------------------------------- */
#define PCI_ADDR  0xCF8
#define PCI_DATA  0xCFC

static u32 pci_addr(u8 bus, u8 slot, u8 func, u8 off)
{
    return 0x80000000u |
           ((u32)bus << 16) | ((u32)(slot & 0x1F) << 11) |
           ((u32)(func & 0x07) << 8) | (u32)(off & 0xFC);
}

u32 rhap_pci_read32(const struct rhap_pci_dev *d, u8 off)
{
    rhap_out32(PCI_ADDR, pci_addr(d->bus, d->slot, d->func, off));
    return rhap_in32(PCI_DATA);
}

u16 rhap_pci_read16(const struct rhap_pci_dev *d, u8 off)
{
    u32 v = rhap_pci_read32(d, off);

    return (u16)((off & 2) ? (v >> 16) : (v & 0xFFFF));
}

void rhap_pci_write32(const struct rhap_pci_dev *d, u8 off, u32 val)
{
    rhap_out32(PCI_ADDR, pci_addr(d->bus, d->slot, d->func, off));
    rhap_out32(PCI_DATA, val);
}

void rhap_pci_write16(const struct rhap_pci_dev *d, u8 off, u16 val)
{
    u32 cur = rhap_pci_read32(d, off);

    if (off & 2)
        cur = (cur & 0x0000FFFFu) | ((u32)val << 16);
    else
        cur = (cur & 0xFFFF0000u) | val;
    rhap_pci_write32(d, off, cur);
}

int rhap_pci_find(u8 base_class, u8 sub_class, u8 prog_if,
                  struct rhap_pci_dev *out, int max)
{
    int found = 0;
    int bus, slot, func;

    for (bus = 0; bus < 256 && found < max; bus++) {
        for (slot = 0; slot < 32 && found < max; slot++) {
            for (func = 0; func < 8 && found < max; func++) {
                struct rhap_pci_dev d;
                u32 id, cls;

                d.bus = (u8)bus;
                d.slot = (u8)slot;
                d.func = (u8)func;
                id = rhap_pci_read32(&d, 0x00);
                if ((id & 0xFFFF) == 0xFFFF)
                    continue;               /* nothing there */
                cls = rhap_pci_read32(&d, 0x08);
                if ((u8)(cls >> 24) != base_class ||
                    (u8)(cls >> 16) != sub_class ||
                    (u8)(cls >> 8) != prog_if)
                    continue;
                d.vendor = (u16)(id & 0xFFFF);
                d.device = (u16)(id >> 16);
                d.irq = (u8)(rhap_pci_read32(&d, 0x3C) & 0xFF);
                out[found++] = d;
            }
        }
    }
    return found;
}

/* ---------------------------------------------------------------------- *
 * Memory
 * ---------------------------------------------------------------------- */
void *rhap_dma_alloc(u32 bytes, u32 align, rhap_paddr_t *pa_out)
{
    unsigned long phys = 0;
    void *p;

    if (align < 16)
        align = 16;
    p = IOMallocContiguous(bytes, align, &phys);
    if (p && pa_out)
        *pa_out = (rhap_paddr_t)phys;
    return p;
}

void rhap_dma_free(void *va, u32 bytes)
{
    if (va)
        IOFreeContiguous(va, bytes);
}

void *rhap_mem_alloc(u32 bytes)
{
    return IOMalloc(bytes);
}

void rhap_mem_free(void *va, u32 bytes)
{
    if (va)
        IOFree(va, bytes);
}

rhap_paddr_t rhap_kvtophys(void *va)
{
    /* DR2 is ILP32, so the cast through size_t is lossless there. */
    return (rhap_paddr_t)pmap_extract(kernel_pmap, (unsigned long)(size_t)va);
}

/* ---------------------------------------------------------------------- *
 * Time
 * ---------------------------------------------------------------------- */
u64 rhap_uptime_us(void)
{
    struct rhap_timeval tv;

    microuptime(&tv);
    return (u64)tv.tv_sec * 1000000u + (u64)tv.tv_usec;
}

void rhap_udelay(u32 usec)
{
    if (usec >= 1000)
        IOSleep(usec / 1000);
    else
        delay((int)usec);
}

/* ---------------------------------------------------------------------- *
 * Locking: a single atomic word.  The driver takes it for microseconds at
 * a time and never sleeps while holding it.
 * ---------------------------------------------------------------------- */
static u32 rhap_xchg(volatile u32 *p, u32 v)
{
    u32 old;

    __asm__ __volatile__("xchgl %0,%1"
                         : "=r"(old), "+m"(*p)
                         : "0"(v)
                         : "memory");
    return old;
}

void rhap_lock_init(rhap_lock_t *l)
{
    *l = 0;
}

void rhap_lock(rhap_lock_t *l)
{
    while (rhap_xchg(l, 1) != 0) {
        while (*l)
            ;                       /* spin; critical sections are tiny */
    }
}

void rhap_unlock(rhap_lock_t *l)
{
    __asm__ __volatile__("" ::: "memory");
    *l = 0;
}

/* ---------------------------------------------------------------------- *
 * Timer.
 *
 * A self-rearming BSD timeout.  Note that timeout callbacks run in the
 * kernel's softclock context, where sleeping is illegal.  The pump only
 * touches MMIO and device buffers, so that is fine -- EXCEPT for
 * rhap_input_key() below, which enters the event driver.
 *
 * If your event driver cannot be called from softclock (likely), build with
 * -DRHAP_PUMP_THREAD and implement rhap_timer_start() with a kernel thread
 * that does:  for (;;) { IOSleep(1); fn(arg); }
 * ---------------------------------------------------------------------- */
struct rhap_timer_rec {
    int  active;
    int  ticks;
    rhap_timer_fn fn;
    void *arg;
};

static struct rhap_timer_rec g_timer;

static void timer_trampoline(void *arg)
{
    struct rhap_timer_rec *t = (struct rhap_timer_rec *)arg;

    if (!t->active)
        return;
    t->fn(t->arg);
    if (t->active)
        timeout(timer_trampoline, t, t->ticks);
}

void *rhap_timer_start(u32 period_us, rhap_timer_fn fn, void *arg)
{
    int ticks = (int)((period_us * hz) / 1000000u);

    if (ticks < 1)
        ticks = 1;
    if (g_timer.active)
        return RHAP_NULL;
    g_timer.active = 1;
    g_timer.ticks = ticks;
    g_timer.fn = fn;
    g_timer.arg = arg;
    timeout(timer_trampoline, &g_timer, ticks);
    return &g_timer;
}

void rhap_timer_stop(void *timer)
{
    struct rhap_timer_rec *t = (struct rhap_timer_rec *)timer;

    if (!t)
        return;
    t->active = 0;
    untimeout(timer_trampoline, t);
}

/* ---------------------------------------------------------------------- *
 * Logging.  The DriverKit build may not link kprintf; IOLog is the
 * alternative in that case.
 * ---------------------------------------------------------------------- */
void rhap_log(int level, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    if (level > 2)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kprintf("usb: %s\n", buf);
}

/* ---------------------------------------------------------------------- *
 * Input injection.
 *
 * This is the one place where a wrong guess costs you a working keyboard,
 * so it is a two-line hook rather than a pile of speculative code.  The
 * codes handed in are classic ADB keycodes; Rhapsody's event driver speaks
 * that language even on x86.
 * ---------------------------------------------------------------------- */
void rhap_input_key(u8 adb_keycode, int down)
{
    kbd_enqueue_event(adb_keycode, down ? 1 : 0);
}

void rhap_input_mouse(s32 dx, s32 dy, u8 buttons)
{
    mouse_enqueue_event((int)dx, (int)dy, buttons);
}

#endif /* RHAP_DR2_BUILD */
