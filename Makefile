# Lace - Database Viewer and Manager
# Top-level Makefile

.PHONY: all no-gtk no-macos laced liblace ncurses notcurses test-notcurses install-notcurses run-notcurses gtk macos test test-gtk test-macos smoke-macos dist-macos test-tui test-integration test-windows fuzz stress clean distclean install install-gtk help

# Default target: every component, the GUI frontends included where their
# toolkit is present (the GTK libraries through pkg-config; Swift on macOS).
# A missing toolkit skips that frontend with a note instead of failing.
HAVE_GTK := $(shell pkg-config --exists gtk4 libadwaita-1 gtksourceview-5 2>/dev/null && echo yes)
HAVE_SWIFT := $(shell [ "$$(uname -s)" = Darwin ] && command -v swift >/dev/null 2>&1 && echo yes)
GUI_TARGETS := $(if $(HAVE_GTK),gtk,no-gtk) $(if $(HAVE_SWIFT),macos,no-macos)
all: laced liblace ncurses notcurses $(GUI_TARGETS)

no-gtk:
	@echo "lace-gtk skipped: gtk4, libadwaita-1 or gtksourceview-5 not found (pkg-config)"
no-macos:
	@echo "Lace.app skipped: not macOS, or no swift toolchain"

# Build the daemon
laced: liblace
	$(MAKE) -C laced

# Build the client library
liblace:
	$(MAKE) -C liblace

# Build the ncurses frontend (depends on liblace)
ncurses: liblace
	$(MAKE) -C tui/ncurses

# Build the notcurses frontend (tui/notcurses; needs notcurses-core)
notcurses: liblace laced
	$(MAKE) -C tui/notcurses

# notcurses unit tests, then its tests in a real terminal (needs tmux)
test-notcurses: notcurses
	$(MAKE) -C tui/notcurses test
	python3 tests/notcurses/run.py $(TUI_ARGS)

# Build the GTK4/libadwaita frontend (gui/gtk; needs gtk4, libadwaita-1,
# gtksourceview-5). Part of `all` when those are installed.
gtk: liblace laced
	$(MAKE) -C gui/gtk

# GTK unit tests (the row model; the SQL builders are liblace's now)
test-gtk:
	$(MAKE) -C gui/gtk test

# Build the macOS frontend (gui/macos: a Swift package; needs the Command
# Line Tools, no Xcode). Part of `all` on macOS. Output: gui/macos/build/Lace.app
macos: liblace laced
	$(MAKE) -C gui/macos

# macOS frontend checks: LaceKit against a private stdio daemon and SQLite
test-macos: liblace laced
	$(MAKE) -C gui/macos test

# The real app driven by a script (gui/macos/Tests/smoke.sh); a window shows briefly
smoke-macos: liblace laced
	$(MAKE) -C gui/macos smoke

# A shippable bundle and DMG (gui/macos/build/dist, Lace-<version>.dmg):
# release build, laced's libraries bundled, signed (CODESIGN_IDENTITY)
dist-macos:
	$(MAKE) -C gui/macos dist

# Unit tests (tests/): liblace, the TUI's SQL scanning and builders, then the
# notcurses frontend's. `make clean && make SANITIZE=1 test` runs them under
# ASan/UBSan.
test: liblace ncurses notcurses
	$(MAKE) -C liblace test
	$(MAKE) -C tui/ncurses test
	$(MAKE) -C tui/notcurses test

# Integration tests against real databases in containers (tests/integration):
# `make test-integration IT_ARGS="--only postgres -- keyset"`
# TUI tests: the real binary driven in tmux (tests/tui), SQLite only
test-tui: all
	python3 tests/tui/run.py $(TUI_ARGS)

# Windows build of liblace: unit tests and a TCP smoke test under Wine, in
# a container (tests/windows/run.sh)
test-windows:
	sh tests/windows/run.sh

# libFuzzer over the SQL lexer and quoting, connection strings, wire values
# and the TUI's query scanners, with ASan/UBSan, in a container
# (tests/fuzz/run.sh). FUZZ_SECONDS per target; FUZZ_TARGETS to pick some.
FUZZ_SECONDS ?= 60
fuzz:
	sh tests/fuzz/run.sh $(FUZZ_SECONDS) $(FUZZ_TARGETS)

