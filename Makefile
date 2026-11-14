# RhapsodyUSB -- UHCI + USB HID for Apple Rhapsody DR2 (x86)
#
# Two builds:
#   make          link the driver for DR2 (needs the DR2 toolchain/DDK)
#   make check    compile and run the host unit tests (needs any gcc)
#
# `make check` is the build that is verified.  The DR2 target is correct in
# structure but cannot be linked here: it needs the DR2 SDK headers, and the
# final link step depends on how your DR2 install loads driver modules.

CC         ?= cc
LD         ?= ld
HOST_CC    ?= gcc

CPPFLAGS   ?= -Iinclude -DRHAP_DR2_BUILD
CFLAGS     ?= -O2 -g -Wall -fno-builtin -fno-common -fno-strict-aliasing
# DR2 linked kernel modules with `ld -r`; Mac OS X Server 1.x+ wants -bundle.
LDFLAGS    ?= -r

TARGET     ?= build/RhapsodyUSB.reloc

SRCS = src/uhci.c \
       src/usb_core.c \
       src/hid_boot.c \
       src/rhap_usb_module.c \
       src/port_dr2.c

OBJS = $(SRCS:.c=.o)

HOST_SRCS = src/usb_core.c \
            src/hid_boot.c \
            src/uhci.c \
            host/port_host.c \
            host/mock_hcd.c \
            host/test_main.c

HOST_CFLAGS = -std=gnu89 -Wall -Wextra -Wno-unused-parameter \
              -Wdeclaration-after-statement -Iinclude -DRHAP_HOST_BUILD

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p build 2>/dev/null || true
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

check:
	@mkdir -p build 2>/dev/null || true
	$(HOST_CC) $(HOST_CFLAGS) -o build/hosttest.exe $(HOST_SRCS)
	./build/hosttest.exe

clean:
	rm -f $(OBJS) $(TARGET) build/hosttest.exe

.PHONY: all check clean
