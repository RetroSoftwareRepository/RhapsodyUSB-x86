# RhapsodyUSB

A USB 1.1 stack for **Apple Rhapsody DR2 for Intel** (1998): UHCI host
controller, root-port enumeration, and HID boot-protocol keyboard/mouse with
ADB keycode translation.

---

## Read this first

Rhapsody DR2 has **no USB support whatsoever**. No `IOUSBFamily`, no hub or
enumeration layer, no HID system, no USB power management. DR2 also predates
IOKit: its driver model is DriverKit's `IODriver`, loaded as a Mach-O module,
not a kext bundle with an `Info.plist`.

So "a USB kext for DR2" cannot be a small driver that plugs into a USB stack.
It has to *be* the stack. That is what this is:

| Layer | File | Status |
|---|---|---|
| UHCI host controller (PCI probe, frame list, TD/QH engine, port reset) | `src/uhci.c` | implemented |
| Enumeration, descriptor parsing, driver binding | `src/usb_core.c` | implemented |
| HID boot keyboard + mouse, ADB keycode map | `src/hid_boot.c` | implemented |
| Module init, 1 ms pump, hot-plug rescan | `src/rhap_usb_module.c` | implemented |
| Rhapsody DriverKit wrapper | `DriverKit/rhapusb_iodriver.cpp` | ~60 lines, 3 things to check |
| OS interface (DMA, PIO, PCI, timer, input) | `src/port_dr2.c` | real for PIO/PCI, hooks for event driver |

**What is verified:** the portable logic (118 assertions) — descriptor
parsing including malformed input, UHCI length/token encodings, control
transfer chain construction, enumeration and driver binding against a mock
controller, HID report decoding and the ADB keymap.

**What is not verified:** anything requiring an actual DR2 machine or the DR2
DDK — the link step, `IOMallocContiguous`, `pmap_extract`, and the call into
Rhapsody's event driver. Those are isolated in `src/port_dr2.c` and flagged.

**What is checked but not linked:** all five translation units (`src/uhci.c`,
`src/usb_core.c`, `src/hid_boot.c`, `src/rhap_usb_module.c`, `src/port_dr2.c`)
compile cleanly with `-std=gnu89 -Wall -Wextra -Wdeclaration-after-statement
-DRHAP_DR2_BUILD -c`. That catches dialect and declaration-order mistakes; it
does not prove the DDK headers and `ld -r` invocation match your install.

---

## Design

```
  timer (1 ms)  ->  uhci_service()      re-arm / reap interrupt pipes
                ->  usb_poll_drivers()  hid_poll -> rhap_input_key/mouse

  frame list [0]      -> async QH  -> control transfer TD chain
  frame list [16..31] -> one QH per interrupt-IN endpoint
  frame list [1..15]  unused
```

Four decisions worth knowing about:

**Fully polled — no interrupts at all.** `USBINTR` is never enabled, so there
is no IRQ handler, no interrupt registration, no lock-ordering between
interrupt and thread context, and no DR2 interrupt API to guess at. The cost
is one timer tick per millisecond and ~1–2 % of a Pentium II.

**Queue heads, not bare TDs.** The hardware does not advance a QH's element
pointer past a TD that failed, *including NAK*, so a busy device is retried
every frame with zero software involvement. Putting TDs directly in the frame
list would let the status stage run after a NAKed data stage and silently
corrupt transfers.

**Boot protocol, not report parsing.** Every HID keyboard and mouse must
support boot reports; `SET_PROTOCOL(0)` + `SET_IDLE(0)` gets us 8-byte
keyboard and 3-byte mouse packets and saves several hundred lines of report
descriptor interpreter.

**ADB keycodes.** Rhapsody's event system is ADB-centric even on x86 — its
PS/2 driver translated scancodes into classic Apple keycodes before queueing
events. A USB keyboard driver for this OS therefore emits `0x00` = A,
`0x24` = Return, `0x33` = Delete, `0x35` = Escape, `0x7e` = Up, and so on.

---

## Building

### Host tests (verified)

Any gcc, no DR2 needed:

```sh
make check
# or by hand:
gcc -std=gnu89 -Wall -Wextra -Wno-unused-parameter \
    -Wdeclaration-after-statement -Iinclude -DRHAP_HOST_BUILD \
    -o build/hosttest.exe \
    src/usb_core.c src/hid_boot.c src/uhci.c \
    host/port_host.c host/mock_hcd.c host/test_main.c
./build/hosttest.exe
```

Expected output:

```
RhapsodyUSB host tests
118 checks, 0 failure(s)
```

The host build is C89 with `-Wdeclaration-after-statement`, the same dialect
the DR2 compiler (egcs 1.1) expects, so the code compiles with a 1998
toolchain rather than only a modern one.

### DR2 module

```sh
make CC=<your DR2 cross cc> LD=<your DR2 ld>
```

`make` produces `build/RhapsodyUSB.reloc` with `ld -r`. The exact link flavor
depends on your DR2 install: DR2 loads relocatable Mach-O modules through the
kernel loader. If your build wants `-bundle` instead, override `LDFLAGS`.

Two things to fix before this links on a real DDK:

1. The `extern` declarations at the top of `src/port_dr2.c` — drop any that
   your SDK headers already declare.
