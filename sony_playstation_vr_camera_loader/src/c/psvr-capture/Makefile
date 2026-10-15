# psvr-capture: camera owner, microphone decoder and preview encoder.
#
#   make                 build ./psvr-capture (needs librnnoise and libturbojpeg)
#   make install         copy it to $(PREFIX)/bin
#   make objects         build only the library objects (used by the tests)
#
# Unit tests and simulations live in .github/tests/c/psvr-capture of the
# repository, so a Home Assistant install never spends time compiling them.
#
# CFLAGS/LDFLAGS are for the caller (optimisation, include and library paths);
# the flags the code needs live in ENGINE_CFLAGS, so a command-line CFLAGS
# cannot drop them.
CC ?= cc
CFLAGS ?= -O2
ENGINE_CFLAGS = $(CFLAGS) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE
LDLIBS = -lrnnoise -lturbojpeg -lm
PREFIX ?= /usr/local
BUILD ?= build

SOURCES = $(wildcard *.c)
OBJECTS = $(patsubst %.c,$(BUILD)/%.o,$(SOURCES))
LIBRARY_OBJECTS = $(filter-out $(BUILD)/main.o,$(OBJECTS))

psvr-capture: $(OBJECTS)
	$(CC) $(ENGINE_CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c $(wildcard *.h) | $(BUILD)
	$(CC) $(ENGINE_CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

objects: $(LIBRARY_OBJECTS)

install: psvr-capture
	install -D -m 755 psvr-capture $(DESTDIR)$(PREFIX)/bin/psvr-capture

clean:
	rm -rf psvr-capture $(BUILD)

.PHONY: objects install clean
