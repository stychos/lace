# Lace GTK4 frontend
# Makefile
#
# Builds build/lace-gtk against liblace.a (by path: -llace would pick the
# shared library once it is built). Saved connections, query history and the
# private file writer are the TUI's own sources, compiled in here, so both
# frontends read and write the same connections.json and history files.

CC = clang
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -g
CFLAGS += -Isrc -I../../liblace/include -I../../tui/ncurses/src
LDFLAGS = -L../../liblace/build
LIBS = ../../liblace/build/liblace.a -lm -pthread

PKGS = gtk4 libadwaita-1 gtksourceview-5 libcjson
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDFLAGS += $(shell pkg-config --libs-only-L $(PKGS))
LIBS += $(shell pkg-config --libs-only-l $(PKGS))

# Extra compiler flags, e.g. `make EXTRA_CFLAGS=-Werror` (CI)
CFLAGS += $(EXTRA_CFLAGS)

# make SANITIZE=1: AddressSanitizer + UndefinedBehaviorSanitizer (clean first)
ifeq ($(SANITIZE),1)
    CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
    LDFLAGS += -fsanitize=address,undefined
endif

BUILD_DIR = build
SRC_DIR = src
RES_DIR = resources
TUI_SRC = ../../tui/ncurses/src

SRC_SUBDIRS = . sql conn model views
SRC_DIRS = $(addprefix $(SRC_DIR)/,$(SRC_SUBDIRS))
SRCS := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))

# Shared with the TUI (see the header comment)
SHARED_SRCS = $(TUI_SRC)/config/connections.c $(TUI_SRC)/core/history.c \
              $(TUI_SRC)/platform/posix/platform_posix.c
SHARED_OBJS = $(BUILD_DIR)/shared/connections.o $(BUILD_DIR)/shared/history.o \
              $(BUILD_DIR)/shared/platform_posix.o

GRESOURCE_XML = $(RES_DIR)/lace.gresource.xml
GRESOURCE_C = $(BUILD_DIR)/resources.c
GRESOURCE_O = $(BUILD_DIR)/resources.o
RES_FILES := $(shell glib-compile-resources --sourcedir=$(RES_DIR) --generate-dependencies $(GRESOURCE_XML) 2>/dev/null)

DEPS := $(OBJS:.o=.d) $(SHARED_OBJS:.o=.d)

TARGET = $(BUILD_DIR)/lace-gtk

all: $(TARGET)

$(TARGET): $(OBJS) $(SHARED_OBJS) $(GRESOURCE_O) ../../liblace/build/liblace.a
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJS) $(SHARED_OBJS) $(GRESOURCE_O) -o $@ $(LDFLAGS) $(LIBS)
	@# liblace looks for laced next to the executable: point a dev build at it
	@if [ -x ../../laced/build/laced ] && [ ! -e $(BUILD_DIR)/laced ]; then \
	  ln -s ../../../laced/build/laced $(BUILD_DIR)/laced; fi

# liblace builds through its own Makefile every time (a no-op when up to date)
../../liblace/build/liblace.a: FORCE
	$(MAKE) -C ../../liblace

FORCE:

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/shared/connections.o: $(TUI_SRC)/config/connections.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/shared/history.o: $(TUI_SRC)/core/history.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/shared/platform_posix.o: $(TUI_SRC)/platform/posix/platform_posix.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(GRESOURCE_C): $(GRESOURCE_XML) $(RES_FILES)
	@mkdir -p $(BUILD_DIR)
	glib-compile-resources --target=$@ --sourcedir=$(RES_DIR) --generate-source $<

$(GRESOURCE_O): $(GRESOURCE_C)
	$(CC) $(CFLAGS) -Wno-unused-parameter -Wno-missing-field-initializers -c $< -o $@

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR)

debug: CFLAGS += -DDEBUG -O0
debug: clean all

release: CFLAGS += -O2 -DNDEBUG
release: clean all

run: all
	$(TARGET)

# Unit tests: the pure helpers (SQL builders, row keys, the row model), no
# display needed
TEST_SQL = $(BUILD_DIR)/test_sql
TEST_MODEL = $(BUILD_DIR)/test_model
PURE_OBJS = $(filter $(BUILD_DIR)/src/sql/%.o,$(OBJS))
MODEL_OBJS = $(filter $(BUILD_DIR)/src/model/%.o,$(OBJS))

$(TEST_SQL): tests/test_sql.c tests/test.h $(PURE_OBJS) ../../liblace/build/liblace.a
	$(CC) $(CFLAGS) -Itests $< $(PURE_OBJS) -o $@ $(LDFLAGS) $(LIBS)

$(TEST_MODEL): tests/test_model.c tests/test.h $(MODEL_OBJS) ../../liblace/build/liblace.a
	$(CC) $(CFLAGS) -Itests $< $(MODEL_OBJS) -o $@ $(LDFLAGS) $(LIBS)

test: $(TEST_SQL) $(TEST_MODEL)
	./$(TEST_SQL)
	./$(TEST_MODEL)

# Install the binary, the desktop entry and the icon (PREFIX, DESTDIR as
# the top-level Makefile)
PREFIX ?= /usr/local
DESTDIR ?=
install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/applications \
	  $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/lace-gtk
	install -m 644 $(RES_DIR)/io.github.stychos.lace.desktop $(DESTDIR)$(PREFIX)/share/applications/
	install -m 644 $(RES_DIR)/icons/scalable/apps/io.github.stychos.lace.svg \
	  $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/

check-deps:
	@for p in $(PKGS); do pkg-config --exists $$p && echo "  $$p: OK" || echo "  $$p: MISSING"; done
	@echo "Debian/Ubuntu: apt install libgtk-4-dev libadwaita-1-dev libgtksourceview-5-dev libcjson-dev"
	@echo "Arch:          pacman -S gtk4 libadwaita gtksourceview5 cjson"
	@echo "macOS:         brew install gtk4 libadwaita gtksourceview5 cjson"

.PHONY: all clean debug release run test install check-deps FORCE
