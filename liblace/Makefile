# liblace - Lace Client Library
# Makefile

CC = clang
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -Iinclude -fPIC -g
LDFLAGS =
LIBS = -lcjson
AR = ar
ARFLAGS = rcs

# Build directory
BUILD_DIR = build

# Source files
SRCS = $(wildcard src/*.c)
OBJS = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

# Output
TARGET_STATIC = $(BUILD_DIR)/liblace.a
TARGET_SHARED = $(BUILD_DIR)/liblace.so

# Detect OS for shared library extension
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    TARGET_SHARED = $(BUILD_DIR)/liblace.dylib
    SHARED_FLAGS = -dynamiclib
else
    SHARED_FLAGS = -shared
endif

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

# Compile source files
$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
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
install: $(TARGET_STATIC)
	install -d $(DESTDIR)$(PREFIX)/include/
	install -m 644 include/*.h $(DESTDIR)$(PREFIX)/include/
	install -d $(DESTDIR)$(PREFIX)/lib/
	install -m 644 $(TARGET_STATIC) $(DESTDIR)$(PREFIX)/lib/

.PHONY: all both clean debug release install
