/*
 * port_host.c -- POSIX/Windows implementation of rhap_port.h.
 *
 * This exists so the stack's logic (enumeration, descriptor parsing, HID
 * decoding, transfer descriptor encoding) can be compiled and exercised on
 * a development machine.  It is NOT a Rhapsody port: port I/O and PCI
 * configuration space are inert here, so uhci.c can be compiled but not run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <stdarg.h>
#include <windows.h>

#include "rhap_types.h"
#include "rhap_port.h"

/* ------------------------------------------------------------------ *
 * Event capture, read by the tests.
 * ------------------------------------------------------------------ */
struct host_event {
    int kind;      /* 1 = key, 2 = mouse */
    int a, b, c;
};

#define HOST_MAX_EV 512
struct host_event host_events[HOST_MAX_EV];
int host_nev;

static void ev_push(int kind, int a, int b, int c)
{
    if (host_nev < HOST_MAX_EV) {
        host_events[host_nev].kind = kind;
        host_events[host_nev].a = a;
        host_events[host_nev].b = b;
        host_events[host_nev].c = c;
        host_nev++;
    }
}

void rhap_input_key(u8 adb_code, int down)
{
    ev_push(1, adb_code, down, 0);
}

void rhap_input_mouse(s32 dx, s32 dy, u8 buttons)
{
    ev_push(2, dx, dy, buttons);
}

/* ------------------------------------------------------------------ *
 * Memory
 * ------------------------------------------------------------------ */
void *rhap_dma_alloc(u32 bytes, u32 align, rhap_paddr_t *pa_out)
{
    void *p;

    if (align < 16)
        align = 16;
    p = _aligned_malloc(bytes, align);
    if (p && pa_out)
        *pa_out = (rhap_paddr_t)(size_t)p;
    return p;
}

void rhap_dma_free(void *va, u32 bytes)
{
    (void)bytes;
    if (va)
        _aligned_free(va);
}

rhap_paddr_t rhap_kvtophys(void *va)
{
    return (rhap_paddr_t)(size_t)va;
}

void *rhap_mem_alloc(u32 bytes)
{
    return malloc(bytes);
}

void rhap_mem_free(void *va, u32 bytes)
{
    (void)bytes;
    free(va);
}

/* ------------------------------------------------------------------ *
 * Port I/O: inert.  Nothing in the host tests drives a real controller.
 * ------------------------------------------------------------------ */
u8 rhap_in8(u16 port)              { (void)port; return 0; }
void rhap_out8(u16 port, u8 val)   { (void)port; (void)val; }
u16 rhap_in16(u16 port)            { (void)port; return 0; }
void rhap_out16(u16 port, u16 val) { (void)port; (void)val; }
u32 rhap_in32(u16 port)            { (void)port; return 0xFFFFFFFFu; }
void rhap_out32(u16 port, u32 val) { (void)port; (void)val; }

/* ------------------------------------------------------------------ *
 * PCI: report nothing, so the module glue finds no controller.
 * ------------------------------------------------------------------ */
int rhap_pci_find(u8 base_class, u8 sub_class, u8 prog_if,
                  struct rhap_pci_dev *out, int max)
{
    (void)base_class;
    (void)sub_class;
    (void)prog_if;
    (void)out;
    (void)max;
    return 0;
}

u32 rhap_pci_read32(const struct rhap_pci_dev *d, u8 off)
{
    (void)d;
    (void)off;
    return 0;
}

u16 rhap_pci_read16(const struct rhap_pci_dev *d, u8 off)
{
    (void)d;
    (void)off;
    return 0;
}

void rhap_pci_write32(const struct rhap_pci_dev *d, u8 off, u32 val)
{
    (void)d;
    (void)off;
    (void)val;
}

void rhap_pci_write16(const struct rhap_pci_dev *d, u8 off, u16 val)
{
    (void)d;
    (void)off;
    (void)val;
}

/* ------------------------------------------------------------------ *
 * Time
 * ------------------------------------------------------------------ */
u64 rhap_uptime_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;

    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (u64)((now.QuadPart * 1000000) / freq.QuadPart);
}

void rhap_udelay(u32 usec)
{
    (void)usec;          /* tests want speed, not realism */
}

/* ------------------------------------------------------------------ *
 * Locks
 * ------------------------------------------------------------------ */
void rhap_lock_init(rhap_lock_t *l)
{
    *l = 0;
}

void rhap_lock(rhap_lock_t *l)
{
    while (InterlockedExchange((volatile LONG *)l, 1) != 0)
        Sleep(0);
}

void rhap_unlock(rhap_lock_t *l)
{
    InterlockedExchange((volatile LONG *)l, 0);
}

/* ------------------------------------------------------------------ *
 * Timers: the test harness calls the pump by hand.
 * ------------------------------------------------------------------ */
void *rhap_timer_start(u32 period_us, rhap_timer_fn fn, void *arg)
{
    (void)period_us;
    (void)fn;
    (void)arg;
    return (void *)1;
}

void rhap_timer_stop(void *timer)
{
    (void)timer;
}

/* ------------------------------------------------------------------ *
 * Logging
 * ------------------------------------------------------------------ */
void rhap_log(int level, const char *fmt, ...)
{
    va_list ap;

    if (getenv("RHAP_VERBOSE")) {
        printf("[%d] ", level);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }
}
