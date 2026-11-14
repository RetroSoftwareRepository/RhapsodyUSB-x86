/*
 * test_main.c -- host unit tests for the portable parts of the stack.
 *
 * Build:  make check   (compiles every .c with a host port layer and runs this)
 *
 * These tests cover the parts that can be wrong silently:
 *   - descriptor parsing, including malformed input
 *   - UHCI length/token encodings (the n-1 fields)
 *   - enumeration state machine and driver binding, against a mock HCD
 *   - HID boot report decoding and ADB keycode translation
 */

#include <stdio.h>
#include <string.h>

#include "rhap_types.h"
#include "rhap_port.h"
#include "rhap_usb.h"
#include "rhap_uhci.h"

/* host/port_host.c */
struct host_event {
    int kind;
    int a, b, c;
};
extern struct host_event host_events[];
extern int host_nev;

/* host/mock_hcd.c */
extern const struct usb_hcd_ops mock_hcd_ops;
extern int mock_configured(void);
extern int mock_set_protocols(void);
extern int mock_set_idles(void);
extern u8  mock_address(void);
extern int mock_push(u8 ep, const u8 *data, int len);

/* hid_boot.c, host build only */
extern u8 rhap_hid_test_usage_to_adb(u8 usage);
extern int usb_hid_init(void);

static int g_fail;
static int g_checks;

#define CHECK(cond)                                                     \
    do {                                                                \
        g_checks++;                                                     \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            g_fail++;                                                   \
        }                                                               \
    } while (0)

static void reset_events(void)
{
    host_nev = 0;
}

/* ------------------------------------------------------------------ *
 * 1. Descriptor parsing
 * ------------------------------------------------------------------ */
static const u8 keyboard_config[] = {
    9, USB_DT_CONFIG, 41, 0, 2, 1, 0, 0x80, 50,
    9, USB_DT_INTERFACE, 0, 0, 1, USB_CLASS_HID, HID_SUBCLASS_BOOT,
       HID_PROTO_KEYBOARD, 0,
    7, USB_DT_ENDPOINT, 0x81, USB_EP_INTERRUPT, 8, 0, 10,
    9, USB_DT_INTERFACE, 1, 0, 1, USB_CLASS_HID, HID_SUBCLASS_BOOT,
       HID_PROTO_MOUSE, 0,
    7, USB_DT_ENDPOINT, 0x82, USB_EP_INTERRUPT, 4, 0, 10
};

static const u8 config_with_hid_desc[] = {
    9, USB_DT_CONFIG, 34, 0, 1, 1, 0, 0x80, 50,
    9, USB_DT_INTERFACE, 0, 0, 1, USB_CLASS_HID, 0, 0, 0,
    9, USB_DT_HID, 0x11, 0x01, 0, 1, USB_DT_REPORT, 63, 0,
    7, USB_DT_ENDPOINT, 0x81, USB_EP_INTERRUPT, 8, 0, 10
};

static void test_parse(void)
{
    struct usb_configuration cfg;
    u8 bad[8];

    CHECK(usb_parse_config(keyboard_config, (int)sizeof keyboard_config,
                           &cfg) == USB_OK);
    CHECK(cfg.value == 1);
    CHECK(cfg.nif == 2);
    CHECK(cfg.intf[0].class == USB_CLASS_HID);
    CHECK(cfg.intf[0].proto == HID_PROTO_KEYBOARD);
    CHECK(cfg.intf[0].nep == 1);
    CHECK(cfg.intf[0].ep[0].addr == 0x81);
    CHECK(cfg.intf[0].ep[0].type == USB_EP_INTERRUPT);
    CHECK(cfg.intf[0].ep[0].maxpkt == 8);
    CHECK(cfg.intf[0].ep[0].interval == 10);
    CHECK(cfg.intf[1].proto == HID_PROTO_MOUSE);
    CHECK(cfg.intf[1].ep[0].addr == 0x82);
    CHECK(cfg.intf[1].ep[0].maxpkt == 4);

    /* A class-specific HID descriptor must be skipped, not choke the parser. */
    CHECK(usb_parse_config(config_with_hid_desc,
                           (int)sizeof config_with_hid_desc, &cfg) == USB_OK);
    CHECK(cfg.nif == 1);
    CHECK(cfg.intf[0].nep == 1);
    CHECK(cfg.intf[0].ep[0].maxpkt == 8);

    /* Zero length would loop forever if we trusted it. */
    memset(bad, 0, sizeof bad);
    CHECK(usb_parse_config(bad, (int)sizeof bad, &cfg) == USB_ERR_PROTO);

    /* Length that runs off the end. */
    bad[0] = 9;
    bad[1] = USB_DT_CONFIG;
    CHECK(usb_parse_config(bad, 4, &cfg) == USB_ERR_PROTO);

    /* Endpoint with no preceding interface. */
    memset(bad, 0, sizeof bad);
    bad[0] = 7;
    bad[1] = USB_DT_ENDPOINT;
    CHECK(usb_parse_config(bad, 7, &cfg) == USB_ERR_PROTO);

    /* Truncated buffer. */
    CHECK(usb_parse_config(keyboard_config, 20, &cfg) == USB_ERR_PROTO);
}

