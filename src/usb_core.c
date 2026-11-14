/*
 * usb_core.c -- bus bookkeeping, enumeration and driver binding.
 *
 * Deliberately small.  The only clever part is that enumeration never
 * trusts a descriptor it has not bounds-checked, because a malformed
 * descriptor on a 1998 keyboard would otherwise walk off the end of the
 * buffer and panic the machine during boot.
 */

#include "rhap_types.h"
#include "rhap_port.h"
#include "rhap_usb.h"

/* ---------------------------------------------------------------------- *
 * Globals
 * ---------------------------------------------------------------------- */
static struct usb_bus  g_buses[USB_MAX_BUSES];
static const struct usb_class_driver *g_class[USB_MAX_CLASS];
static int g_nclass;

void usb_core_init(void)
{
    int i;

    for (i = 0; i < USB_MAX_BUSES; i++) {
        g_buses[i].used = 0;
        g_buses[i].ndev = 0;
        g_buses[i].next_addr = 1;
        g_buses[i].ops = RHAP_NULL;
        g_buses[i].ctx = RHAP_NULL;
    }
    for (i = 0; i < USB_MAX_CLASS; i++)
        g_class[i] = RHAP_NULL;
    g_nclass = 0;
}

int usb_register_class_driver(const struct usb_class_driver *drv)
{
    int i;

    if (!drv || !drv->match || !drv->attach)
        return USB_ERR_PROTO;
    for (i = 0; i < g_nclass; i++)
        if (g_class[i] == drv)
            return USB_OK;
    if (g_nclass >= USB_MAX_CLASS)
        return USB_ERR_NOMEM;
    g_class[g_nclass++] = drv;
    return USB_OK;
}

/* ---------------------------------------------------------------------- *
 * Bus / device lifecycle
 * ---------------------------------------------------------------------- */
struct usb_bus *usb_bus_alloc(const struct usb_hcd_ops *ops, void *ctx)
{
    int i;

    if (!ops)
        return RHAP_NULL;
    for (i = 0; i < USB_MAX_BUSES; i++) {
        struct usb_bus *b = &g_buses[i];

        if (b->used)
            continue;
        b->used = 1;
        b->ndev = 0;
        b->next_addr = 1;
        b->ops = ops;
        b->ctx = ctx;
        rhap_lock_init(&b->lock);
        return b;
    }
    return RHAP_NULL;
}

static u8 usb_alloc_address(struct usb_bus *bus)
{
    u8 a;
    int i, used;

    for (a = 1; a < 127; a++) {
        used = 0;
        for (i = 0; i < bus->ndev; i++)
            if (bus->dev[i] && bus->dev[i]->addr == a)
                used = 1;
        if (!used)
            return a;
    }
    return 0;
}

struct usb_device *usb_bus_add_device(struct usb_bus *bus, u8 speed)
{
    struct usb_device *dev;
    int i;

    if (!bus || !bus->ops)
        return RHAP_NULL;
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (bus->dev[i])
            continue;
        dev = (struct usb_device *)rhap_mem_alloc(sizeof *dev);
        if (!dev)
            return RHAP_NULL;
        dev->bus = bus;
        dev->addr = 0;
        dev->speed = speed;
        dev->ep0_max = 8;
        dev->configured = 0;
        dev->id_vendor = 0;
        dev->id_product = 0;
        dev->n_attached = 0;
        dev->cfg.nif = 0;
        bus->dev[i] = dev;
        bus->ndev++;
        return dev;
    }
    return RHAP_NULL;
}

static void usb_detach_drivers(struct usb_device *dev)
{
    int i;

    for (i = 0; i < dev->cfg.nif; i++) {
        struct usb_interface *intf = &dev->cfg.intf[i];

        if (intf->driver && intf->driver->detach)
            intf->driver->detach(dev, intf);
        intf->driver = RHAP_NULL;
        intf->driver_data = RHAP_NULL;
    }
}

void usb_device_remove(struct usb_device *dev)
{
    struct usb_bus *bus;
    int i;

    if (!dev)
        return;
    bus = dev->bus;
    if (bus) {
        rhap_lock(&bus->lock);
        usb_detach_drivers(dev);
        for (i = 0; i < USB_MAX_DEVICES; i++) {
            if (bus->dev[i] == dev) {
                bus->dev[i] = RHAP_NULL;
                bus->ndev--;
                break;
            }
        }
        rhap_unlock(&bus->lock);
    }
    rhap_mem_free(dev, sizeof *dev);
}

