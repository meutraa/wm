CFLAGS ?= -pedantic -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare

CFLAGS += -O3 -DXWAYLAND -I. -DWLR_USE_UNSTABLE -std=c11

WAYLAND_PROTOCOLS=$(shell pkg-config --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER=$(shell pkg-config --variable=wayland_scanner wayland-scanner)

PKGS = wlroots wayland-server xcb xkbcommon libinput
CFLAGS += $(foreach p,$(PKGS),$(shell pkg-config --cflags $(p)))
LDLIBS += $(foreach p,$(PKGS),$(shell pkg-config --libs $(p)))

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
