# Lace TUI ncurses frontend
# Makefile

CC = clang
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -g
CFLAGS += -Isrc -I../../liblace/include
LDFLAGS = -L../../liblace/build
# The archive by path: -llace would pick liblace.dylib/.so once built
LIBS = ../../liblace/build/liblace.a -lncursesw -lmenuw -lcjson -pthread

# Detect OS for platform-specific paths
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS - use Homebrew paths
    CFLAGS += $(shell pkg-config --cflags libcjson 2>/dev/null || echo "-I/opt/homebrew/opt/cjson/include")
    LDFLAGS += $(shell pkg-config --libs-only-L libcjson 2>/dev/null || echo "-L/opt/homebrew/opt/cjson/lib")
    # The macOS SDK ships ncurses headers but no libncursesw, and Homebrew's
    # ncursesw.pc shim emits neither -I nor -L. Compile and link against the
    # same Homebrew ncurses so header and library versions match.
    NCURSES_PREFIX := $(shell brew --prefix ncurses 2>/dev/null || echo /opt/homebrew/opt/ncurses)
    CFLAGS += -I$(NCURSES_PREFIX)/include/ncursesw -I$(NCURSES_PREFIX)/include
    LDFLAGS += -L$(NCURSES_PREFIX)/lib
else
    CFLAGS += $(shell pkg-config --cflags libcjson 2>/dev/null || echo "-I/usr/include/cjson")
    CFLAGS += $(shell pkg-config --cflags ncursesw 2>/dev/null)
    LDFLAGS += $(shell pkg-config --libs-only-L ncursesw 2>/dev/null)
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

# Build and source directories
BUILD_DIR = build
SRC_DIR = src

# Source directories relative to SRC_DIR
SRC_SUBDIRS = . app async config core platform/posix viewmodel views
SRC_DIRS = $(addprefix $(SRC_DIR)/,$(SRC_SUBDIRS))

# Find all source files
SRCS := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Output
TARGET = $(BUILD_DIR)/lace

# Default target
all: $(TARGET)

# Build the executable
$(TARGET): $(OBJS) ../../liblace/build/liblace.a
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

# liblace builds through its own Makefile every time (a no-op when up to
# date), so a change there relinks the TUI instead of using a stale archive
../../liblace/build/liblace.a: FORCE
	$(MAKE) -C ../../liblace

FORCE:

# Compile source files
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Include dependency files
-include $(DEPS)

# Clean
clean:
	rm -rf $(BUILD_DIR)

# Clean all (including liblace)
cleanall: clean
	$(MAKE) -C ../../liblace clean

# Debug build
debug: CFLAGS += -DDEBUG -O0
debug: clean all

# Release build
release: CFLAGS += -O2 -DNDEBUG
release: clean all

# Run
run: all
	$(TARGET)

.PHONY: all clean cleanall debug release run FORCE

# Unit tests (../../tests): every object but main, so tests call the real code
TEST_BIN = $(BUILD_DIR)/test_tui
TEST_OBJS = $(filter-out $(BUILD_DIR)/$(SRC_DIR)/app/main.o,$(OBJS))

$(TEST_BIN): ../../tests/test_tui.c ../../tests/test.h $(TEST_OBJS) ../../liblace/build/liblace.a
	$(CC) $(CFLAGS) -I../../tests $< $(TEST_OBJS) -o $@ $(LDFLAGS) $(LIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

.PHONY: test

# Integration tests (../../tests/integration): real databases, see run.py.
# Linked with the TUI's objects too, so tests can drive its SQL builders and
# database layer (paging, savepoints) as well as liblace.
IT_SRCS = $(wildcard ../../tests/integration/*.c)
IT_BIN = $(BUILD_DIR)/test_integration

$(IT_BIN): $(IT_SRCS) ../../tests/integration/it.h ../../tests/test.h $(TEST_OBJS) ../../liblace/build/liblace.a
	$(CC) $(CFLAGS) -I../../tests $(IT_SRCS) $(TEST_OBJS) -o $@ $(LDFLAGS) $(LIBS) -lm

integration: $(IT_BIN)

.PHONY: integration

