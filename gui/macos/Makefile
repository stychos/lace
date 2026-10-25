# Lace macOS frontend
# Makefile
#
# Builds the Swift package (Package.swift: LaceKit over liblace, the Lace app,
# the LaceKitChecks test program) and assembles build/Lace.app with laced
# inside it, next to the executable, where liblace looks for the daemon.
# Needs the Command Line Tools (swift) and cJSON (brew install cjson); no
# Xcode. liblace.a is linked by path from Package.swift, never with -llace.
# Saved connections, recent connections, query history and the private
# file writer are the TUI's own sources, compiled here into
# build/shared/liblaceshared.a (as the GTK Makefile does), so all three
# frontends read and write the same connections.json, recent.json and
# history/<id>.json. Resources/Lace.icns comes from the GTK icon
# (scripts/make-icon.sh) and is committed.

SWIFT = swift
# debug | release (swift build -c)
CONFIG ?= debug
BUILD_DIR = build
APP = $(BUILD_DIR)/Lace.app
SWIFT_OUT = .build/$(CONFIG)
LIBLACE = ../../liblace/build/liblace.a
LACED = ../../laced/build/laced
VERSION = 0.1.0
BUNDLE_ID = dev.hexed.lace
# "-" signs ad hoc; set a Developer ID identity to distribute
CODESIGN_IDENTITY ?= -

# The Command Line Tools' swift cannot start the default (SwiftBuild) build
# system without Xcode; the native one works in both setups
SWIFT_FLAGS = --build-system native -c $(CONFIG) $(SWIFT_EXTRA_FLAGS)

# cJSON for liblace: Homebrew's static archive when there is one, so the app
# does not depend on /opt/homebrew at run time (Package.swift reads
# LACE_CJSON_STATIC), else the shared library from its prefix
CJSON_LIBDIR := $(shell pkg-config --variable=libdir libcjson 2>/dev/null)
ifneq ($(wildcard $(CJSON_LIBDIR)/libcjson.a),)
    export LACE_CJSON_STATIC = $(CJSON_LIBDIR)/libcjson.a
else ifneq ($(CJSON_LIBDIR),)
    SWIFT_FLAGS += -Xlinker -L$(CJSON_LIBDIR)
endif

# The shared TUI sources (see the header comment). Package.swift links the
# archive by path. MACOS_MIN is the deployment target (Package.swift: 14).
CC = clang
MACOS_MIN = 14.0
TUI_SRC = ../../tui/ncurses/src
SHARED_DIR = $(BUILD_DIR)/shared
SHARED_LIB = $(SHARED_DIR)/liblaceshared.a
SHARED_OBJS = $(SHARED_DIR)/connections.o $(SHARED_DIR)/recent.o $(SHARED_DIR)/history.o \
  $(SHARED_DIR)/platform_posix.o $(SHARED_DIR)/catalog_rows.o $(SHARED_DIR)/utf8.o
SHARED_CFLAGS = -std=c11 -Wall -Wextra -O2 -g -mmacosx-version-min=$(MACOS_MIN) \
  -I../../liblace/include -I$(TUI_SRC) $(shell pkg-config --cflags libcjson 2>/dev/null) $(EXTRA_CFLAGS)

all: $(APP)

$(SHARED_DIR)/connections.o: $(TUI_SRC)/config/connections.c
	@mkdir -p $(SHARED_DIR)
	$(CC) $(SHARED_CFLAGS) -MMD -MP -c $< -o $@
$(SHARED_DIR)/recent.o: $(TUI_SRC)/config/recent.c
	@mkdir -p $(SHARED_DIR)
	$(CC) $(SHARED_CFLAGS) -MMD -MP -c $< -o $@
$(SHARED_DIR)/history.o: $(TUI_SRC)/core/history.c
	@mkdir -p $(SHARED_DIR)
	$(CC) $(SHARED_CFLAGS) -MMD -MP -c $< -o $@
$(SHARED_DIR)/platform_posix.o: $(TUI_SRC)/platform/posix/platform_posix.c
	@mkdir -p $(SHARED_DIR)
	$(CC) $(SHARED_CFLAGS) -MMD -MP -c $< -o $@
$(SHARED_DIR)/catalog_rows.o: $(TUI_SRC)/core/catalog_rows.c
	@mkdir -p $(SHARED_DIR)
	$(CC) $(SHARED_CFLAGS) -MMD -MP -c $< -o $@
$(SHARED_DIR)/utf8.o: $(TUI_SRC)/core/utf8.c
	@mkdir -p $(SHARED_DIR)
	$(CC) $(SHARED_CFLAGS) -MMD -MP -c $< -o $@

$(SHARED_LIB): $(SHARED_OBJS)
	ar rcs $@ $^

-include $(SHARED_OBJS:.o=.d)

shared: $(SHARED_LIB)

# liblace and laced build through their own Makefiles (no-ops when up to date)
$(LIBLACE): FORCE
	$(MAKE) -C ../../liblace

$(LACED): FORCE
	$(MAKE) -C ../../laced

FORCE:

