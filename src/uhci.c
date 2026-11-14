/*
 * uhci.c -- Intel UHCI host controller driver.
 *
 * Runs the controller in a fully polled mode:
 *
 *   - frame list slot 0  -> async QH  -> control transfer chains
 *   - frame list 16..31  -> one QH per interrupt-IN endpoint
 *   - no interrupt enable bits are ever set; uhci_service() is called from a
 *     1 ms kernel timer and is the only place that re-arms periodic work.
 *
 * Queue heads are used even for a single TD because they give us NAK retry
 * for free: the controller does not advance the QH element pointer past a
 * TD that completed with an error (including NAK), so a busy device is
 * retried every frame without software intervention.
 *
 * Interrupt latency is therefore 1 frame plus up to one service period, i.e.
 * roughly 2 ms worst case.  That is far below what a boot-protocol keyboard
 * or mouse needs, and it costs one timer tick instead of an IRQ handler.
 */

#include "rhap_types.h"
#include "rhap_port.h"
#include "rhap_usb.h"
#include "rhap_uhci.h"

static struct uhci g_hcd[USB_MAX_HCD];

/* ---------------------------------------------------------------------- *
 * Small helpers
 * ---------------------------------------------------------------------- */
static void uhci_zero(void *p, u32 n)
{
    u8 *b = (u8 *)p;

    while (n--)
        *b++ = 0;
}

static u16 uhci_rd16(struct uhci *h, u16 reg)
{
    return rhap_in16((u16)(h->iobase + reg));
}

static void uhci_wr16(struct uhci *h, u16 reg, u16 val)
{
    rhap_out16((u16)(h->iobase + reg), val);
}

static void uhci_wr32(struct uhci *h, u16 reg, u32 val)
{
    rhap_out32((u16)(h->iobase + reg), val);
}
static u32 td_pa(struct uhci *h, uhci_td_t *td)
{
    return h->td_pa + (u32)((u8 *)td - (u8 *)h->td);
}

static u32 qh_pa(struct uhci *h, uhci_qh_t *qh)
{
    return h->qh_pa + (u32)((u8 *)qh - (u8 *)h->qh);
}

/*
 * Fill a transfer descriptor.  The status word carries ACTIVE and is
 * written last, but nothing executes until the descriptors are reachable
 * from a queue head, which happens under an explicit barrier.
 */
static void td_fill(uhci_td_t *td, u32 status, u32 token, u32 buf, u32 link)
{
    rhap_write32(&td->link, link);
    rhap_write32(&td->buffer, buf);
    rhap_write32(&td->token, token);
    rhap_barrier();
    rhap_write32(&td->status, status);
    rhap_barrier();
}

/* ---------------------------------------------------------------------- *
 * Transfer descriptor pool
 *
 * Released descriptors are not handed out again for 2 ms: the controller
 * may still be reading a TD it fetched just before we unlinked the queue.
 * ---------------------------------------------------------------------- */
static uhci_td_t *td_alloc(struct uhci *h)
{
    u32 now = (u32)rhap_uptime_us();
    int i;

    for (i = 0; i < UHCI_MAX_TD; i++) {
        u32 bit = 1u << (i & 31);
        u32 *word = &h->td_used[i >> 5];

        if (*word & bit)
            continue;
        if (h->td_free_at[i] && (s32)(now - h->td_free_at[i]) < 0)
            continue;
        *word |= bit;
        h->td_free_at[i] = 0;
        uhci_zero(&h->td[i], sizeof(uhci_td_t));
        return &h->td[i];
    }
    return RHAP_NULL;
}

static void td_release(struct uhci *h, uhci_td_t *td)
{
    int i = (int)(td - h->td);
    u32 bit = 1u << (i & 31);
    u32 *word = &h->td_used[i >> 5];

    *word &= ~bit;
    h->td_free_at[i] = (u32)rhap_uptime_us() + 2000;
}

/* ---------------------------------------------------------------------- *
 * Ports
 * ---------------------------------------------------------------------- */
static void uhci_port_set(struct uhci *h, int port, u16 clear, u16 set)
{
    u16 v = h->port_shadow[port];

    v = (u16)((v & ~clear) | set);
    v = (u16)((v & (PORTSC_RW | PORTSC_RES1)) | PORTSC_RES1);
    /* Read-only and reserved bits are written as zero. */
    uhci_wr16(h, UHCI_PORTSC(port), v);
    h->port_shadow[port] = v;
}

