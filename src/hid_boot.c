/*
 * hid_boot.c -- USB HID boot-protocol keyboard and mouse.
 *
 * Boot protocol is used on purpose: parsing the report descriptor would add
 * several hundred lines for zero benefit on a 1998 machine, and every HID
 * keyboard and mouse is required to support the boot reports.
 *
 * Output is ADB keycodes, because that is what Rhapsody's event system
 * consumes even on x86 (its PS/2 driver translated scancodes the same way).
 * The mapping table below is the standard Apple keycode set:
 * 0x00 = A, 0x24 = Return, 0x33 = Delete, 0x35 = Escape, 0x7e = Up Arrow.
 *
 * Keyboard reports are 8 bytes:   [modifiers][reserved][key x6]
 * Mouse reports are 3 bytes:      [buttons][dx][dy]
 */

#include "rhap_types.h"
#include "rhap_port.h"
#include "rhap_usb.h"

#define HID_MAX_DEV 4

struct hid_dev {
    int  in_use;
    u8   proto;
    u8   buttons;
    struct usb_device *dev;
    struct usb_interface *intf;
    struct usb_pipe *pipe;
    u8   buf[8] RHAP_ALIGN(16);
    u8   prev[8];
};

static struct hid_dev g_hid[HID_MAX_DEV];

/* ---------------------------------------------------------------------- *
 * HID usage -> ADB keycode
 * ---------------------------------------------------------------------- */
static u8 hid_usage_to_adb(u8 usage)
{
    switch (usage) {
    /* letters, HID 0x04..0x1d -> A..Z */
    case 0x04: return 0x00;     /* a */
    case 0x05: return 0x0B;     /* b */
    case 0x06: return 0x08;     /* c */
    case 0x07: return 0x02;     /* d */
    case 0x08: return 0x0E;     /* e */
    case 0x09: return 0x03;     /* f */
    case 0x0A: return 0x05;     /* g */
    case 0x0B: return 0x04;     /* h */
    case 0x0C: return 0x22;     /* i */
    case 0x0D: return 0x26;     /* j */
    case 0x0E: return 0x28;     /* k */
    case 0x0F: return 0x25;     /* l */
    case 0x10: return 0x2E;     /* m */
    case 0x11: return 0x2D;     /* n */
    case 0x12: return 0x1F;     /* o */
    case 0x13: return 0x23;     /* p */
    case 0x14: return 0x0C;     /* q */
    case 0x15: return 0x0F;     /* r */
    case 0x16: return 0x01;     /* s */
    case 0x17: return 0x11;     /* t */
    case 0x18: return 0x20;     /* u */
    case 0x19: return 0x09;     /* v */
    case 0x1A: return 0x0D;     /* w */
    case 0x1B: return 0x07;     /* x */
    case 0x1C: return 0x10;     /* y */
    case 0x1D: return 0x06;     /* z */

    /* digits 1..0 */
    case 0x1E: return 0x12;     /* 1 */
    case 0x1F: return 0x13;     /* 2 */
    case 0x20: return 0x14;     /* 3 */
    case 0x21: return 0x15;     /* 4 */
    case 0x22: return 0x17;     /* 5 */
    case 0x23: return 0x16;     /* 6 */
    case 0x24: return 0x1A;     /* 7 */
    case 0x25: return 0x1C;     /* 8 */
    case 0x26: return 0x19;     /* 9 */
    case 0x27: return 0x1D;     /* 0 */

    /* punctuation and control keys */
    case 0x28: return 0x24;     /* Return */
    case 0x29: return 0x35;     /* Escape */
    case 0x2A: return 0x33;     /* Backspace */
    case 0x2B: return 0x30;     /* Tab */
    case 0x2C: return 0x31;     /* Space */
    case 0x2D: return 0x1B;     /* - */
    case 0x2E: return 0x18;     /* = */
    case 0x2F: return 0x21;     /* [ */
    case 0x30: return 0x1E;     /* ] */
    case 0x31: return 0x2A;     /* backslash */
    case 0x33: return 0x29;     /* ; */
    case 0x34: return 0x27;     /* ' */
    case 0x35: return 0x32;     /* ` */
    case 0x36: return 0x2B;     /* , */
    case 0x37: return 0x2F;     /* . */
    case 0x38: return 0x2C;     /* / */
    case 0x39: return 0x39;     /* Caps Lock */

    /* function keys */
    case 0x3A: return 0x7A;     /* F1 */
    case 0x3B: return 0x78;     /* F2 */
    case 0x3C: return 0x63;     /* F3 */
    case 0x3D: return 0x76;     /* F4 */
    case 0x3E: return 0x60;     /* F5 */
    case 0x3F: return 0x61;     /* F6 */
    case 0x40: return 0x62;     /* F7 */
    case 0x41: return 0x64;     /* F8 */
    case 0x42: return 0x65;     /* F9 */
    case 0x43: return 0x6D;     /* F10 */
    case 0x44: return 0x67;     /* F11 */
    case 0x45: return 0x6F;     /* F12 */

    /* navigation cluster and editing keys */
    case 0x49: return 0x72;     /* Insert -> Help */
    case 0x4A: return 0x73;     /* Home */
    case 0x4B: return 0x74;     /* Page Up */
    case 0x4C: return 0x75;     /* Delete */
    case 0x4D: return 0x77;     /* End */
    case 0x4E: return 0x79;     /* Page Down */
    case 0x4F: return 0x7C;     /* Right */
    case 0x50: return 0x7B;     /* Left */
    case 0x51: return 0x7D;     /* Down */
    case 0x52: return 0x7E;     /* Up */

    /* modifiers (reported in byte 0, not in the key array) */
    case 0xE0: return 0x3B;     /* Left Control */
    case 0xE1: return 0x38;     /* Left Shift */
    case 0xE2: return 0x3A;     /* Left Option */
    case 0xE3: return 0x37;     /* Left Command */
    case 0xE4: return 0x3E;     /* Right Control */
    case 0xE5: return 0x3C;     /* Right Shift */
    case 0xE6: return 0x3D;     /* Right Option */
    case 0xE7: return 0x37;     /* Right Command */

    default:  return 0xFF;      /* not mapped: silently ignored */
    }
}

