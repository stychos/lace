# Lace - Database Viewer and Manager
# Top-level Makefile

.PHONY: all laced liblace ncurses gtk test test-tui test-integration test-windows fuzz stress clean distclean install help

# Default target
all: laced liblace ncurses

# Build the daemon
laced: liblace
	$(MAKE) -C laced

# Build the client library
liblace:
	$(MAKE) -C liblace

# Build the ncurses frontend (depends on liblace)
ncurses: liblace
	$(MAKE) -C tui/ncurses

# Build the GTK4 frontend (depends on liblace)
gtk: liblace
	@if [ -d gui/gtk ]; then $(MAKE) -C gui/gtk; \
	else echo "GTK frontend not present (gui/gtk does not exist yet)"; exit 1; fi

# Unit tests (tests/): liblace, then the TUI's SQL scanning and builders.
# `make clean && make SANITIZE=1 test` runs them under ASan/UBSan.
test: liblace ncurses
	$(MAKE) -C liblace test
	$(MAKE) -C tui/ncurses test

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
	-$(MAKE) -C gui/gtk clean 2>/dev/null || true

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
	$(MAKE) -C liblace install PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)

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
	@echo "  gtk       Build the GTK4 frontend"
	@echo "  test      Build and run the unit tests"
	@echo "  test-tui  TUI tests in a real terminal (needs tmux)"
	@echo "  test-integration  Integration tests on real databases (docker)"
	@echo "  test-windows  liblace built for Windows, tested under Wine (docker)"
	@echo "  fuzz      libFuzzer on the parsers, FUZZ_SECONDS each (docker)"
	@echo "  stress    liblace threads vs. daemon kills, under ThreadSanitizer"
	@echo "  clean     Remove build artifacts"
	@echo "  distclean Remove all artifacts"
	@echo "  install   Install to PREFIX (default: /usr/local)"
	@echo "  run       Build and run the ncurses frontend"
	@echo ""
	@echo "Variables:"
	@echo "  PREFIX    Installation prefix (default: /usr/local)"
	@echo "  DESTDIR   Staging directory for packaging"
