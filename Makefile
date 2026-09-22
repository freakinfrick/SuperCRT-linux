# SuperCRT (Linux port).  Requires only libX11 + libGL (Mesa or NVIDIA) and pthread-free
# plain C99: glXGetProcAddress resolves everything newer than GL 1.1 at runtime.
CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -Wall -Wextra -Wno-unused-parameter -D_DEFAULT_SOURCE
LDLIBS  := -lX11 -lGL -ldl -lm

SRC := src/main.c src/capture.c src/marker.c src/assets.c src/params.c src/ui.c \
       src/shader.c src/gl_api.c
OBJ := $(SRC:.c=.o)

PREFIX ?= /usr/local

all: supercrt

supercrt: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

# Regenerates the embedded overlay font (needs Pillow; not needed to build).
font:
	~/venv/bin/python tools/bake_font.py > src/font_atlas.h

install: supercrt
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/supercrt/assets
	install -m 755 supercrt $(DESTDIR)$(PREFIX)/bin/supercrt
	install -m 644 assets/* $(DESTDIR)$(PREFIX)/share/supercrt/assets/

clean:
	rm -f $(OBJ) supercrt

.PHONY: all clean install font