static u8 hid_modifier_to_adb(int bit)
{
    switch (bit) {
    case 0: return 0x3B;        /* LCtrl */
    case 1: return 0x38;        /* LShift */
    case 2: return 0x3A;        /* LOption */
    case 3: return 0x37;        /* LCommand */
    case 4: return 0x3E;        /* RCtrl */
    case 5: return 0x3C;        /* RShift */
    case 6: return 0x3D;        /* ROption */
    default: return 0x37;       /* RCommand */
    }
}

/* ---------------------------------------------------------------------- *
 * Report decoding
 * ---------------------------------------------------------------------- */
static int hid_key_present(const u8 *rep, u8 key)
{
    int i;

    for (i = 2; i < 8; i++)
        if (rep[i] == key)
            return 1;
    return 0;
}

static void hid_keyboard_report(struct hid_dev *d, int len)
{
    u8 mods = d->buf[0];
    u8 code;
    int i;

    if (len < 8)
        return;

    /*
     * Ordering matters for the event consumer:
     *   modifier presses, key releases, key presses, modifier releases.
     * That way a shifted character is always seen with shift already down,
     * and a released character is never seen with shift already up.
     */
    for (i = 0; i < 8; i++) {
        if (((d->prev[0] >> i) & 1) == 0 && ((mods >> i) & 1) != 0) {
            code = hid_modifier_to_adb(i);
            rhap_input_key(code, 1);
        }
    }

    for (i = 2; i < 8; i++) {
        u8 key = d->prev[i];

        if (key == 0 || key == 1)
            continue;
        if (hid_key_present(d->buf, key))
            continue;
        code = hid_usage_to_adb(key);
        if (code != 0xFF)
            rhap_input_key(code, 0);
    }

    for (i = 2; i < 8; i++) {
        u8 key = d->buf[i];

        if (key == 0 || key == 1)       /* 0x01 = rollover error */
            continue;
        if (hid_key_present(d->prev, key))
            continue;
        code = hid_usage_to_adb(key);
        if (code != 0xFF)
            rhap_input_key(code, 1);
    }

    for (i = 0; i < 8; i++) {
        if (((d->prev[0] >> i) & 1) != 0 && ((mods >> i) & 1) == 0) {
            code = hid_modifier_to_adb(i);
            rhap_input_key(code, 0);
        }
    }

    for (i = 0; i < 8; i++)
        d->prev[i] = d->buf[i];
}

static void hid_mouse_report(struct hid_dev *d, int len)
{
    u8 buttons;
    s32 dx, dy;

    if (len < 3)
        return;

    buttons = (u8)(d->buf[0] & 0x07);
    dx = (s32)(s8)d->buf[1];
    dy = (s32)(s8)d->buf[2];

    /*
     * Send button-only events too: a click with no motion still matters.
     * dy stays in USB orientation (+ = away from the user); the port layer
     * flips it if Rhapsody's cursor expects screen coordinates.
     */
    if (dx != 0 || dy != 0 || buttons != d->buttons) {
        rhap_input_mouse(dx, dy, buttons);
        d->buttons = buttons;
    }
}

/* ---------------------------------------------------------------------- *
 * Class driver
 * ---------------------------------------------------------------------- */