static void uhci_port_clear_changes(struct uhci *h, int port)
{
    u16 v = (u16)(h->port_shadow[port] | PORTSC_CSC | PORTSC_PEC |
                  PORTSC_OCC | PORTSC_RD);

    uhci_wr16(h, UHCI_PORTSC(port), v);
    h->port_shadow[port] =
        (u16)(h->port_shadow[port] & ~(PORTSC_CSC | PORTSC_PEC |
                                       PORTSC_OCC | PORTSC_RD));
}

int uhci_port_connected(struct uhci *h, int port)
{
    if (!h || port < 0 || port >= h->nports)
        return 0;
    return (uhci_rd16(h, UHCI_PORTSC(port)) & PORTSC_CCS) != 0;
}

int uhci_port_speed(struct uhci *h, int port)
{
    if (!h || port < 0 || port >= h->nports)
        return USB_SPEED_FULL;
    return (uhci_rd16(h, UHCI_PORTSC(port)) & PORTSC_LSDA)
               ? USB_SPEED_LOW
               : USB_SPEED_FULL;
}

u16 uhci_port_status(struct uhci *h, int port)
{
    if (!h || port < 0 || port >= h->nports)
        return 0;
    return uhci_rd16(h, UHCI_PORTSC(port));
}

void uhci_port_ack(struct uhci *h, int port)
{
    if (!h || port < 0 || port >= h->nports)
        return;
    uhci_port_clear_changes(h, port);
}

int uhci_port_reset(struct uhci *h, int port)
{
    u16 ps;
    int i;

    if (!h || !h->in_use || port < 0 || port >= h->nports)
        return -1;

    /* A UHCI port will not reset while it is disabled. */
    uhci_port_set(h, port, 0, PORTSC_PE);
    rhap_udelay(2000);

    uhci_port_set(h, port, 0, PORTSC_PR);
    rhap_udelay(50000);              /* USB 1.1 7.1.7.3: 50 ms minimum */
    uhci_port_set(h, port, PORTSC_PR, 0);
    rhap_udelay(10000);              /* reset recovery time */

    /* Some devices take a while to be re-enabled by the controller. */
    for (i = 0; i < 100; i++) {
        ps = uhci_rd16(h, UHCI_PORTSC(port));
        if (ps & PORTSC_PE)
            break;
        if (!(ps & PORTSC_CCS))
            return -1;
        uhci_port_set(h, port, 0, PORTSC_PE);
        rhap_udelay(1000);
    }

    uhci_port_clear_changes(h, port);
    ps = uhci_rd16(h, UHCI_PORTSC(port));
    if (!(ps & PORTSC_PE)) {
        rhap_log(0, "uhci: port %d did not enable after reset", port + 1);
        return -1;
    }
    h->port_shadow[port] =
        (u16)((h->port_shadow[port] & PORTSC_RW) | PORTSC_RES1);
    return 0;
}

/* ---------------------------------------------------------------------- *
 * Host controller bring-up
 * ---------------------------------------------------------------------- */
static int uhci_controller_reset(struct uhci *h)
{
    int i;

    uhci_wr16(h, UHCI_USBCMD, 0);
    rhap_udelay(20000);

    uhci_wr16(h, UHCI_USBCMD, USBCMD_HCRESET);
    for (i = 0; i < 50; i++) {
        if (!(uhci_rd16(h, UHCI_USBCMD) & USBCMD_HCRESET))
            break;
        rhap_udelay(1000);
    }
    if (uhci_rd16(h, UHCI_USBCMD) & USBCMD_HCRESET) {
        rhap_log(0, "uhci: controller reset timed out");
        return -1;
    }

    /* Idle schedule: every frame entry terminated, every QH empty. */
    for (i = 0; i < 1024; i++)
        rhap_write32(&h->fl[i], UHCI_PTR_TERM);
    for (i = 0; i <= UHCI_MAX_IRQ_PIPE; i++) {
        rhap_write32(&h->qh[i].head, UHCI_PTR_TERM);
        rhap_write32(&h->qh[i].element, UHCI_PTR_TERM);
    }
    rhap_write32(&h->fl[0], qh_pa(h, &h->qh[UHCI_ASYNC_QH]) | UHCI_PTR_QH);
    rhap_barrier();

    uhci_wr32(h, UHCI_FLBASEADD, h->fl_pa);
    rhap_out8((u16)(h->iobase + UHCI_SOFMOD), USBSOF_1MS);
    uhci_wr16(h, UHCI_USBINTR, 0);          /* polled operation */
    uhci_wr16(h, UHCI_USBSTS, USBSTS_W1C);  /* clear stale status */
    uhci_wr16(h, UHCI_USBCMD, USBCMD_RS | USBCMD_CF | USBCMD_MAXP);
    rhap_udelay(10000);

    if (uhci_rd16(h, UHCI_USBSTS) & USBSTS_HCAA) {
        rhap_log(0, "uhci: controller halted after start");
        return -1;
    }
    return 0;
}

