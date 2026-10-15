# laced - Lace Database Daemon
# Makefile

CC = clang
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -g
LDFLAGS = -L../liblace/build

# Source directories
SRC_DIR = src
DB_SRC_DIR = $(SRC_DIR)/db

# Include paths
INCLUDES = -I$(SRC_DIR) -I../liblace/include
INCLUDES += $(shell pkg-config --cflags libcjson 2>/dev/null || echo "-I/opt/homebrew/opt/cjson/include")

# cJSON library path (needed for linking)
LDFLAGS += $(shell pkg-config --libs-only-L libcjson 2>/dev/null || echo "-L/opt/homebrew/opt/cjson/lib")

# Build directory
BUILD_DIR = build

# Libraries (liblace now provides str, mem, connstr utilities)
# The archive by path: -llace would pick liblace.dylib/.so once built
LIBS = ../liblace/build/liblace.a -lcjson -lsqlite3 -lpq -lmariadb -pthread

# Output
TARGET = $(BUILD_DIR)/laced

# Sources: everything under src/ (objects keep their directories in build/,
# so two files with the same name cannot collide)
SRCS := $(wildcard $(SRC_DIR)/*.c) $(wildcard $(DB_SRC_DIR)/*.c) \
        $(wildcard $(DB_SRC_DIR)/*/*.c)
OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Database client libraries. pkg-config is consulted on every platform; the
# Homebrew paths are fallbacks for macOS installs without .pc files.
# mariadb's .pc points at the directory that holds mysql.h itself
# (Homebrew: .../include/mysql, Debian: /usr/include/mariadb), so the driver
# includes <mysql.h>, not <mysql/mysql.h>.
INCLUDES += $(shell pkg-config --cflags libpq 2>/dev/null || echo "-I/opt/homebrew/opt/libpq/include")
LDFLAGS  += $(shell pkg-config --libs-only-L libpq 2>/dev/null || echo "-L/opt/homebrew/opt/libpq/lib")
INCLUDES += $(shell pkg-config --cflags mariadb 2>/dev/null || echo "-I/opt/homebrew/opt/mariadb/include/mysql")
LDFLAGS  += $(shell pkg-config --libs-only-L mariadb 2>/dev/null || echo "-L/opt/homebrew/opt/mariadb/lib")

# Extra compiler flags, e.g. `make EXTRA_CFLAGS=-Werror` (CI)
CFLAGS += $(EXTRA_CFLAGS)

# macOS: room in the Mach-O header for install_name_tool, so the app bundle
# can point laced at libraries copied next to it (gui/macos/scripts/bundle-libs.sh)
ifeq ($(shell uname -s),Darwin)
    LDFLAGS += -Wl,-headerpad_max_install_names
endif

# make SANITIZE=1 builds with AddressSanitizer + UndefinedBehaviorSanitizer.
# Run `make clean` first so sanitized and plain objects do not mix.
ifeq ($(SANITIZE),1)
    CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
    LDFLAGS += -fsanitize=address,undefined
endif
# make SANITIZE=thread builds with ThreadSanitizer (tests/stress uses it)
ifeq ($(SANITIZE),thread)
    CFLAGS += -fsanitize=thread -fno-omit-frame-pointer -O1
    LDFLAGS += -fsanitize=thread
endif

# Default target
all: $(TARGET)

# liblace builds through its own Makefile every time (a no-op when it is up
# to date), so a change there relinks the daemon instead of using a stale
# archive
../liblace/build/liblace.a: FORCE
	$(MAKE) -C ../liblace

FORCE:

# Link
$(TARGET): $(OBJS) ../liblace/build/liblace.a
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

# Compile
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# Include dependency files
-include $(DEPS)

# Clean
clean:
	rm -rf $(BUILD_DIR)

# Debug build
debug: CFLAGS += -DDEBUG -O0
debug: clean all

# Release build
release: CFLAGS += -O2 -DNDEBUG
release: clean all

# Install
PREFIX ?= /usr/local
DESTDIR ?=

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin/
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/

.PHONY: all clean debug release install FORCE