2. `rhap_input_key()` / `rhap_input_mouse()` at the bottom of that file: point
   them at whatever the event driver actually exports (the names used,
   `kbd_enqueue_event` / `mouse_enqueue_event`, are placeholders).

### Installing on DR2

DR2 has no `kextd` and does not read `Info.plist`. The module has to be
registered where your install's kernel loader looks for drivers (usually a
driver personality/config file naming class `RhapsodyUSB`, plus the module in
the drivers directory), then reboot. `DriverKit/Info.plist` and
`DriverKit/rhapusb_iodriver.cpp` are for the *IOKit-era* variant — Mac OS X
Server 1.x and later — if you want to port upward.

---

## Porting checklist

Everything OS-specific is in `src/port_dr2.c`. The functions that matter, and
what they need to do:

| Function | Requirement | DR2 primitive (verify) |
|---|---|---|
| `rhap_dma_alloc` | physically contiguous, 32-bit addressable, alignment honoured | `IOMallocContiguous` |
| `rhap_kvtophys` | kernel VA → PA, must handle stacks | `pmap_extract(kernel_pmap, va)` |
| `rhap_in/out*` | x86 port I/O | inline asm, already correct |
| `rhap_pci_find/read/write` | config mechanism #1 | `0xCF8/0xCFC`, already correct |
| `rhap_timer_start` | 1 ms periodic callback | self-rearming `timeout()` |
| `rhap_lock*` | non-sleeping mutex | `xchg`, already correct |
| `rhap_input_key` | ADB keycode into the event queue | **you must supply** |
| `rhap_input_mouse` | relative deltas + button bits | **you must supply** |

One real constraint: `timeout()` callbacks run in softclock context, where
sleeping is illegal. The pump itself is safe there (MMIO and buffers only),
but the event driver may not be. If so, replace `rhap_timer_start()` with a
kernel thread that calls the pump every millisecond — the comment in
`port_dr2.c` shows the shape.

---

## Hardware support

Supported:

- **UHCI** controllers: Intel PIIX3/PIIX4 (82371SB/AB), VIA VT82C586/596 and
  other UHCI clones. Matched as PCI class `0x0c`, subclass `0x03`, prog-if
  `0x00`.
- Full-speed and low-speed devices, directly on the two root ports.
- HID boot keyboards and mice, including composite devices.

Not supported (deliberately, with the work each would need):

- **OHCI** (class prog-if `0x10`) — different register set and TD format; a
  second HCD file, roughly the same size as `uhci.c`.
- **Hubs** — needs a hub class driver, port power management and split
  transactions for low-speed devices behind a full-speed hub.
- **Isochronous** and **bulk** transfers — no audio, no storage. Bulk is
  mostly a matter of removing the `USB_EP_INTERRUPT` check in
  `usb_pipe_open_int()` plus a retry loop.
- **Suspend/resume, power management, selective suspend** — none.
- **Multiple configurations** and alternate settings — config 1 only, no
  `SET_INTERFACE`.

Known sharp edges:

- Interrupt reports are polled at 1 kHz regardless of `bInterval`; the device
  simply NAKs the surplus polls.
- Only the most recent report is kept. A key press and release inside the same
  millisecond would be lost — physically impossible for a human, but a
  scripted USB device could do it.
- Released TDs are quarantined for 2 ms before reuse because the controller
  may still be prefetching them. Do not "optimise" that away.
- Hot-plug is polled every 250 ms via the root ports' connect-change bits.
  Devices yanked mid-transfer are handled by the enumeration timeout.

---

## Testing on QEMU

The cheapest real-bus test rig is QEMU with a PIIX3 UHCI controller, if you
can get DR2 to boot there:

```sh
qemu-system-i386 -M pc -m 128 \
  -device piix3-usb-uhci,id=usb \
  -device usb-kbd,bus=usb.0 -device usb-mouse,bus=usb.0
```

Then watch the log for:

```
uhci: 00:07.2 at io 0xc000 rev 1, 2 port(s)
usb: addr 1: 0627:0001 USB 1.10 ep0 8
hid: keyboard on interface 0, ep 0x81
```

---

## Files

```
include/rhap_types.h    fixed-width types, barriers, MMIO accessors
include/rhap_port.h     the entire OS interface (two dozen functions)
include/rhap_usb.h      USB 1.1 wire structures, topology, HCD/class ops
include/rhap_uhci.h     UHCI registers, TD/QH layout, driver state
src/uhci.c              host controller driver
src/usb_core.c          enumeration, parsing, binding, pipes
src/hid_boot.c          boot keyboard/mouse -> ADB keycodes
src/rhap_usb_module.c   init, pump, hot-plug
src/port_dr2.c          Rhapsody port (PIO/PCI real, rest flagged)
host/                   host port layer, mock UHCI device, unit tests
DriverKit/              IODriver wrapper + IOKit-era Info.plist
Makefile                target build, plus `make check`
```

References used for the controller details: Intel *UHCI Design Guide 1.1*
(TD/QH layout, `n-1` length encodings, port status bits) cross-checked against
Linux `drivers/usb/host/uhci-hcd.h` and `uhci-q.c`; USB 1.1 spec §9.2 for
enumeration and §9.6 for standard requests.
