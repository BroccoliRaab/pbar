.POSIX:

CFLAGS  =-std=c99 -O2 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
LDFLAGS =

# Backend selection: "wayland" or "x11"
BACKEND = x11

# Wayland definitions
wayland_CFLAGS  = -DBUILD_WAYLAND
wayland_LDFLAGS = -lwayland-client
wayland_OBJS    = wlr-layer-shell-protocol.o
wayland_HDRS    = wlr-layer-shell-protocol.h

WAYLAND_CFLAGS  = $(wayland_CFLAGS)
WAYLAND_LDFLAGS = $(wayland_LDFLAGS)
WAYLAND_OBJS    = $(wayland_OBJS)
WAYLAND_HDRS    = $(wayland_HDRS)

# X11 definitions
x11_CFLAGS      = -DBUILD_X11
x11_LDFLAGS     = -lxcb -lxcb-shm -lxcb-randr -lm
x11_OBJS        =
x11_HDRS        =

X11_CFLAGS      = $(x11_CFLAGS)
X11_LDFLAGS     = $(x11_LDFLAGS)
X11_OBJS        = $(x11_OBJS)
X11_HDRS        = $(x11_HDRS)

# Selected backend parameters
ALL_CFLAGS   = $(CFLAGS) $($(BACKEND)_CFLAGS)
ALL_LDFLAGS  = $(LDFLAGS) $($(BACKEND)_LDFLAGS)
BACKEND_OBJS = $($(BACKEND)_OBJS)
BACKEND_HDRS = $($(BACKEND)_HDRS)

all: pbar

wlr-layer-shell-unstable-v1.xml:
	curl -sO https://raw.githubusercontent.com/wayland-project/wayland-protocols/master/staging/wlr-layer-shell/wlr-layer-shell-unstable-v1.xml

wlr-layer-shell-protocol.h: wlr-layer-shell-unstable-v1.xml
	wayland-scanner client-header < wlr-layer-shell-unstable-v1.xml > wlr-layer-shell-protocol.h

wlr-layer-shell-protocol.c: wlr-layer-shell-unstable-v1.xml
	wayland-scanner private-code < wlr-layer-shell-unstable-v1.xml > wlr-layer-shell-protocol.c

wlr-layer-shell-protocol.o: wlr-layer-shell-protocol.c wlr-layer-shell-protocol.h
	$(CC) $(ALL_CFLAGS) -c wlr-layer-shell-protocol.c -o wlr-layer-shell-protocol.o

pbar.o: pbar.c pbar.h config.h backend_wayland.h backend_x11.h $(BACKEND_HDRS)
	$(CC) $(ALL_CFLAGS) -c pbar.c -o pbar.o

pbar: pbar.o $(BACKEND_OBJS)
	$(CC) -o pbar pbar.o $(BACKEND_OBJS) $(ALL_LDFLAGS)

clean:
	rm -f pbar pbar.o wlr-layer-shell-protocol.o wlr-layer-shell-protocol.c wlr-layer-shell-protocol.h wlr-layer-shell-unstable-v1.xml