void usb_bus_free(struct usb_bus *bus)
{
    int i;

    if (!bus)
        return;
    for (i = 0; i < USB_MAX_DEVICES; i++)
        if (bus->dev[i])
            usb_device_remove(bus->dev[i]);
    bus->used = 0;
    bus->ops = RHAP_NULL;
    bus->ctx = RHAP_NULL;
}

/* ---------------------------------------------------------------------- *
 * Control transfers
 * ---------------------------------------------------------------------- */
int usb_control_msg(struct usb_device *dev, u8 req_type, u8 req,
                    u16 value, u16 index, void *data, u16 len,
                    u32 timeout_ms)
{
    struct usb_setup setup RHAP_ALIGN(16);
    int rc;

    if (!dev || !dev->bus || !dev->bus->ops || !dev->bus->ops->control)
        return USB_ERR_NODEV;
    setup.bmRequestType = req_type;
    setup.bRequest = req;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = len;

    rc = dev->bus->ops->control(dev->bus->ctx, dev->addr, dev->ep0_max,
                                dev->speed, &setup, data, len, timeout_ms);
    if (rc < 0)
        return rc;
    if (len == 0)
        return 0;
    return (rc > (int)len) ? (int)len : rc;
}

int usb_get_descriptor(struct usb_device *dev, u8 type, u8 index,
                       u16 langid, void *buf, u16 len)
{
    return usb_control_msg(dev, USB_DIR_IN | USB_TYPE_STANDARD |
                                USB_RECIP_DEVICE,
                           USB_REQ_GET_DESCRIPTOR,
                           (u16)((u16)type << 8) | index, langid,
                           buf, len, 1000);
}

int usb_set_address(struct usb_device *dev, u8 addr)
{
    return usb_control_msg(dev, USB_DIR_OUT | USB_TYPE_STANDARD |
                                USB_RECIP_DEVICE,
                           USB_REQ_SET_ADDRESS, (u16)addr, 0,
                           RHAP_NULL, 0, 1000);
}

int usb_set_configuration(struct usb_device *dev, u8 value)
{
    return usb_control_msg(dev, USB_DIR_OUT | USB_TYPE_STANDARD |
                                USB_RECIP_DEVICE,
                           USB_REQ_SET_CONFIGURATION, (u16)value, 0,
                           RHAP_NULL, 0, 1000);
}

/* ---------------------------------------------------------------------- *
 * Pure descriptor parser
 * ---------------------------------------------------------------------- */
static void usb_zero(void *p, u32 n)
{
    u8 *b = (u8 *)p;
    while (n--)
        *b++ = 0;
}

int usb_parse_config(const u8 *buf, int len, struct usb_configuration *out)
{
    int off;
    int have_config;
    struct usb_interface *cur;

    if (!buf || !out || len < 9)
        return USB_ERR_PROTO;

    usb_zero(out, sizeof *out);
    off = 0;
    have_config = 0;
    cur = RHAP_NULL;

    while (off + 2 <= len) {
        int blen = buf[off];
        int btype = buf[off + 1];

        /*
         * A zero or short length would spin forever on a corrupt device.
         * Reject the whole configuration instead of skipping: silently
         * mis-parsed endpoints are worse than a failed attach.
         */
        if (blen < 2 || off + blen > len)
            return USB_ERR_PROTO;

        if (btype == USB_DT_CONFIG) {
            const struct usb_config_desc *cd;

            if (blen < 9)
                return USB_ERR_PROTO;
            cd = (const struct usb_config_desc *)(const void *)(buf + off);
            out->value = cd->bConfigurationValue;
            out->attr = cd->bmAttributes;
            out->maxpower = cd->bMaxPower;
            have_config = 1;
        } else if (btype == USB_DT_INTERFACE) {
            struct usb_interface *in;

            if (blen < 9)
                return USB_ERR_PROTO;
            if (out->nif >= USB_MAX_IF)
                return USB_ERR_NOMEM;
            in = &out->intf[out->nif++];
            in->number = buf[off + 2];
            in->alt = buf[off + 3];
            in->class = buf[off + 5];
            in->subclass = buf[off + 6];
            in->proto = buf[off + 7];
            in->nep = 0;
            in->driver = RHAP_NULL;
            in->driver_data = RHAP_NULL;
            cur = in;
        } else if (btype == USB_DT_ENDPOINT) {
            struct usb_endpoint *ep;

            if (blen < 7)
                return USB_ERR_PROTO;
            if (!cur || cur->nep >= USB_MAX_EP)
                return USB_ERR_PROTO;
            ep = &cur->ep[cur->nep++];
            ep->addr = buf[off + 2];
            ep->type = (u8)(buf[off + 3] & USB_EP_TYPE_MASK);
            ep->maxpkt = (u16)(buf[off + 4] | ((u16)buf[off + 5] << 8));
            ep->maxpkt &= 0x07FF;          /* strip the high-bandwidth bits */
            ep->interval = buf[off + 6];
            ep->hcd = RHAP_NULL;
            if (ep->maxpkt == 0 || ep->maxpkt > 1024)
                return USB_ERR_PROTO;
        }
        /* everything else (HID, class-specific, CS_ENDPOINT) is skipped */
        off += blen;
    }

    if (!have_config || out->nif == 0)
        return USB_ERR_PROTO;
    return USB_OK;
}