struct uhci *uhci_create(const struct rhap_pci_dev *pci)
{
    struct uhci *h;
    u32 bar;
    u16 cmd;
    int i;

    if (!pci)
        return RHAP_NULL;
    h = RHAP_NULL;
    for (i = 0; i < USB_MAX_HCD; i++) {
        if (!g_hcd[i].in_use) {
            h = &g_hcd[i];
            break;
        }
    }
    if (!h) {
        rhap_log(0, "uhci: out of controller slots");
        return RHAP_NULL;
    }

    uhci_zero(h, sizeof *h);
    h->in_use = 1;
    h->pci = *pci;
    rhap_lock_init(&h->lock);

    bar = rhap_pci_read32(pci, 0x10);
    if (!(bar & 1)) {
        rhap_log(0, "uhci: BAR0 is not an I/O window (0x%08x)", bar);
        h->in_use = 0;
        return RHAP_NULL;
    }
    h->iobase = (u16)(bar & 0xFFFC);
    h->revision = (u8)(rhap_pci_read32(pci, 0x08) & 0xFF);

    /* Memory space off, I/O space on, bus master on. */
    cmd = rhap_pci_read16(pci, 0x04);
    rhap_pci_write16(pci, 0x04, (u16)((cmd | 0x0001 | 0x0004) & ~0x0002));

    /* Drop legacy SMI routing; we never want the BIOS handling our ports. */
    rhap_pci_write16(pci, 0xC0, 0x2000);

    h->fl = (volatile u32 *)rhap_dma_alloc(4096, 4096, &h->fl_pa);
    h->qh = (uhci_qh_t *)rhap_dma_alloc(sizeof(uhci_qh_t) * (UHCI_MAX_IRQ_PIPE + 1),
                                        16, &h->qh_pa);
    h->td = (uhci_td_t *)rhap_dma_alloc(sizeof(uhci_td_t) * UHCI_MAX_TD,
                                        16, &h->td_pa);
    if (!h->fl || !h->qh || !h->td) {
        rhap_log(0, "uhci: out of DMA memory");
        uhci_destroy(h);
        return RHAP_NULL;
    }

    if (uhci_controller_reset(h) != 0) {
        uhci_destroy(h);
        return RHAP_NULL;
    }

    /* Probe ports: a missing port reads back as 0xffff (floating I/O). */
    h->nports = 0;
    for (i = 0; i < UHCI_MAX_PORTS; i++) {
        u16 ps = uhci_rd16(h, UHCI_PORTSC(i));

        if (ps == 0xFFFF)
            break;
        h->port_shadow[i] = (u16)((ps & (PORTSC_RW | PORTSC_RES1)) |
                                  PORTSC_RES1);
        h->nports++;
    }
    if (h->nports == 0) {
        rhap_log(0, "uhci: no ports found");
        uhci_destroy(h);
        return RHAP_NULL;
    }

    rhap_log(1, "uhci: %02x:%02x.%x at io 0x%04x rev %d, %d port(s)",
             pci->bus, pci->slot, pci->func, h->iobase, h->revision,
             h->nports);
    return h;
}

void uhci_destroy(struct uhci *h)
{
    if (!h || !h->in_use)
        return;
    if (h->iobase) {
        uhci_wr16(h, UHCI_USBCMD, 0);
        uhci_wr16(h, UHCI_USBINTR, 0);
    }
    if (h->fl)
        rhap_dma_free((void *)h->fl, 4096);
    if (h->qh)
        rhap_dma_free(h->qh, sizeof(uhci_qh_t) * (UHCI_MAX_IRQ_PIPE + 1));
    if (h->td)
        rhap_dma_free(h->td, sizeof(uhci_td_t) * UHCI_MAX_TD);
    h->fl = RHAP_NULL;
    h->qh = RHAP_NULL;
    h->td = RHAP_NULL;
    h->in_use = 0;
}

