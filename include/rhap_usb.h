/*
 * rhap_usb.h -- USB 1.1 protocol definitions and the HCD/class-driver
 * interfaces used by the rest of the stack.
 *
 * Only what a boot-protocol keyboard/mouse needs.  No hubs, no isochronous,
 * no string descriptors beyond what enumeration asks for.
 */

#ifndef _RHAP_USB_H
#define _RHAP_USB_H

#include "rhap_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- packet IDs (as they appear in a UHCI TD token) ------ */
#define USB_PID_OUT         0xE1
#define USB_PID_IN          0x69
#define USB_PID_SETUP       0x2D

/* ---------------- bmRequestType --------------------------------------- */
#define USB_DIR_OUT         0x00
#define USB_DIR_IN          0x80
#define USB_TYPE_MASK       0x60
#define USB_TYPE_STANDARD   0x00
#define USB_TYPE_CLASS      0x20
#define USB_TYPE_VENDOR     0x40
#define USB_RECIP_MASK      0x1F
#define USB_RECIP_DEVICE    0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT  0x02

/* ---------------- standard requests ----------------------------------- */
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0A
#define USB_REQ_SET_INTERFACE     0x0B
#define USB_REQ_SYNCH_FRAME       0x0C

/* ---------------- class requests: HID -------------------------------- */
#define USB_REQ_HID_GET_REPORT    0x01
#define USB_REQ_HID_GET_IDLE      0x02
#define USB_REQ_HID_GET_PROTOCOL  0x03
#define USB_REQ_HID_SET_REPORT    0x09
#define USB_REQ_HID_SET_IDLE      0x0A
#define USB_REQ_HID_SET_PROTOCOL  0x0B

/* ---------------- descriptor types ------------------------------------ */
#define USB_DT_DEVICE       1
#define USB_DT_CONFIG       2
#define USB_DT_STRING       3
#define USB_DT_INTERFACE    4
#define USB_DT_ENDPOINT     5
#define USB_DT_DEVICE_QUAL  6
#define USB_DT_HID          0x21
#define USB_DT_REPORT       0x22
#define USB_DT_PHYSICAL     0x23

/* ---------------- endpoint attributes --------------------------------- */
#define USB_EP_TYPE_MASK    0x03
#define USB_EP_CONTROL      0x00
#define USB_EP_ISO          0x01
#define USB_EP_BULK         0x02
#define USB_EP_INTERRUPT    0x03

#define USB_EP_DIR_IN       0x80
#define USB_EP_NUM(a)       ((a) & 0x0F)
#define USB_EP_IS_IN(a)     (((a) & 0x80) != 0)

/* ---------------- classes --------------------------------------------- */
#define USB_CLASS_HID       0x03
#define USB_CLASS_HUB       0x09

#define HID_SUBCLASS_BOOT   0x01
#define HID_PROTO_KEYBOARD  0x01
#define HID_PROTO_MOUSE     0x02

/* ---------------- speeds ---------------------------------------------- */
#define USB_SPEED_LOW       0
#define USB_SPEED_FULL      1

/* ---------------- limits ---------------------------------------------- */
#define USB_MAX_BUSES       4
#define USB_MAX_DEVICES     8
#define USB_MAX_IF          8
#define USB_MAX_EP          6
#define USB_MAX_CLASS       4
#define USB_MAX_HCD         4
#define USB_DESC_MAX        512

/* ---------------- status codes (all errors are negative) -------------- */
#define USB_OK              0
#define USB_ERR_STALL      (-1)
#define USB_ERR_TIMEOUT    (-2)
#define USB_ERR_PROTO      (-3)
#define USB_ERR_NODEV      (-4)
#define USB_ERR_NOMEM      (-5)
#define USB_ERR_IO         (-6)
#define USB_ERR_UNSUPPORTED (-7)

/* ---------------------------------------------------------------------- *
 * Wire structures.  Little-endian on both sides (x86), and the controller
 * does DMA straight into the caller's buffer, so these are packed.
 * ---------------------------------------------------------------------- */
struct usb_setup {
    u8  bmRequestType;
    u8  bRequest;
    u16 wValue;
    u16 wIndex;
    u16 wLength;
} RHAP_PACKED;

struct usb_device_desc {
    u8  bLength;
    u8  bDescriptorType;
    u16 bcdUSB;
    u8  bDeviceClass;
    u8  bDeviceSubClass;
    u8  bDeviceProtocol;
    u8  bMaxPacketSize0;
    u16 idVendor;
    u16 idProduct;
    u16 bcdDevice;
    u8  iManufacturer;
    u8  iProduct;
    u8  iSerialNumber;
    u8  bNumConfigurations;
} RHAP_PACKED;

struct usb_config_desc {
    u8  bLength;
    u8  bDescriptorType;
    u16 wTotalLength;
    u8  bNumInterfaces;
    u8  bConfigurationValue;
    u8  iConfiguration;
    u8  bmAttributes;
    u8  bMaxPower;
} RHAP_PACKED;

struct usb_interface_desc {
    u8  bLength;
    u8  bDescriptorType;
    u8  bInterfaceNumber;
    u8  bAlternateSetting;
    u8  bNumEndpoints;
    u8  bInterfaceClass;
    u8  bInterfaceSubClass;
    u8  bInterfaceProtocol;
    u8  iInterface;
} RHAP_PACKED;

struct usb_endpoint_desc {
    u8  bLength;
    u8  bDescriptorType;
    u8  bEndpointAddress;
    u8  bmAttributes;
    u16 wMaxPacketSize;
    u8  bInterval;
} RHAP_PACKED;

