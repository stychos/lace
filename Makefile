# Lace - Database Viewer and Manager
# Top-level Makefile

.PHONY: all laced liblace ncurses clean distclean install help

# Default target
all: laced liblace ncurses

# Build the daemon
laced:
	$(MAKE) -C laced

# Build the client library
liblace:
	$(MAKE) -C liblace

# Build the ncurses frontend (depends on liblace)
ncurses: liblace
	$(MAKE) -C tui/ncurses

# Clean all build artifacts
clean:
	$(MAKE) -C laced clean
	$(MAKE) -C liblace clean
	$(MAKE) -C tui/ncurses clean

# Full clean
distclean: clean
	rm -rf build

# Install (requires PREFIX or DESTDIR)
PREFIX ?= /usr/local
DESTDIR ?=

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include/lace
	install -m 755 laced/build/laced $(DESTDIR)$(PREFIX)/bin/
	install -m 755 tui/ncurses/build/lace $(DESTDIR)$(PREFIX)/bin/
	install -m 644 liblace/build/liblace.a $(DESTDIR)$(PREFIX)/lib/
	install -m 644 liblace/include/*.h $(DESTDIR)$(PREFIX)/include/lace/

# Run the ncurses frontend
run: all
	./tui/ncurses/build/lace

# Help
help:
	@echo "Lace - Database Viewer and Manager"
	@echo ""
	@echo "Targets:"
	@echo "  all       Build everything (default)"
	@echo "  laced     Build the daemon"
	@echo "  liblace   Build the client library"
	@echo "  ncurses   Build the ncurses frontend"
	@echo "  clean     Remove build artifacts"
	@echo "  distclean Remove all artifacts"
	@echo "  install   Install to PREFIX (default: /usr/local)"
	@echo "  run       Build and run the ncurses frontend"
	@echo ""
	@echo "Variables:"
	@echo "  PREFIX    Installation prefix (default: /usr/local)"
	@echo "  DESTDIR   Staging directory for packaging"