/* ------------------------------------------------------------------ *
 * 2. UHCI encodings
 * ------------------------------------------------------------------ */
static void test_encoding(void)
{
    u32 st;

    /* MaxLen is "expected bytes - 1" in bits 31:21. */
    CHECK((uhci_explen(8) >> TD_TOKEN_LEN_SHIFT) == 7);
    CHECK((uhci_explen(64) >> TD_TOKEN_LEN_SHIFT) == 63);
    CHECK((uhci_explen(1) >> TD_TOKEN_LEN_SHIFT) == 0);

    /* ACTLEN is "actual bytes - 1" in bits 10:0. */
    st = 7;
    CHECK(uhci_actlen(st) == 8);
    st = 0;
    CHECK(uhci_actlen(st) == 1);
    /* 0x7ff is the "no data" encoding and must decode to zero. */
    st = TD_ACTLEN_MASK;
    CHECK(uhci_actlen(st) == 0);

    /* A token must carry pid, endpoint, address and toggle independently. */
    st = 0x05u | (3u << TD_TOKEN_EP_SHIFT) |
         (0x2Au << TD_TOKEN_ADDR_SHIFT) | TD_TOKEN_TOGGLE | uhci_explen(8);
    CHECK((st & TD_TOKEN_PID_MASK) == 0x05);
    CHECK(((st >> TD_TOKEN_EP_SHIFT) & 0xF) == 3);
    CHECK(((st >> TD_TOKEN_ADDR_SHIFT) & 0x7F) == 0x2A);
    CHECK((st & TD_TOKEN_TOGGLE) != 0);
    CHECK((st >> TD_TOKEN_LEN_SHIFT) == 7);
}

/* ------------------------------------------------------------------ *
 * 3. Enumeration + HID, end to end through the mock controller
 * ------------------------------------------------------------------ */