/* ---------------------------------------------------------------------- *
 * Control transfers (endpoint 0)
 * ---------------------------------------------------------------------- */
static int control_ntd(u8 ep0_max, u16 len)
{
    int n;

    if (ep0_max == 0)
        ep0_max = 8;
    n = 2;                                  /* setup + status */
    if (len > 0)
        n += ((int)len + ep0_max - 1) / ep0_max;
    return n;
}

/*
 * Fill a control transfer chain into caller-provided descriptors.
 *
 * Pure with respect to the controller: no MMIO, no allocation.  That is
 * deliberate -- the toggle sequence, packet directions and the n-1 length
 * encodings are the easiest things to get subtly wrong, so they are unit
 * tested on the host (see host/test_main.c).
 */
static void control_fill(uhci_td_t **td, u8 addr, u8 ep0_max, u8 speed,
                         const struct usb_setup *setup, void *data, u16 len,
                         rhap_paddr_t setup_pa)
{
    u32 base, status, dest;
    int n;

    if (ep0_max == 0)
        ep0_max = 8;

    n = 0;
    base = ((u32)(addr & 0x7F) << 8);           /* endpoint number 0 */
    status = (u32)(TD_CERR(3) | (speed == USB_SPEED_LOW ? TD_LS : 0));

    /* setup stage, always DATA0, always 8 bytes */
    td_fill(td[n], status | TD_ACTIVE,
            base | USB_PID_SETUP | uhci_explen(8), setup_pa, UHCI_PTR_TERM);
    n++;

    dest = base;
    if (len == 0 || !(setup->bmRequestType & USB_DIR_IN))
        dest |= USB_PID_OUT;
    else
        dest |= USB_PID_IN;

    if (len > 0) {
        u32 bufpa = rhap_kvtophys(data);
        u32 st = status | TD_ACTIVE;
        u16 left = len;
        int in = (setup->bmRequestType & USB_DIR_IN) != 0;

        /* A short packet legitimately ends an IN data stage. */
        if (in)
            st |= TD_SPD;

        while (left > 0) {
            u16 chunk = ep0_max;

            if (chunk > left)
                chunk = left;
            dest ^= TD_TOKEN_TOGGLE;            /* DATA1, DATA0, DATA1... */
            td_fill(td[n], st, dest | uhci_explen(chunk), bufpa,
                    UHCI_PTR_TERM);
            n++;
            bufpa += chunk;
            left = (u16)(left - chunk);
        }
    }

    /* status stage: opposite direction, always DATA1, zero length */
    dest ^= (USB_PID_IN ^ USB_PID_OUT);
    dest |= TD_TOKEN_TOGGLE;
    td_fill(td[n], status | TD_ACTIVE, dest | uhci_explen(0), 0,
            UHCI_PTR_TERM);
}

