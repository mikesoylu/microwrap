CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

RELEASE_CFLAGS ?= -Os -DNDEBUG -ffunction-sections -fdata-sections \
	-fno-unwind-tables -fno-asynchronous-unwind-tables -fno-ident
RELEASE_MAX_PAGE_SIZE ?= 4096
RELEASE_LDFLAGS ?= -Wl,-O1,--gc-sections,--strip-all,--build-id=none \
	-Wl,-z,noseparate-code,-z,max-page-size=$(RELEASE_MAX_PAGE_SIZE)

all: microwrap

microwrap: microwrap.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

release:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) $(RELEASE_CFLAGS)" \
		LDFLAGS="$(LDFLAGS) $(RELEASE_LDFLAGS)" microwrap

install: microwrap
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 microwrap "$(DESTDIR)$(BINDIR)/microwrap"

clean:
	rm -f microwrap

.PHONY: all release install clean
