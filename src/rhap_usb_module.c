/*
 * rhap_usb_module.c -- module entry points and the 1 ms pump.
 *
 * This file is the seam between the portable stack and Rhapsody's driver
 * loading.  The DriverKit wrapper (DriverKit/rhapusb_iodriver.cpp) calls
 * rhap_usb_init()/rhap_usb_shutdown(); everything below is plain C so it
 * can be unit tested on a host.
 *
 * Hot plug is handled by polling the root ports' connect-change bits.  There
 * is no hub driver, so devices behind a hub are not seen -- see README.
 */

#include "rhap_types.h"
#include "rhap_port.h"
#include "rhap_usb.h"
#include "rhap_uhci.h"

extern int usb_hid_init(void);

/* Public entry points, also declared by the DriverKit wrapper. */
int  rhap_usb_init(void);
void rhap_usb_shutdown(void);
int  rhap_usb_is_started(void);

#define RESCAN_TICKS 250      /* check root ports every 250 ms */

struct ctrl {
    struct uhci *h;
    struct usb_bus *bus;
    struct usb_device *port_dev[UHCI_MAX_PORTS];
};

static struct ctrl g_ctrl[USB_MAX_HCD];
static int g_nctrl;
static int g_started;
static void *g_timer;
static u32 g_ticks;

/* ---------------------------------------------------------------------- *
 * Port attach/detach
 * ---------------------------------------------------------------------- */
static void ctrl_plug(struct ctrl *c, int port)
{
    struct usb_device *dev;

    if (uhci_port_reset(c->h, port) != 0) {
        rhap_log(0, "usb: port %d reset failed", port + 1);
        return;
    }

    dev = usb_bus_add_device(c->bus, (u8)uhci_port_speed(c->h, port));
    if (!dev) {
        rhap_log(0, "usb: no room for another device");
        return;
    }
    if (usb_enumerate(dev) != USB_OK) {
        rhap_log(0, "usb: enumeration failed on port %d", port + 1);
        usb_device_remove(dev);
        return;
    }
    c->port_dev[port] = dev;
    rhap_log(1, "usb: port %d: %04x:%04x ready", port + 1,
             dev->id_vendor, dev->id_product);
}

static void ctrl_unplug(struct ctrl *c, int port)
{
    if (!c->port_dev[port])
        return;
    rhap_log(1, "usb: port %d disconnected", port + 1);
    usb_device_remove(c->port_dev[port]);
    c->port_dev[port] = RHAP_NULL;
}

static void ctrl_rescan(struct ctrl *c)
{
    int p;

    for (p = 0; p < c->h->nports; p++) {
        u16 ps = uhci_port_status(c->h, p);

        if (!(ps & PORTSC_CSC))
            continue;
        uhci_port_ack(c->h, p);
        if ((ps & PORTSC_CCS) && !c->port_dev[p])
            ctrl_plug(c, p);
        else if (!(ps & PORTSC_CCS) && c->port_dev[p])
            ctrl_unplug(c, p);
    }
}

/* ---------------------------------------------------------------------- *
 * Pump
 * ---------------------------------------------------------------------- */
static void usb_pump(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < g_nctrl; i++) {
        uhci_service(g_ctrl[i].h);
        usb_poll_drivers(g_ctrl[i].bus);
    }

    if (++g_ticks >= RESCAN_TICKS) {
        g_ticks = 0;
        for (i = 0; i < g_nctrl; i++)
            ctrl_rescan(&g_ctrl[i]);
    }
}

/* ---------------------------------------------------------------------- *
 * Init / shutdown
 * ---------------------------------------------------------------------- */
int rhap_usb_init(void)
{
    struct rhap_pci_dev pci[USB_MAX_HCD];
    int n, i, p;

    if (g_started)
        return 0;

    usb_core_init();
    if (usb_hid_init() != USB_OK) {
        rhap_log(0, "usb: cannot register HID driver");
        return -1;
    }

    /*
     * UHCI is PCI class 0x0c (serial bus), subclass 0x03 (USB),
     * programming interface 0x00.  OHCI is 0x10 and is not supported.
     */
    n = rhap_pci_find(0x0C, 0x03, 0x00, pci, USB_MAX_HCD);
    if (n <= 0) {
        rhap_log(1, "usb: no UHCI controller present");
        return -1;
    }

    for (i = 0; i < n; i++) {
        struct uhci *h;
        struct usb_bus *bus;

        h = uhci_create(&pci[i]);
        if (!h)
            continue;
        bus = usb_bus_alloc(&uhci_hcd_ops, h);
        if (!bus) {
            uhci_destroy(h);
            continue;
        }

        g_ctrl[g_nctrl].h = h;
        g_ctrl[g_nctrl].bus = bus;
        for (p = 0; p < UHCI_MAX_PORTS; p++)
            g_ctrl[g_nctrl].port_dev[p] = RHAP_NULL;
        g_nctrl++;

        /* Cold-boot scan: no change bits are pending on a fresh controller. */
        for (p = 0; p < h->nports; p++) {
            if (uhci_port_connected(h, p))
                ctrl_plug(&g_ctrl[g_nctrl - 1], p);
        }
    }

    if (g_nctrl == 0) {
        rhap_log(0, "usb: no usable controller");
        return -1;
    }

    g_timer = rhap_timer_start(1000, usb_pump, RHAP_NULL);
    if (!g_timer) {
        rhap_log(0, "usb: cannot start pump timer");
        rhap_usb_shutdown();
        return -1;
    }

    g_started = 1;
    rhap_log(1, "usb: %d controller(s) running", g_nctrl);
    return 0;
}

void rhap_usb_shutdown(void)
{
    int i, p;

    if (g_timer) {
        rhap_timer_stop(g_timer);
        g_timer = RHAP_NULL;
    }
    for (i = 0; i < g_nctrl; i++) {
        for (p = 0; p < UHCI_MAX_PORTS; p++)
            ctrl_unplug(&g_ctrl[i], p);
        usb_bus_free(g_ctrl[i].bus);
        uhci_destroy(g_ctrl[i].h);
        g_ctrl[i].h = RHAP_NULL;
        g_ctrl[i].bus = RHAP_NULL;
    }
    g_nctrl = 0;
    g_ticks = 0;
    g_started = 0;
}

/* Called by the DriverKit wrapper's start()/stop(). */
int rhap_usb_is_started(void)
{
    return g_started;
}