static int hid_match(struct usb_device *dev, struct usb_interface *intf)
{
    (void)dev;
    if (intf->class != USB_CLASS_HID)
        return 0;
    if (intf->subclass != HID_SUBCLASS_BOOT &&
        intf->proto != HID_PROTO_KEYBOARD &&
        intf->proto != HID_PROTO_MOUSE)
        return 0;
    return intf->nep > 0;
}

static struct hid_dev *hid_alloc(void)
{
    int i;

    for (i = 0; i < HID_MAX_DEV; i++) {
        if (!g_hid[i].in_use)
            return &g_hid[i];
    }
    return RHAP_NULL;
}

static int hid_attach(struct usb_device *dev, struct usb_interface *intf)
{
    struct hid_dev *d;
    struct usb_endpoint *ep = RHAP_NULL;
    int i, rc;

    for (i = 0; i < intf->nep; i++) {
        if (intf->ep[i].type == USB_EP_INTERRUPT &&
            USB_EP_IS_IN(intf->ep[i].addr)) {
            ep = &intf->ep[i];
            break;
        }
    }
    if (!ep) {
        rhap_log(0, "hid: interface %d has no interrupt-IN endpoint",
                 intf->number);
        return USB_ERR_PROTO;
    }

    d = hid_alloc();
    if (!d) {
        rhap_log(0, "hid: no free device slot");
        return USB_ERR_NOMEM;
    }

    d->in_use = 1;
    d->dev = dev;
    d->intf = intf;
    d->pipe = RHAP_NULL;
    d->proto = intf->proto;
    d->buttons = 0;
    for (i = 0; i < 8; i++) {
        d->buf[i] = 0;
        d->prev[i] = 0;
    }

    /*
     * Ask for boot reports and idle 0 (report only on change).  Both are
     * advisory: a device that rejects them can still stream boot reports
     * at its default rate, so failures are logged but not fatal.
     */
    rc = usb_hid_set_boot(dev, intf);
    if (rc < 0)
        rhap_log(1, "hid: SET_PROTOCOL(boot) rejected (%d), continuing", rc);
    rc = usb_hid_set_idle(dev, intf, 0);
    if (rc < 0)
        rhap_log(1, "hid: SET_IDLE(0) rejected (%d), continuing", rc);

    rc = usb_pipe_open_int(dev, ep, d->buf, sizeof d->buf, &d->pipe);
    if (rc != USB_OK) {
        rhap_log(0, "hid: cannot open interrupt pipe (%d)", rc);
        d->in_use = 0;
        return rc;
    }

    intf->driver_data = d;
    rhap_log(1, "hid: %s on interface %d, ep 0x%02x",
             (d->proto == HID_PROTO_MOUSE) ? "mouse" : "keyboard",
             intf->number, ep->addr);
    return USB_OK;
}

static void hid_detach(struct usb_device *dev, struct usb_interface *intf)
{
    struct hid_dev *d = (struct hid_dev *)intf->driver_data;

    (void)dev;
    if (!d)
        return;
    if (d->pipe) {
        usb_pipe_close(d->pipe);
        d->pipe = RHAP_NULL;
    }
    d->in_use = 0;
    intf->driver_data = RHAP_NULL;
}

static void hid_poll(struct usb_device *dev, struct usb_interface *intf)
{
    struct hid_dev *d = (struct hid_dev *)intf->driver_data;
    int len;

    (void)dev;
    if (!d || !d->pipe)
        return;
    if (!usb_pipe_poll(d->pipe))
        return;
    len = usb_pipe_actual(d->pipe);
    if (len <= 0)
        return;

    if (d->proto == HID_PROTO_MOUSE)
        hid_mouse_report(d, len);
    else
        hid_keyboard_report(d, len);
}

static const struct usb_class_driver hid_driver = {
    "usbhid",
    hid_match,
    hid_attach,
    hid_detach,
    hid_poll
};

int usb_hid_init(void)
{
    return usb_register_class_driver(&hid_driver);
}

/* ---------------------------------------------------------------------- *
 * HID class requests
 * ---------------------------------------------------------------------- */
int usb_hid_set_boot(struct usb_device *dev, struct usb_interface *intf)
{
    return usb_control_msg(dev,
                           USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                           USB_REQ_HID_SET_PROTOCOL, 0,
                           (u16)intf->number, RHAP_NULL, 0, 1000);
}

int usb_hid_set_idle(struct usb_device *dev, struct usb_interface *intf,
                     u8 duration4ms)
{
    return usb_control_msg(dev,
                           USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                           USB_REQ_HID_SET_IDLE,
                           (u16)((u16)duration4ms << 8), (u16)intf->number,
                           RHAP_NULL, 0, 1000);
}

#ifdef RHAP_HOST_BUILD
u8 rhap_hid_test_usage_to_adb(u8 usage)
{
    return hid_usage_to_adb(usage);
}
#endif
