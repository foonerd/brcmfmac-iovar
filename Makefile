# brcmfmac_iovar - Makefile
#
# Native build:
#   make
#
# Cross-compile for Volumio 4 armhf (Raspberry Pi):
#   make CROSS_COMPILE=arm-linux-gnueabihf-
#
# Cross-compile for Volumio 4 aarch64 (Pi 4/5 64-bit):
#   make CROSS_COMPILE=aarch64-linux-gnu-
#
# Install to Volumio system:
#   scp brcmfmac_iovar volumio@volumio.local:/usr/local/bin/
#
# Dependencies (build host):
#   libnl-3-dev libnl-genl-3-dev
#   For cross-compile: matching target-arch libnl packages or sysroot
#
# Dependencies (target runtime):
#   libnl-3-200 libnl-genl-3-200
#

PROG     = brcmfmac_iovar
SRC      = brcmfmac_iovar.c

CC       = $(CROSS_COMPILE)gcc
STRIP    = $(CROSS_COMPILE)strip

CFLAGS   = -Wall -Wextra -Werror -O2 -std=gnu11
CFLAGS  += $(shell pkg-config --cflags libnl-3.0 libnl-genl-3.0 2>/dev/null)

LDFLAGS  =
LIBS     = $(shell pkg-config --libs libnl-3.0 libnl-genl-3.0 2>/dev/null)

# Fallback if pkg-config unavailable (cross-compile with manual sysroot)
ifeq ($(LIBS),)
CFLAGS  += -I/usr/include/libnl3
LIBS     = -lnl-genl-3 -lnl-3
endif

.PHONY: all clean install strip

all: $(PROG)

$(PROG): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

strip: $(PROG)
	$(STRIP) $(PROG)

clean:
	rm -f $(PROG)

install: $(PROG)
	install -m 0755 $(PROG) $(DESTDIR)/usr/local/bin/