/* ---------------------------------------------------------------------- *
 * Parsed topology.  Fixed-size, no allocation.
 * ---------------------------------------------------------------------- */
struct usb_endpoint {
    u8  addr;           /* bEndpointAddress, direction bit included */
    u8  type;           /* USB_EP_* */
    u8  interval;       /* bInterval, in frames */
    u16 maxpkt;
    void *hcd;          /* opaque per-pipe handle from the HCD */
};

struct usb_interface {
    u8  number;
    u8  alt;
    u8  class;
    u8  subclass;
    u8  proto;
    int nep;
    struct usb_endpoint ep[USB_MAX_EP];
    const struct usb_class_driver *driver;
    void *driver_data;
};

struct usb_configuration {
    u8  value;
    u8  attr;
    u8  maxpower;
    int nif;
    struct usb_interface intf[USB_MAX_IF];
};

struct usb_bus;
struct usb_device;
struct usb_pipe;

/* ---------------------------------------------------------------------- *
 * Host controller driver interface.
 *
 * control():     synchronous control transfer on endpoint 0.
 * open_int_pipe: register an interrupt-IN endpoint.  buf/buflen must stay
 *                valid (and DMA-capable) until close_pipe.
 * pipe_poll():   non-destructive: returns 1 once when a new packet landed
 *                in buf since the last call, 0 otherwise.  Never blocks.
 * pipe_actual(): byte count of the packet returned by the last poll (1).
 * ---------------------------------------------------------------------- */
struct usb_hcd_ops {
    int   (*control)(void *ctx, u8 addr, u8 ep0_max, u8 speed,
                     const struct usb_setup *s, void *data, u16 len,
                     u32 timeout_ms);
    void *(*open_int_pipe)(void *ctx, u8 addr, u8 ep, u16 maxpkt,
                           u8 interval, u8 speed, void *buf, u32 buflen);
    void  (*close_pipe)(void *ctx, void *pipe);
    int   (*pipe_poll)(void *ctx, void *pipe);
    int   (*pipe_actual)(void *ctx, void *pipe);
};

struct usb_pipe {
    struct usb_bus *bus;
    void *hcd;
    u8 *buf;
    u32 buflen;
    u8  addr;
    u8  ep;
};

struct usb_device {
    struct usb_bus *bus;
    u8  addr;
    u8  speed;
    u8  ep0_max;
    u8  configured;
    u16 id_vendor;
    u16 id_product;
    struct usb_configuration cfg;
    int n_attached;
};

struct usb_bus {
    int  used;
    int  ndev;
    u8   next_addr;
    rhap_lock_t lock;
    const struct usb_hcd_ops *ops;
    void *ctx;
    struct usb_device *dev[USB_MAX_DEVICES];
    u8 desc[USB_DESC_MAX] RHAP_ALIGN(16);
};

struct usb_class_driver {
    const char *name;
    int  (*match)(struct usb_device *dev, struct usb_interface *intf);
    int  (*attach)(struct usb_device *dev, struct usb_interface *intf);
    void (*detach)(struct usb_device *dev, struct usb_interface *intf);
    void (*poll)(struct usb_device *dev, struct usb_interface *intf);
};

/* ---------------------------------------------------------------------- *
 * Core API (usb_core.c)
 * ---------------------------------------------------------------------- */
void  usb_core_init(void);

struct usb_bus *usb_bus_alloc(const struct usb_hcd_ops *ops, void *ctx);
void  usb_bus_free(struct usb_bus *bus);

/* Add a device that has already been reset on a root port. */
struct usb_device *usb_bus_add_device(struct usb_bus *bus, u8 speed);
void  usb_device_remove(struct usb_device *dev);

/* Full enumeration: GET_DESCRIPTOR(8) -> SET_ADDRESS -> GET_DESCRIPTOR(18)
 * -> GET_DESCRIPTOR(config) -> SET_CONFIGURATION -> driver attach. */
int   usb_enumerate(struct usb_device *dev);

int   usb_register_class_driver(const struct usb_class_driver *drv);
void  usb_poll_drivers(struct usb_bus *bus);

/* Low-level helpers. */
int   usb_control_msg(struct usb_device *dev, u8 req_type, u8 req,
                      u16 value, u16 index, void *data, u16 len,
                      u32 timeout_ms);
int   usb_get_descriptor(struct usb_device *dev, u8 type, u8 index,
                         u16 langid, void *buf, u16 len);
int   usb_set_address(struct usb_device *dev, u8 addr);
int   usb_set_configuration(struct usb_device *dev, u8 value);

/* Pure descriptor parser -- no I/O, unit tested. */
int   usb_parse_config(const u8 *buf, int len, struct usb_configuration *out);

/* Interrupt pipes. */
int   usb_pipe_open_int(struct usb_device *dev, struct usb_endpoint *ep,
                        void *buf, u32 buflen, struct usb_pipe **out);
int   usb_pipe_poll(struct usb_pipe *p);
int   usb_pipe_actual(struct usb_pipe *p);
void  usb_pipe_close(struct usb_pipe *p);

/* HID helpers shared by the class driver (hid_boot.c). */
int   usb_hid_set_boot(struct usb_device *dev, struct usb_interface *intf);
int   usb_hid_set_idle(struct usb_device *dev, struct usb_interface *intf,
                       u8 duration4ms);

#ifdef __cplusplus
}
#endif

#endif /* _RHAP_USB_H */
