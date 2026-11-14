/*
 * rhap_uhci.h -- Intel UHCI (Universal Host Controller Interface) register
 * file and in-memory data structures.
 *
 * Reference: Intel UHCI Design Guide 1.1.  Bit positions cross-checked
 * against drivers/usb/host/uhci-hcd.h in Linux.
 *
 * Note the two encodings that bite people:
 *   - TD status ACTLEN  is "actual bytes - 1", 11 bits
 *   - TD token  MaxLen  is "expected bytes - 1", 11 bits
 */

#ifndef _RHAP_UHCI_H
#define _RHAP_UHCI_H

#include "rhap_types.h"
#include "rhap_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- register offsets (BAR0, I/O space) ------------------ */
#define UHCI_USBCMD       0x00
#define UHCI_USBSTS       0x02
#define UHCI_USBINTR      0x04
#define UHCI_FRNUM        0x06
#define UHCI_FLBASEADD    0x08
#define UHCI_SOFMOD       0x0C
#define UHCI_PORTSC(p)    (0x10 + 2 * (p))

/* USBCMD */
#define USBCMD_RS         0x0001   /* run/stop */
#define USBCMD_HCRESET    0x0002   /* host controller reset */
#define USBCMD_GRESET     0x0004   /* global reset */
#define USBCMD_EGSM       0x0008
#define USBCMD_FGR        0x0010
#define USBCMD_SWDBG      0x0020
#define USBCMD_CF         0x0040   /* config flag: always set by software */
#define USBCMD_MAXP       0x0080   /* max packet: 0 = 32, 1 = 64 */

/* USBSTS (write 1 to clear, except HCHalted which is read-only) */
#define USBSTS_USBINT     0x0001
#define USBSTS_ERRINT     0x0002
#define USBSTS_RESUME     0x0004
#define USBSTS_HSE        0x0008   /* host system error */
#define USBSTS_HCPE       0x0010   /* host controller process error */
#define USBSTS_HCAA       0x0020   /* host controller halted */
#define USBSTS_RD         0x0040   /* resume detected */
#define USBSTS_W1C        0x003F

/* PORTSC */
#define PORTSC_CCS        0x0001   /* current connect status */
#define PORTSC_CSC        0x0002   /* connect status change (W1C) */
#define PORTSC_PE         0x0004   /* port enabled */
#define PORTSC_PEC        0x0008   /* port enable change (W1C) */
#define PORTSC_DPLUS      0x0010   /* D+ high */
#define PORTSC_DMINUS     0x0020   /* D- high */
#define PORTSC_RD         0x0040   /* resume detect (W1C) */
#define PORTSC_RES1       0x0080   /* reserved, always written as 1 */
#define PORTSC_LSDA       0x0100   /* low-speed device attached */
#define PORTSC_PR         0x0200   /* port reset */
#define PORTSC_OC         0x0400
#define PORTSC_OCC        0x0800   /* over-current change (W1C) */
#define PORTSC_SUSP       0x1000
#define PORTSC_RES234     0xE000   /* reserved, write zeroes */

/* Bits software may change; everything else must be preserved. */
#define PORTSC_RW         (PORTSC_PE | PORTSC_PR | PORTSC_SUSP)

/* USBSOF: 64 -> exactly 1 ms frames */
#define USBSOF_1MS        64

/* Pointer tag bits in link/head/element fields. */
#define UHCI_PTR_TERM     0x0001
#define UHCI_PTR_QH       0x0002
#define UHCI_PTR_DEPTH    0x0004
#define UHCI_PTR_MASK     0xFFFFFFF0

/* ---------------- TD status word bits --------------------------------- */
#define TD_SPD            (1U << 29)  /* short packet detect */
#define TD_CERR_SHIFT     27
#define TD_CERR(n)        ((u32)(n) << TD_CERR_SHIFT)
#define TD_LS             (1U << 26)  /* low-speed device */
#define TD_IOS            (1U << 25)
#define TD_IOC            (1U << 24)
#define TD_ACTIVE         (1U << 23)
#define TD_STALLED        (1U << 22)
#define TD_DBUFERR        (1U << 21)
#define TD_BABBLE         (1U << 20)
#define TD_NAK            (1U << 19)
#define TD_CRCTIMEO       (1U << 18)
#define TD_BITSTUFF       (1U << 17)
#define TD_ACTLEN_MASK    0x7FFU      /* actual length, encoded n - 1 */