static void test_enumeration_and_hid(void)
{
    struct usb_bus *bus;
    struct usb_device *dev;
    u8 report[8];

    usb_core_init();
    CHECK(usb_hid_init() == USB_OK);

    bus = usb_bus_alloc(&mock_hcd_ops, 0);
    CHECK(bus != 0);
    dev = usb_bus_add_device(bus, USB_SPEED_LOW);
    CHECK(dev != 0);
    CHECK(usb_enumerate(dev) == USB_OK);

    CHECK(dev->addr == 1);
    CHECK(mock_address() == 1);
    CHECK(dev->configured == 1);
    CHECK(mock_configured() == 1);
    CHECK(dev->id_vendor == 0x05AC);
    CHECK(dev->id_product == 0x0201);
    CHECK(dev->ep0_max == 8);
    CHECK(dev->cfg.nif == 2);
    CHECK(dev->cfg.intf[0].driver != 0);
    CHECK(dev->cfg.intf[1].driver != 0);
    CHECK(dev->n_attached == 2);
    CHECK(mock_set_protocols() == 2);
    CHECK(mock_set_idles() == 2);

    /* --- keyboard: shift + 'a' down, then everything up --- */
    reset_events();
    memset(report, 0, sizeof report);
    report[0] = 0x02;               /* left shift */
    report[2] = 0x04;               /* a */
    CHECK(mock_push(0x01, report, 8) == 1);
    usb_poll_drivers(bus);
    CHECK(host_nev == 2);
    CHECK(host_events[0].kind == 1);
    CHECK(host_events[0].a == 0x38);    /* left shift */
    CHECK(host_events[0].b == 1);
    CHECK(host_events[1].a == 0x00);    /* 'a' */
    CHECK(host_events[1].b == 1);

    /* Repeating the same report must not repeat the key. */
    reset_events();
    CHECK(mock_push(0x01, report, 8) == 1);
    usb_poll_drivers(bus);
    CHECK(host_nev == 0);

    /* Release: key up before modifier up. */
    reset_events();
    memset(report, 0, sizeof report);
    CHECK(mock_push(0x01, report, 8) == 1);
    usb_poll_drivers(bus);
    CHECK(host_nev == 2);
    CHECK(host_events[0].a == 0x00);
    CHECK(host_events[0].b == 0);
    CHECK(host_events[1].a == 0x38);
    CHECK(host_events[1].b == 0);

    /* --- mouse: left button, +5 x, -5 y --- */
    reset_events();
    report[0] = 0x01;
    report[1] = 5;
    report[2] = 0xFB;               /* -5 */
    CHECK(mock_push(0x82, report, 3) == 1);
    usb_poll_drivers(bus);
    CHECK(host_nev == 1);
    CHECK(host_events[0].kind == 2);
    CHECK(host_events[0].a == 5);
    CHECK(host_events[0].b == -5);
    CHECK(host_events[0].c == 1);

    /* Button-only change must still be delivered. */
    reset_events();
    report[0] = 0x00;
    report[1] = 0;
    report[2] = 0;
    CHECK(mock_push(0x82, report, 3) == 1);
    usb_poll_drivers(bus);
    CHECK(host_nev == 1);
    CHECK(host_events[0].c == 0);

    /* Teardown must close both pipes and free the device. */
    usb_device_remove(dev);
    CHECK(bus->ndev == 0);
    usb_bus_free(bus);
}

/* ------------------------------------------------------------------ *
 * 4. Control transfer chain construction
 * ------------------------------------------------------------------ */
extern int uhci_test_fill_control(uhci_td_t **td, u8 addr, u8 ep0_max,
                                  u8 speed, const struct usb_setup *setup,
                                  void *data, u16 len);