static int uhci_control(void *ctx, u8 addr, u8 ep0_max, u8 speed,
                        const struct usb_setup *setup, void *data, u16 len,
                        u32 timeout_ms)
{
    struct uhci *h = (struct uhci *)ctx;
    uhci_td_t *td[UHCI_MAX_XFER_TD];
    u32 deadline, now;
    int ntd, i, rc, nbytes;

    if (!h || !h->in_use || !setup)
        return USB_ERR_NODEV;
    if (len > 0 && !data)
        return USB_ERR_PROTO;
    if (len > 512)
        return USB_ERR_PROTO;
    if (ep0_max == 0)
        ep0_max = 8;

    ntd = control_ntd(ep0_max, len);
    if (ntd > UHCI_MAX_XFER_TD)
        return USB_ERR_PROTO;

    rc = USB_OK;
    nbytes = 0;
    for (i = 0; i < ntd; i++)
        td[i] = RHAP_NULL;

    rhap_lock(&h->lock);

    for (i = 0; i < ntd; i++) {
        td[i] = td_alloc(h);
        if (!td[i]) {
            rc = USB_ERR_NOMEM;
            goto out;
        }
    }

    control_fill(td, addr, ep0_max, speed, setup, data, len,
                 rhap_kvtophys((void *)setup));

    /* ---- publish the chain ---- */
    for (i = 0; i < ntd - 1; i++)
        rhap_write32(&td[i]->link, td_pa(h, td[i + 1]));
    rhap_barrier();
    rhap_write32(&h->qh[UHCI_ASYNC_QH].element, td_pa(h, td[0]));
    rhap_barrier();

    /* ---- poll for completion; the QH retries NAKs on its own ---- */
    deadline = (u32)rhap_uptime_us() + timeout_ms * 1000u;
    for (;;) {
        if (!(rhap_read32(&td[ntd - 1]->status) & TD_ACTIVE))
            break;

        for (i = 0; i < ntd; i++) {
            u32 st = rhap_read32(&td[i]->status);

            if ((st & TD_ACTIVE) || !(st & TD_ERR_MASK))
                continue;
            rc = (st & TD_STALLED) ? USB_ERR_STALL : USB_ERR_IO;
            goto unlink;
        }

        now = (u32)rhap_uptime_us();
        if ((s32)(now - deadline) >= 0) {
            rc = USB_ERR_TIMEOUT;
            goto unlink;
        }
        rhap_udelay(20);
    }

    if (len > 0 && (setup->bmRequestType & USB_DIR_IN)) {
        for (i = 1; i < ntd - 1; i++) {
            u32 st = rhap_read32(&td[i]->status);

            if (st & TD_ACTIVE)
                break;
            nbytes += (int)uhci_actlen(st);
        }
        if (nbytes > (int)len)
            nbytes = (int)len;
        rc = nbytes;
    }

unlink:
    /*
     * Stop the queue before touching the descriptors.  The 2 ms TD reuse
     * delay in td_release() covers the controller's in-flight prefetch.
     */
    rhap_write32(&h->qh[UHCI_ASYNC_QH].element, UHCI_PTR_TERM);
    rhap_barrier();

out:
    for (i = 0; i < ntd; i++)
        if (td[i])
            td_release(h, td[i]);
    rhap_unlock(&h->lock);
    return rc;
}

#ifdef RHAP_HOST_BUILD
/*
 * Test hook: build a control chain into caller-supplied descriptors and
 * return the number used.  Lets the host tests check the toggle sequence
 * and packet directions without a controller.
 */
int uhci_test_fill_control(uhci_td_t **td, u8 addr, u8 ep0_max, u8 speed,
                           const struct usb_setup *setup, void *data, u16 len)
{
    int n, i;

    n = control_ntd(ep0_max, len);
    control_fill(td, addr, ep0_max, speed, setup, data, len,
                 rhap_kvtophys((void *)setup));
    for (i = 0; i < n - 1; i++)
        td[i]->link = rhap_kvtophys(td[i + 1]);
    return n;
}
#endif

/* ---------------------------------------------------------------------- *
 * Interrupt-IN pipes
 * ---------------------------------------------------------------------- */
static void irq_arm(struct uhci *h, struct uhci_irq_pipe *p)
{
    u32 st, tok;
    u16 len;

    len = p->maxpkt;
    if (len > p->buflen)
        len = (u16)p->buflen;
    if (len == 0)
        len = 8;

    st = TD_ACTIVE | TD_CERR(3) | TD_SPD;
    if (p->low_speed)
        st |= TD_LS;

    tok = ((u32)(p->addr & 0x7F) << 8) |
          ((u32)(p->ep & 0x0F) << 15) |
          ((u32)(p->toggle & 1) << 19) |
          USB_PID_IN |
          uhci_explen(len);

    td_fill(p->td, st, tok, rhap_kvtophys(p->buf), UHCI_PTR_TERM);
    rhap_barrier();
    rhap_write32(&p->qh->element, td_pa(h, p->td));
    rhap_barrier();
}

static void irq_service_one(struct uhci *h, struct uhci_irq_pipe *p)
{
    u32 st;
    int n;

    st = rhap_read32(&p->td->status);
    if (st & TD_ACTIVE)
        return;                     /* still in flight this frame */

    if (st & TD_ERR_MASK) {
        if (!p->failed)
            rhap_log(0, "uhci: interrupt ep %d.%d failed (status %08x)",
                     p->addr, p->ep, st);
        p->failed = 1;
        rhap_write32(&p->qh->element, UHCI_PTR_TERM);
        return;
    }

    if (!(st & TD_NAK)) {
        n = (int)uhci_actlen(st);
        if (n > (int)p->buflen)
            n = (int)p->buflen;
        if (n > 0) {
            p->pending_len = n;
            p->last_len = n;
        }
        /*
         * A zero-length packet is a successful transaction too and consumes
         * a data toggle, so flip unconditionally.  (Boot devices normally NAK
         * instead of sending a ZLP, but a wrong toggle wedges the endpoint.)
         */
        p->toggle ^= 1;
    }

    irq_arm(h, p);
}