/* Any of these means the transfer failed; NAK is handled separately. */
#define TD_ERR_MASK       (TD_STALLED | TD_DBUFERR | TD_BABBLE | \
                           TD_CRCTIMEO | TD_BITSTUFF)

/* ---------------- TD token word bits ---------------------------------- */
#define TD_TOKEN_PID_MASK 0xFF
#define TD_TOKEN_ADDR_SHIFT 8
#define TD_TOKEN_EP_SHIFT   15
#define TD_TOKEN_TOGGLE   (1U << 19)
#define TD_TOKEN_LEN_SHIFT 21
#define TD_TOKEN_LEN_MASK 0x7FFU

/* expected length, encoded n-1 */
#define uhci_explen(n)    (((((u32)(n)) - 1) & TD_TOKEN_LEN_MASK) << TD_TOKEN_LEN_SHIFT)
/* actual length, decoded from status */
#define uhci_actlen(s)    ((((s) & TD_ACTLEN_MASK) + 1) & TD_ACTLEN_MASK)

/* ====================================================================== *
 * Hardware structures.  All 16-byte aligned; the link pointers carry the
 * low four bits as tags, so the low four bits of every address must be 0.
 * ====================================================================== */
typedef struct uhci_td {
    volatile u32 link;      /* next TD, or UHCI_PTR_TERM */
    volatile u32 status;
    volatile u32 token;
    volatile u32 buffer;    /* physical address of the data buffer */
} RHAP_ALIGN(16) uhci_td_t;

typedef struct uhci_qh {
    volatile u32 head;      /* next horizontal link (frame list/QH) */
    volatile u32 element;   /* first TD of this queue */
    volatile u32 reserved0;
    volatile u32 reserved1;
} RHAP_ALIGN(16) uhci_qh_t;

/* ====================================================================== *
 * Driver state.
 * ====================================================================== */
#define UHCI_MAX_TD        128
#define UHCI_MAX_IRQ_PIPE  16
#define UHCI_ASYNC_QH      0     /* qh[0] serves control transfers */
#define UHCI_IRQ_SLOT_BASE 16    /* frame list slots 16..31 for interrupts */
#define UHCI_MAX_PORTS     4

/* One control transfer is capped so the interrupt pipes always keep their
 * reserved descriptors: 512 bytes at an 8-byte packet size plus setup and
 * status stages, with headroom. */
#define UHCI_MAX_XFER_TD   72

struct uhci_irq_pipe {
    int  in_use;
    int  slot;                  /* frame list index */
    u8   addr;
    u8   ep;
    u8   toggle;
    u8   low_speed;
    u8   failed;
    u16  maxpkt;
    uhci_qh_t *qh;
    uhci_td_t *td;
    void *buf;
    u32  buflen;
    int  pending_len;           /* >0 when a packet is ready for the caller */
    int  last_len;              /* size of the most recent packet */
};

struct uhci {
    int  in_use;
    u16  iobase;
    int  nports;
    u8   revision;

    volatile u32 *fl;           /* 1024-entry frame list */
    rhap_paddr_t  fl_pa;

    uhci_qh_t    *qh;           /* UHCI_MAX_IRQ_PIPE + 1 queue heads */
    rhap_paddr_t  qh_pa;
    uhci_td_t    *td;           /* UHCI_MAX_TD transfer descriptors */
    rhap_paddr_t  td_pa;

    u32  td_free_at[UHCI_MAX_TD];   /* uptime (us) before reuse */
    u32  td_used[4];                /* bitmap of allocated TDs */

    rhap_lock_t lock;
    struct uhci_irq_pipe irq[UHCI_MAX_IRQ_PIPE];
    struct rhap_pci_dev pci;
    u16  port_shadow[UHCI_MAX_PORTS];   /* our last write to each PORTSC */
};

/* API used by the module glue. */
struct uhci *uhci_create(const struct rhap_pci_dev *pci);
void         uhci_destroy(struct uhci *h);
int          uhci_port_connected(struct uhci *h, int port);
int          uhci_port_speed(struct uhci *h, int port);
int          uhci_port_reset(struct uhci *h, int port);
u16          uhci_port_status(struct uhci *h, int port);
void         uhci_port_ack(struct uhci *h, int port);
void         uhci_service(struct uhci *h);

/* HCD operations table, filled by uhci.c. */
extern const struct usb_hcd_ops uhci_hcd_ops;

#ifdef __cplusplus
}
#endif

#endif /* _RHAP_UHCI_H */