/* ---------------------------------------------------------------------- *
 * Driver binding
 * ---------------------------------------------------------------------- */
static void usb_attach_drivers(struct usb_device *dev)
{
    int i, c;

    for (i = 0; i < dev->cfg.nif; i++) {
        struct usb_interface *intf = &dev->cfg.intf[i];

        for (c = 0; c < g_nclass; c++) {
            const struct usb_class_driver *drv = g_class[c];

            if (!drv->match(dev, intf))
                continue;
            intf->driver = drv;
            if (drv->attach(dev, intf) != 0) {
                intf->driver = RHAP_NULL;
                rhap_log(0, "usb: %s attach failed for interface %d",
                         drv->name, intf->number);
                continue;
            }
            dev->n_attached++;
            break;
        }
    }
    if (dev->n_attached == 0)
        rhap_log(1, "usb: device %04x:%04x enumerated, no driver claimed it",
                 dev->id_vendor, dev->id_product);
}

void usb_poll_drivers(struct usb_bus *bus)
{
    int d, i;

    if (!bus)
        return;
    for (d = 0; d < USB_MAX_DEVICES; d++) {
        struct usb_device *dev = bus->dev[d];

        if (!dev || !dev->configured)
            continue;
        for (i = 0; i < dev->cfg.nif; i++) {
            struct usb_interface *intf = &dev->cfg.intf[i];

            if (intf->driver && intf->driver->poll)
                intf->driver->poll(dev, intf);
        }
    }
}

/* ---------------------------------------------------------------------- *
 * Enumeration
 * ---------------------------------------------------------------------- */