static void *uhci_open_int_pipe(void *ctx, u8 addr, u8 ep, u16 maxpkt,
                                u8 interval, u8 speed, void *buf, u32 buflen)
{
    struct uhci *h = (struct uhci *)ctx;
    struct uhci_irq_pipe *p = RHAP_NULL;
    int i;

    if (!h || !h->in_use || !buf || buflen == 0)
        return RHAP_NULL;
    if (maxpkt == 0)
        maxpkt = 8;
    if (maxpkt > buflen)
        maxpkt = (u16)buflen;

    /*
     * interval is advisory: every pipe is serviced once per frame and
     * re-armed once per service tick, so the effective poll rate is 1 kHz
     * whatever the descriptor asks for.  Devices simply NAK the surplus
     * polls, and a keyboard report is 8 bytes.
     */
    (void)interval;

    rhap_lock(&h->lock);
    for (i = 0; i < UHCI_MAX_IRQ_PIPE; i++) {
        if (!h->irq[i].in_use) {
            p = &h->irq[i];
            break;
        }
    }
    if (!p) {
        rhap_unlock(&h->lock);
        return RHAP_NULL;
    }

    uhci_zero(p, sizeof *p);
    p->in_use = 1;
    p->addr = addr;
    p->ep = (u8)(ep & 0x0F);
    p->maxpkt = maxpkt;
    p->low_speed = (speed == USB_SPEED_LOW);
    p->buf = buf;
    p->buflen = buflen;
    p->slot = UHCI_IRQ_SLOT_BASE + i;
    p->qh = &h->qh[i + 1];
    p->td = td_alloc(h);
    if (!p->td) {
        p->in_use = 0;
        rhap_unlock(&h->lock);
        return RHAP_NULL;
    }

    rhap_write32(&p->qh->head, UHCI_PTR_TERM);
    rhap_write32(&p->qh->element, UHCI_PTR_TERM);
    irq_arm(h, p);
    rhap_barrier();
    rhap_write32(&h->fl[p->slot], qh_pa(h, p->qh) | UHCI_PTR_QH);
    rhap_unlock(&h->lock);

    rhap_log(2, "uhci: interrupt pipe %d.%d maxpkt %d in slot %d",
             addr, p->ep, maxpkt, p->slot);
    return p;
}

static void uhci_close_pipe(void *ctx, void *pipe)
{
    struct uhci *h = (struct uhci *)ctx;
    struct uhci_irq_pipe *p = (struct uhci_irq_pipe *)pipe;

    if (!h || !p || !p->in_use)
        return;
    rhap_lock(&h->lock);
    rhap_write32(&h->fl[p->slot], UHCI_PTR_TERM);
    rhap_write32(&p->qh->element, UHCI_PTR_TERM);
    rhap_barrier();
    if (p->td)
        td_release(h, p->td);
    p->td = RHAP_NULL;
    p->in_use = 0;
    rhap_unlock(&h->lock);
}

static int uhci_pipe_poll(void *ctx, void *pipe)
{
    struct uhci *h = (struct uhci *)ctx;
    struct uhci_irq_pipe *p = (struct uhci_irq_pipe *)pipe;
    int n;

    if (!h || !p)
        return 0;
    rhap_lock(&h->lock);
    n = p->pending_len;
    if (n > 0)
        p->pending_len = 0;
    rhap_unlock(&h->lock);
    return n > 0;
}

static int uhci_pipe_actual(void *ctx, void *pipe)
{
    struct uhci_irq_pipe *p = (struct uhci_irq_pipe *)pipe;

    (void)ctx;
    if (!p)
        return 0;
    return p->last_len;
}

void uhci_service(struct uhci *h)
{
    int i;

    if (!h || !h->in_use)
        return;
    rhap_lock(&h->lock);
    for (i = 0; i < UHCI_MAX_IRQ_PIPE; i++) {
        if (h->irq[i].in_use)
            irq_service_one(h, &h->irq[i]);
    }
    rhap_unlock(&h->lock);
}

const struct usb_hcd_ops uhci_hcd_ops = {
    uhci_control,
    uhci_open_int_pipe,
    uhci_close_pipe,
    uhci_pipe_poll,
    uhci_pipe_actual
};