static void test_control_chain(void)
{
    static uhci_td_t pool[8] RHAP_ALIGN(16);
    uhci_td_t *td[8];
    struct usb_setup s;
    u8 buf[64];
    int n, i;

    for (i = 0; i < 8; i++)
        td[i] = &pool[i];

    /* GET_DESCRIPTOR, 18 bytes over an 8-byte endpoint:
     * setup + 3 data packets + status = 5 descriptors. */
    memset(&s, 0, sizeof s);
    s.bmRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    s.bRequest = USB_REQ_GET_DESCRIPTOR;
    s.wValue = 0x0100;
    s.wLength = 18;
    n = uhci_test_fill_control(td, 5, 8, USB_SPEED_FULL, &s, buf, 18);

    CHECK(n == 5);
    CHECK((td[0]->token & TD_TOKEN_PID_MASK) == USB_PID_SETUP);
    CHECK(((td[0]->token >> TD_TOKEN_ADDR_SHIFT) & 0x7F) == 5);
    CHECK(((td[0]->token >> TD_TOKEN_EP_SHIFT) & 0xF) == 0);
    CHECK((td[0]->token & TD_TOKEN_TOGGLE) == 0);
    CHECK((td[0]->token >> TD_TOKEN_LEN_SHIFT) == 7);   /* 8 bytes */
    CHECK((td[0]->status & TD_ACTIVE) != 0);
    CHECK(td[0]->buffer == (u32)(size_t)&s);

    /* Data stage: IN, DATA1/DATA0/DATA1, 8/8/2 bytes, short packet detect. */
    CHECK((td[1]->token & TD_TOKEN_PID_MASK) == USB_PID_IN);
    CHECK((td[1]->token & TD_TOKEN_TOGGLE) != 0);
    CHECK((td[2]->token & TD_TOKEN_TOGGLE) == 0);
    CHECK((td[3]->token & TD_TOKEN_TOGGLE) != 0);
    CHECK((td[2]->token >> TD_TOKEN_LEN_SHIFT) == 7);
    CHECK((td[3]->token >> TD_TOKEN_LEN_SHIFT) == 1);   /* 2 bytes */
    CHECK((td[1]->status & TD_SPD) != 0);
    CHECK((td[1]->buffer == (u32)(size_t)buf));

    /* Status stage: opposite direction (OUT), DATA1, zero length. */
    CHECK((td[4]->token & TD_TOKEN_PID_MASK) == USB_PID_OUT);
    CHECK((td[4]->token & TD_TOKEN_TOGGLE) != 0);
    CHECK((td[4]->token >> TD_TOKEN_LEN_SHIFT) == TD_TOKEN_LEN_MASK);
    CHECK(td[4]->buffer == 0);
    CHECK((td[4]->status & TD_SPD) == 0);

    /* Chain links must reach the next descriptor, then terminate. */
    CHECK(td[0]->link == (u32)(size_t)&pool[1]);
    CHECK(td[3]->link == (u32)(size_t)&pool[4]);
    CHECK(td[4]->link == UHCI_PTR_TERM);

    /* Zero-length OUT (SET_ADDRESS) on a low-speed device: setup + status,
     * and the status stage flips to IN. */
    memset(&s, 0, sizeof s);
    s.bmRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    s.bRequest = USB_REQ_SET_ADDRESS;
    s.wValue = 9;
    n = uhci_test_fill_control(td, 9, 8, USB_SPEED_LOW, &s, 0, 0);

    CHECK(n == 2);
    CHECK((td[0]->token & TD_TOKEN_PID_MASK) == USB_PID_SETUP);
    CHECK((td[0]->status & TD_LS) != 0);
    CHECK((td[1]->token & TD_TOKEN_PID_MASK) == USB_PID_IN);
    CHECK((td[1]->token & TD_TOKEN_TOGGLE) != 0);
    CHECK((td[1]->token >> TD_TOKEN_LEN_SHIFT) == TD_TOKEN_LEN_MASK);

    /* 64-byte endpoint: a 41-byte descriptor fits in one packet. */
    memset(&s, 0, sizeof s);
    s.bmRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    s.bRequest = USB_REQ_GET_DESCRIPTOR;
    s.wLength = 41;
    n = uhci_test_fill_control(td, 2, 64, USB_SPEED_FULL, &s, buf, 41);

    CHECK(n == 3);
    CHECK((td[1]->token >> TD_TOKEN_LEN_SHIFT) == 40);  /* 41 bytes */
}

/* ------------------------------------------------------------------ *
 * 5. Keycode translation
 * ------------------------------------------------------------------ */
static void test_keymap(void)
{
    CHECK(rhap_hid_test_usage_to_adb(0x04) == 0x00);   /* a */
    CHECK(rhap_hid_test_usage_to_adb(0x1D) == 0x06);   /* z */
    CHECK(rhap_hid_test_usage_to_adb(0x28) == 0x24);   /* return */
    CHECK(rhap_hid_test_usage_to_adb(0x2C) == 0x31);   /* space */
    CHECK(rhap_hid_test_usage_to_adb(0x2A) == 0x33);   /* backspace */
    CHECK(rhap_hid_test_usage_to_adb(0x35) == 0x32);   /* grave */
    CHECK(rhap_hid_test_usage_to_adb(0x52) == 0x7E);   /* up arrow */
    CHECK(rhap_hid_test_usage_to_adb(0x51) == 0x7D);   /* down arrow */
    CHECK(rhap_hid_test_usage_to_adb(0x3A) == 0x7A);   /* F1 */
    CHECK(rhap_hid_test_usage_to_adb(0xE0) == 0x3B);   /* left control */
    CHECK(rhap_hid_test_usage_to_adb(0xE5) == 0x3C);   /* right shift */
    CHECK(rhap_hid_test_usage_to_adb(0x70) == 0xFF);   /* unmapped */
}

int main(void)
{
    printf("RhapsodyUSB host tests\n");

    test_parse();
    test_encoding();
    test_enumeration_and_hid();
    test_control_chain();
    test_keymap();

    printf("%d checks, %d failure(s)\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
