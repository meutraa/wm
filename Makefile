CFLAGS ?= -pedantic -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare

CFLAGS += -static -O3 -DXWAYLAND -I. -DWLR_USE_UNSTABLE -std=c11

WAYLAND_PROTOCOLS=$(shell pkg-config --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER=$(shell pkg-config --variable=wayland_scanner wayland-scanner)

CFLAGS += $(shell pkg-config --cflags wlroots)
CFLAGS += $(shell pkg-config --cflags wayland-server)
CFLAGS += $(shell pkg-config --cflags --static xcb)
CFLAGS += $(shell pkg-config --cflags xkbcommon)
CFLAGS += $(shell pkg-config --cflags libinput)
LDLIBS += $(shell pkg-config --libs wlroots)
LDLIBS += $(shell pkg-config --libs wayland-server)
LDLIBS += $(shell pkg-config --libs --static xcb)
LDLIBS += $(shell pkg-config --libs xkbcommon)
LDLIBS += $(shell pkg-config --libs libinput)

all: wm

xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.o: xdg-shell-protocol.h

wm.o: xdg-shell-protocol.h

wm: xdg-shell-protocol.o

clean:
	rm -f wm *.o *-protocol.h *-protocol.c

.DEFAULT_GOAL=wm
.PHONY: clean
