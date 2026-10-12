# liblace - Lace Client Library
# Makefile

CC ?= clang
ifeq ($(origin CC),default)
    CC = clang
endif
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -Iinclude -g
LDFLAGS =
LIBS = -lcjson
AR = ar
ARFLAGS = rcs

# Windows (MSYS2 / MinGW-w64; also cross builds: make OS=Windows_NT
# CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar). The OS layer is
# src/os/os_win32.c; winpthreads provides pthreads.
ifeq ($(OS),Windows_NT)
    WINDOWS = 1
    UNAME_S = Windows
    LIBS += -lws2_32 -lpthread
    # Programs carry winpthreads and libgcc instead of needing their DLLs
    LDFLAGS += -static
else
    UNAME_S := $(shell uname -s)
    # Only what the public headers declare (LACE_BEGIN_DECLS) is exported
    CFLAGS += -fPIC -fvisibility=hidden
endif

# cJSON: CJSON_CFLAGS / CJSON_LDFLAGS override the detection
ifdef CJSON_CFLAGS
    CFLAGS += $(CJSON_CFLAGS)
else ifeq ($(UNAME_S),Darwin)
    # macOS - use Homebrew paths
    CFLAGS += $(shell pkg-config --cflags libcjson 2>/dev/null || echo "-I/opt/homebrew/opt/cjson/include")
else ifneq ($(WINDOWS),1)
    CFLAGS += -I/usr/include/cjson
endif
ifdef CJSON_LDFLAGS
    LDFLAGS += $(CJSON_LDFLAGS)
else ifeq ($(UNAME_S),Darwin)
    LDFLAGS += $(shell pkg-config --libs-only-L libcjson 2>/dev/null || echo "-L/opt/homebrew/opt/cjson/lib")
endif

# Extra compiler flags, e.g. `make EXTRA_CFLAGS=-Werror` (CI)
CFLAGS += $(EXTRA_CFLAGS)

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

# Build directory
BUILD_DIR = build

# Source files (including subdirectories)
SRCS = $(wildcard src/*.c) $(wildcard src/util/*.c) $(wildcard src/config/*.c) $(wildcard src/os/*.c)
OBJS = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

# Library version: bump MAJOR on any incompatible change to the public
# headers (it is the SONAME / install name), MINOR for additions
VERSION_MAJOR = 0
VERSION_MINOR = 1
VERSION_PATCH = 0
VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)

# Output
TARGET_STATIC = $(BUILD_DIR)/liblace.a
ifeq ($(WINDOWS),1)
    SHARED_REAL = lace.dll
    SHARED_LINKS =
    SHARED_FLAGS = -shared -Wl,--out-implib,$(BUILD_DIR)/liblace.dll.a
else ifeq ($(UNAME_S),Darwin)
    SHARED_REAL = liblace.$(VERSION_MAJOR).dylib
    SHARED_LINKS = liblace.dylib
    SHARED_FLAGS = -dynamiclib -install_name @rpath/$(SHARED_REAL) \
        -compatibility_version $(VERSION_MAJOR).$(VERSION_MINOR) \
        -current_version $(VERSION)
else
    SHARED_REAL = liblace.so.$(VERSION)
    SHARED_LINKS = liblace.so.$(VERSION_MAJOR) liblace.so
    SHARED_FLAGS = -shared -Wl,-soname,liblace.so.$(VERSION_MAJOR)
endif
TARGET_SHARED = $(BUILD_DIR)/$(SHARED_REAL)

# Default: build static library
all: $(TARGET_STATIC)

# Build both static and shared
both: $(TARGET_STATIC) $(TARGET_SHARED)

# Static library
$(TARGET_STATIC): $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(AR) $(ARFLAGS) $@ $^

# Shared library
$(TARGET_SHARED): $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(SHARED_FLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
	cd $(BUILD_DIR) && for l in $(SHARED_LINKS); do ln -sf $(SHARED_REAL) $$l; done

# Compile source files (handles subdirectories)
$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

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

# Install headers and library
PREFIX ?= /usr/local
DESTDIR ?=

# Headers go to include/lace/ with their subdirectories (lace.h includes
# "types.h", "util/str.h", ...), the same layout the top-level install uses
install: $(TARGET_STATIC) $(TARGET_SHARED)
	install -d $(DESTDIR)$(PREFIX)/include/lace/util $(DESTDIR)$(PREFIX)/include/lace/config
	install -m 644 include/*.h $(DESTDIR)$(PREFIX)/include/lace/
	install -m 644 include/util/*.h $(DESTDIR)$(PREFIX)/include/lace/util/
	install -m 644 include/config/*.h $(DESTDIR)$(PREFIX)/include/lace/config/
	install -d $(DESTDIR)$(PREFIX)/lib/
	install -m 644 $(TARGET_STATIC) $(DESTDIR)$(PREFIX)/lib/
	install -m 755 $(TARGET_SHARED) $(DESTDIR)$(PREFIX)/lib/
	cd $(DESTDIR)$(PREFIX)/lib && for l in $(SHARED_LINKS); do ln -sf $(SHARED_REAL) $$l; done

.PHONY: all both clean debug release install

# Unit tests (../tests): built with the same flags, linked to the static lib
TEST_BIN = $(BUILD_DIR)/test_liblace

$(TEST_BIN): ../tests/test_liblace.c ../tests/test.h $(TARGET_STATIC)
	$(CC) $(CFLAGS) -I../tests $< $(TARGET_STATIC) -o $@ $(LDFLAGS) $(LIBS) -lm

test: $(TEST_BIN)
	./$(TEST_BIN)

.PHONY: test