# liblace from many threads while the daemon is killed and replaced, under
# ThreadSanitizer (tests/stress/run.sh; builds its own copy of the tree)
STRESS_SECONDS ?= 30
stress:
	sh tests/stress/run.sh $(STRESS_SECONDS) $(STRESS_SCENARIOS)

test-integration: liblace laced ncurses
	$(MAKE) -C tui/ncurses integration
	python3 tests/integration/run.py $(IT_ARGS)

# Clean all build artifacts
clean:
	$(MAKE) -C laced clean
	$(MAKE) -C liblace clean
	$(MAKE) -C tui/ncurses clean
	-$(MAKE) -C tui/notcurses clean 2>/dev/null || true
	-$(MAKE) -C gui/gtk clean 2>/dev/null || true
	-$(MAKE) -C gui/macos clean 2>/dev/null || true

# Full clean
distclean: clean
	rm -rf build

# Install (requires PREFIX or DESTDIR)
PREFIX ?= /usr/local
DESTDIR ?=

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 laced/build/laced $(DESTDIR)$(PREFIX)/bin/
	install -m 755 tui/ncurses/build/lace $(DESTDIR)$(PREFIX)/bin/
	install -m 755 tui/notcurses/build/lace-nc $(DESTDIR)$(PREFIX)/bin/
	$(MAKE) -C liblace install PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)

# Install the GTK frontend too (binary, desktop entry, icon)
install-gtk: install gtk
	$(MAKE) -C gui/gtk install PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)

# The notcurses frontend is part of `install`; kept for scripts that call it
install-notcurses: install

# Run the notcurses frontend
run-notcurses: notcurses
	./tui/notcurses/build/lace-nc

# Run the ncurses frontend
run: all
	./tui/ncurses/build/lace

# Run the GTK frontend
run-gtk: gtk
	./gui/gtk/build/lace-gtk

# Run the macOS frontend
run-macos: macos
	./gui/macos/build/Lace.app/Contents/MacOS/Lace

# Help
help:
	@echo "Lace - Database Viewer and Manager"
	@echo ""
	@echo "Targets:"
	@echo "  all       Build the daemon, liblace and both TUIs (default)"
	@echo "  laced     Build the daemon"
	@echo "  liblace   Build the client library"
	@echo "  ncurses   Build the ncurses frontend"
	@echo "  notcurses Build the notcurses frontend (tui/notcurses/build/lace-nc)"
	@echo "  test-notcurses  notcurses unit tests and terminal tests (needs tmux)"
	@echo "  run-notcurses  Build and run the notcurses frontend"
	@echo "  gtk       Build the GTK4 frontend (gui/gtk/build/lace-gtk)"
	@echo "  test-gtk  GTK frontend unit tests"
	@echo "  macos     Build the macOS frontend (gui/macos/build/Lace.app)"
	@echo "  test-macos  macOS frontend checks (LaceKit)"
	@echo "  smoke-macos  Drive the macOS app with a script against a temporary SQLite file"
	@echo "  dist-macos  Shippable Lace.app and DMG (gui/macos/build/dist)"
	@echo "  install-gtk  Install everything plus the GTK frontend"
	@echo "  run-gtk   Build and run the GTK frontend"
	@echo "  test      Build and run the unit tests (liblace, both TUIs)"
	@echo "  test-tui  TUI tests in a real terminal (needs tmux)"
	@echo "  test-integration  Integration tests on real databases (podman or docker)"
	@echo "  test-windows  liblace built for Windows, tested under Wine (podman or docker)"
	@echo "  fuzz      libFuzzer on the parsers, FUZZ_SECONDS each (podman or docker)"
	@echo "  stress    liblace threads vs. daemon kills, under ThreadSanitizer"
	@echo "  clean     Remove build artifacts"
	@echo "  distclean Remove all artifacts"
	@echo "  install   Install to PREFIX (default: /usr/local)"
	@echo "  run       Build and run the ncurses frontend"
	@echo ""
	@echo "Variables:"
	@echo "  PREFIX    Installation prefix (default: /usr/local)"
	@echo "  DESTDIR   Staging directory for packaging"
