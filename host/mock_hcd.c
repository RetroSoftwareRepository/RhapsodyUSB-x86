/*
 * mock_hcd.c -- a fake host controller with a composite keyboard + mouse.
 *
 * It implements just enough of usb_hcd_ops for enumeration and HID polling,
 * so the real usb_core.c and hid_boot.c code paths run unmodified under the
 * host test harness.
 */

#include <string.h>

#include "rhap_types.h"
#include "rhap_port.h"
#include "rhap_usb.h"

#define MOCK_MAX_PIPE 4

struct mock_pipe {
    int in_use;
    u8  ep;
    u8 *buf;
    u32 buflen;
    u8  pending[16];
    int pending_len;
    int last_len;
};

static struct mock_pipe g_pipe[MOCK_MAX_PIPE];
static int g_configured;
static int g_set_protocol;
static int g_set_idle;
static u8  g_addr;

static const u8 g_dev_desc[18] = {
    18, USB_DT_DEVICE, 0x10, 0x01,
    0x00, 0x00, 0x00, 8,
    0xAC, 0x05,             /* vendor 0x05ac */
    0x01, 0x02,             /* product 0x0201 */
    0x00, 0x01,             /* bcdDevice 1.00 */
    0, 0, 0, 1
};

/* Two interfaces: boot keyboard on 0x81, boot mouse on 0x82. */
static const u8 g_cfg_desc[41] = {
    /* configuration */
    9, USB_DT_CONFIG, 41, 0, 2, 1, 0, 0x80, 50,
    /* interface 0: keyboard */
    9, USB_DT_INTERFACE, 0, 0, 1, USB_CLASS_HID, HID_SUBCLASS_BOOT,
       HID_PROTO_KEYBOARD, 0,
    7, USB_DT_ENDPOINT, 0x81, USB_EP_INTERRUPT, 8, 0, 10,
    /* interface 1: mouse */
    9, USB_DT_INTERFACE, 1, 0, 1, USB_CLASS_HID, HID_SUBCLASS_BOOT,
       HID_PROTO_MOUSE, 0,
    7, USB_DT_ENDPOINT, 0x82, USB_EP_INTERRUPT, 4, 0, 10
};

static int mock_control(void *ctx, u8 addr, u8 ep0_max, u8 speed,
                        const struct usb_setup *s, void *data, u16 len,
                        u32 timeout_ms)
{
    (void)ctx;
    (void)ep0_max;
    (void)speed;
    (void)timeout_ms;
    (void)addr;

    if (s->bmRequestType & USB_DIR_IN) {
        if (s->bRequest != USB_REQ_GET_DESCRIPTOR)
            return USB_ERR_STALL;
        switch (s->wValue >> 8) {
        case USB_DT_DEVICE: {
            int n = len;
            if (n > (int)sizeof g_dev_desc)
                n = (int)sizeof g_dev_desc;
            memcpy(data, g_dev_desc, (size_t)n);
            return n;
        }
        case USB_DT_CONFIG: {
            int n = len;
            if (n > (int)sizeof g_cfg_desc)
                n = (int)sizeof g_cfg_desc;
            memcpy(data, g_cfg_desc, (size_t)n);
            return n;
        }
        default:
            return USB_ERR_STALL;
        }
    }

    switch (s->bRequest) {
    case USB_REQ_SET_ADDRESS:
        g_addr = (u8)s->wValue;
        return 0;
    case USB_REQ_SET_CONFIGURATION:
        g_configured = (int)s->wValue;
        return 0;
    case USB_REQ_HID_SET_PROTOCOL:
        g_set_protocol++;
        return 0;
    case USB_REQ_HID_SET_IDLE:
        g_set_idle++;
        return 0;
    default:
        return USB_ERR_STALL;
    }
}

static void *mock_open_int_pipe(void *ctx, u8 addr, u8 ep, u16 maxpkt,
                                u8 interval, u8 speed, void *buf, u32 buflen)
{
    int i;

    (void)ctx;
    (void)addr;
    (void)maxpkt;
    (void)interval;
    (void)speed;
    for (i = 0; i < MOCK_MAX_PIPE; i++) {
        if (g_pipe[i].in_use)
            continue;
        memset(&g_pipe[i], 0, sizeof g_pipe[i]);
        g_pipe[i].in_use = 1;
        g_pipe[i].ep = (u8)(ep & 0x0F);
        g_pipe[i].buf = (u8 *)buf;
        g_pipe[i].buflen = buflen;
        return &g_pipe[i];
    }
    return 0;
}

static void mock_close_pipe(void *ctx, void *pipe)
{
    (void)ctx;
    if (pipe)
        ((struct mock_pipe *)pipe)->in_use = 0;
}

static int mock_pipe_poll(void *ctx, void *pipe)
{
    struct mock_pipe *p = (struct mock_pipe *)pipe;
    int n;

    (void)ctx;
    if (!p || !p->in_use || p->pending_len == 0)
        return 0;
    n = p->pending_len;
    if (n > (int)p->buflen)
        n = (int)p->buflen;
    memcpy(p->buf, p->pending, (size_t)n);
    p->last_len = n;
    p->pending_len = 0;
    return 1;
}

static int mock_pipe_actual(void *ctx, void *pipe)
{
    (void)ctx;
    return pipe ? ((struct mock_pipe *)pipe)->last_len : 0;
}

const struct usb_hcd_ops mock_hcd_ops = {
    mock_control,
    mock_open_int_pipe,
    mock_close_pipe,
    mock_pipe_poll,
    mock_pipe_actual
};

/* ------------------------------------------------------------------ *
 * Test hooks
 * ------------------------------------------------------------------ */
int mock_configured(void)   { return g_configured; }
int mock_set_protocols(void) { return g_set_protocol; }
int mock_set_idles(void)    { return g_set_idle; }
u8  mock_address(void)      { return g_addr; }

int mock_push(u8 ep, const u8 *data, int len)
{
    int i;

    for (i = 0; i < MOCK_MAX_PIPE; i++) {
        if (!g_pipe[i].in_use || g_pipe[i].ep != (ep & 0x0F))
            continue;
        if (len > (int)sizeof g_pipe[i].pending)
            len = (int)sizeof g_pipe[i].pending;
        memcpy(g_pipe[i].pending, data, (size_t)len);
        g_pipe[i].pending_len = len;
        return 1;
    }
    return 0;
}