# The package. A dev run of .build/<config>/Lace finds the daemon through
# the symlink, like the GTK frontend's build/laced.
swift: $(LIBLACE) $(LACED) $(SHARED_LIB)
	$(SWIFT) build $(SWIFT_FLAGS)
	ln -sfn $(abspath $(LACED)) $(SWIFT_OUT)/laced

$(APP): swift Resources/Info.plist Resources/Lace.icns
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	cp $(SWIFT_OUT)/Lace $(APP)/Contents/MacOS/Lace
	cp $(LACED) $(APP)/Contents/MacOS/laced
	cp Resources/Lace.icns $(APP)/Contents/Resources/Lace.icns
	sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@BUNDLE_ID@/$(BUNDLE_ID)/g' \
	  Resources/Info.plist > $(APP)/Contents/Info.plist
	printf 'APPL????' > $(APP)/Contents/PkgInfo
	codesign --force --sign "$(CODESIGN_IDENTITY)" $(APP)/Contents/MacOS/laced
	codesign --force --sign "$(CODESIGN_IDENTITY)" $(APP)

# LaceKit against a private stdio daemon and a temporary SQLite database
# (Tests/LaceKitChecks; a plain executable: the Command Line Tools ship no
# XCTest). LACE_TEST_LACED names the daemon it starts.
test: $(LIBLACE) $(LACED) $(SHARED_LIB)
	$(SWIFT) build $(SWIFT_FLAGS) --product LaceKitChecks
	LACE_TEST_LACED=$(abspath $(LACED)) $(SWIFT_OUT)/LaceKitChecks

# The real app driven by LACE_MACOS_SCRIPT (Tests/smoke.sh): a private HOME
# and stdio daemon, a temporary SQLite file; a window shows briefly
smoke: swift
	sh Tests/smoke.sh $(SWIFT_OUT)/Lace $(abspath $(LACED))

run: all
	$(APP)/Contents/MacOS/Lace

# Shipping: build/dist/Lace.app and build/Lace-$(VERSION).dmg. liblace and
# laced are rebuilt for MACOS_MIN (the deployment target the package has),
# the app in release configuration, laced's Homebrew libraries bundled into
# Contents/Frameworks (scripts/bundle-libs.sh), then everything is signed
# from the inside out with CODESIGN_IDENTITY: ad hoc by default; a
# Developer ID identity gets the hardened runtime and a timestamp, which
# notarization needs (`make notarize NOTARY_PROFILE=<notarytool profile>`).
DIST = $(BUILD_DIR)/dist
DMG = $(BUILD_DIR)/Lace-$(VERSION).dmg
ifeq ($(CODESIGN_IDENTITY),-)
    SIGN_OPTIONS =
else
    SIGN_OPTIONS = --options runtime --timestamp
endif

dist:
	$(MAKE) -C ../../liblace clean
	MACOSX_DEPLOYMENT_TARGET=$(MACOS_MIN) $(MAKE) -C ../../liblace
	$(MAKE) -C ../../laced clean
	MACOSX_DEPLOYMENT_TARGET=$(MACOS_MIN) $(MAKE) -C ../../laced
	$(MAKE) CONFIG=release all
	rm -rf $(DIST)
	mkdir -p $(DIST)
	cp -R $(APP) $(DIST)/Lace.app
	sh scripts/bundle-libs.sh $(DIST)/Lace.app
	for f in $(DIST)/Lace.app/Contents/Frameworks/*.dylib; do \
	  codesign --force --sign "$(CODESIGN_IDENTITY)" $(SIGN_OPTIONS) "$$f"; done
	codesign --force --sign "$(CODESIGN_IDENTITY)" $(SIGN_OPTIONS) $(DIST)/Lace.app/Contents/MacOS/laced
	codesign --force --sign "$(CODESIGN_IDENTITY)" $(SIGN_OPTIONS) $(DIST)/Lace.app/Contents/MacOS/Lace
	codesign --force --sign "$(CODESIGN_IDENTITY)" $(SIGN_OPTIONS) $(DIST)/Lace.app
	codesign --verify --deep --strict $(DIST)/Lace.app
	ln -sfn /Applications $(DIST)/Applications
	rm -f $(DMG)
	hdiutil create -quiet -volname Lace -srcfolder $(DIST) -ov -format UDZO $(DMG)
	@echo "dist: $(DIST)/Lace.app and $(DMG)"

# Notarize the DMG with credentials stored by
# `xcrun notarytool store-credentials <profile>`; staples the ticket
notarize:
	xcrun notarytool submit $(DMG) --keychain-profile "$(NOTARY_PROFILE)" --wait
	xcrun stapler staple $(DMG)

clean:
	rm -rf $(BUILD_DIR) .build

check-deps:
	@$(SWIFT) --version 2>/dev/null | head -1 || echo "  swift: MISSING (xcode-select --install)"
	@pkg-config --exists libcjson && echo "  libcjson: OK" || echo "  libcjson: MISSING (brew install cjson)"

.PHONY: all shared swift test smoke run dist notarize clean check-deps FORCE