int usb_enumerate(struct usb_device *dev)
{
    struct usb_bus *bus;
    struct usb_device_desc dd;
    struct usb_config_desc cd;
    u8 newaddr;
    int n, rc, total;

    if (!dev || !dev->bus)
        return USB_ERR_NODEV;
    bus = dev->bus;

    dev->addr = 0;
    dev->ep0_max = 8;
    dev->configured = 0;

    /* 1. First 8 bytes at address 0, packet size 8: the universal hello. */
    n = usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0, &dd, 8);
    if (n < 8) {
        rhap_log(0, "usb: no device descriptor (%d)", n);
        return (n < 0) ? n : USB_ERR_PROTO;
    }
    if (dd.bDescriptorType != USB_DT_DEVICE) {
        rhap_log(0, "usb: bad device descriptor type 0x%02x",
                 dd.bDescriptorType);
        return USB_ERR_PROTO;
    }

    /* 2. Assign an address.  The device switches after the status stage. */
    newaddr = usb_alloc_address(bus);
    if (newaddr == 0)
        return USB_ERR_NOMEM;
    rc = usb_set_address(dev, newaddr);
    if (rc < 0) {
        rhap_log(0, "usb: SET_ADDRESS failed (%d)", rc);
        return rc;
    }
    dev->addr = newaddr;
    rhap_udelay(2000);          /* USB 1.1 9.2.6.3: 2 ms recovery */

    /* 3. Full device descriptor, now that we can talk at the new address. */
    n = usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0,
                           &dd, (u16)sizeof dd);
    if (n < 18) {
        rhap_log(0, "usb: short device descriptor (%d)", n);
        return (n < 0) ? n : USB_ERR_PROTO;
    }
    dev->id_vendor = dd.idVendor;
    dev->id_product = dd.idProduct;
    if (dd.bMaxPacketSize0 == 8 || dd.bMaxPacketSize0 == 16 ||
        dd.bMaxPacketSize0 == 32 || dd.bMaxPacketSize0 == 64)
        dev->ep0_max = dd.bMaxPacketSize0;

    rhap_log(1, "usb: addr %d: %04x:%04x USB %x.%02x ep0 %d",
             dev->addr, dd.idVendor, dd.idProduct,
             dd.bcdUSB >> 8, dd.bcdUSB & 0xFF, dev->ep0_max);

    /* 4. Configuration header tells us how much more to ask for. */
    n = usb_get_descriptor(dev, USB_DT_CONFIG, 0, 0, &cd,
                           (u16)sizeof cd);
    if (n < 9 || cd.bDescriptorType != USB_DT_CONFIG) {
        rhap_log(0, "usb: no configuration descriptor");
        return USB_ERR_PROTO;
    }
    total = cd.wTotalLength;
    if (total < 9 || total > USB_DESC_MAX) {
        rhap_log(0, "usb: implausible config length %d", total);
        return USB_ERR_PROTO;
    }

    /* 5. Whole configuration, with interfaces and endpoints. */
    n = usb_get_descriptor(dev, USB_DT_CONFIG, 0, 0, bus->desc, (u16)total);
    if (n < 9) {
        rhap_log(0, "usb: short config descriptor (%d)", n);
        return (n < 0) ? n : USB_ERR_PROTO;
    }
    if (n < total)
        rhap_log(1, "usb: config truncated %d of %d bytes", n, total);

    rc = usb_parse_config(bus->desc, n, &dev->cfg);
    if (rc != USB_OK) {
        rhap_log(0, "usb: config parse failed (%d)", rc);
        return rc;
    }

    /* 6. Select configuration 1 (or whatever the device calls it). */
    rc = usb_set_configuration(dev, dev->cfg.value ? dev->cfg.value : 1);
    if (rc < 0) {
        rhap_log(0, "usb: SET_CONFIGURATION(%d) failed (%d)",
                 dev->cfg.value, rc);
        return rc;
    }
    dev->configured = 1;

    usb_attach_drivers(dev);
    return USB_OK;
}

/* ---------------------------------------------------------------------- *
 * Interrupt pipes
 * ---------------------------------------------------------------------- */
int usb_pipe_open_int(struct usb_device *dev, struct usb_endpoint *ep,
                      void *buf, u32 buflen, struct usb_pipe **out)
{
    struct usb_pipe *p;

    if (!dev || !ep || !out || !buf || buflen == 0)
        return USB_ERR_PROTO;
    if (ep->type != USB_EP_INTERRUPT || !USB_EP_IS_IN(ep->addr))
        return USB_ERR_UNSUPPORTED;
    if (!dev->bus->ops->open_int_pipe)
        return USB_ERR_UNSUPPORTED;

    p = (struct usb_pipe *)rhap_mem_alloc(sizeof *p);
    if (!p)
        return USB_ERR_NOMEM;
    p->bus = dev->bus;
    p->buf = (u8 *)buf;
    p->buflen = buflen;
    p->addr = dev->addr;
    p->ep = USB_EP_NUM(ep->addr);
    p->hcd = dev->bus->ops->open_int_pipe(dev->bus->ctx, dev->addr, p->ep,
                                          ep->maxpkt, ep->interval,
                                          dev->speed, buf, buflen);
    if (!p->hcd) {
        rhap_mem_free(p, sizeof *p);
        return USB_ERR_NOMEM;
    }
    ep->hcd = p->hcd;
    *out = p;
    return USB_OK;
}

int usb_pipe_poll(struct usb_pipe *p)
{
    if (!p || !p->hcd || !p->bus->ops->pipe_poll)
        return 0;
    return p->bus->ops->pipe_poll(p->bus->ctx, p->hcd);
}

int usb_pipe_actual(struct usb_pipe *p)
{
    if (!p || !p->hcd || !p->bus->ops->pipe_actual)
        return 0;
    return p->bus->ops->pipe_actual(p->bus->ctx, p->hcd);
}

void usb_pipe_close(struct usb_pipe *p)
{
    if (!p)
        return;
    if (p->hcd && p->bus->ops->close_pipe)
        p->bus->ops->close_pipe(p->bus->ctx, p->hcd);
    rhap_mem_free(p, sizeof *p);
}
