# Lace TUI ncurses frontend
# Makefile

CC = clang
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -g
CFLAGS += -Isrc -I../../liblace/include
LDFLAGS = -L../../liblace/build
LIBS = -llace -lncursesw -lmenuw -lcjson

# Detect OS for platform-specific paths
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS - use Homebrew paths
    CFLAGS += $(shell pkg-config --cflags libcjson 2>/dev/null || echo "-I/opt/homebrew/opt/cjson/include")
    LDFLAGS += $(shell pkg-config --libs-only-L libcjson 2>/dev/null || echo "-L/opt/homebrew/opt/cjson/lib")
    CFLAGS += $(shell pkg-config --cflags ncursesw 2>/dev/null || echo "-I/opt/homebrew/opt/ncurses/include")
    LDFLAGS += $(shell pkg-config --libs-only-L ncursesw 2>/dev/null || echo "-L/opt/homebrew/opt/ncurses/lib")
else
    CFLAGS += -I/usr/include/cjson
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

# Build liblace first
../../liblace/build/liblace.a:
	$(MAKE) -C ../../liblace

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

.PHONY: all clean cleanall debug release run
