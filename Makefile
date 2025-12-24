# Lace - Database Viewer and Manager
# Built with clang

CC = clang
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -Isrc -g
LDFLAGS = -lncursesw -lpanel -lmenu -lsqlite3 -lmariadb -lpq -lpthread

# Build directory
BUILD_DIR = build

# Detect OS
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    # macOS - detect Homebrew prefix (ARM vs Intel)
    HOMEBREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)

    # Add Homebrew include paths
    CFLAGS += -I$(HOMEBREW_PREFIX)/include
    CFLAGS += -I$(HOMEBREW_PREFIX)/opt/mariadb/include
    CFLAGS += -I$(HOMEBREW_PREFIX)/opt/mariadb/include/mysql
    CFLAGS += -I$(HOMEBREW_PREFIX)/opt/libpq/include
    CFLAGS += -I$(HOMEBREW_PREFIX)/opt/sqlite/include
    CFLAGS += -I$(HOMEBREW_PREFIX)/opt/ncurses/include

    # Add Homebrew library paths
    LDFLAGS += -L$(HOMEBREW_PREFIX)/lib
    LDFLAGS += -L$(HOMEBREW_PREFIX)/opt/mariadb/lib
    LDFLAGS += -L$(HOMEBREW_PREFIX)/opt/libpq/lib
    LDFLAGS += -L$(HOMEBREW_PREFIX)/opt/sqlite/lib
    LDFLAGS += -L$(HOMEBREW_PREFIX)/opt/ncurses/lib
endif

# Source directories (common)
SRC_DIRS_COMMON = src/app src/core src/db src/db/sqlite src/db/postgres src/db/mysql \
                  src/tui/ncurses src/tui/ncurses/views src/util src/async src/viewmodel

# Platform-specific source directories
ifeq ($(UNAME_S),Windows_NT)
    SRC_DIRS_PLATFORM = src/platform/win32
else
    SRC_DIRS_PLATFORM = src/platform/posix
endif

SRC_DIRS = $(SRC_DIRS_COMMON) $(SRC_DIRS_PLATFORM)

# Find all C source files
SRCS = $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))

# Object files in build directory (strip src/ prefix, keep subdirs)
OBJS = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

TARGET = $(BUILD_DIR)/lace

# Default target
all: $(TARGET)

# Link the final executable
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# Compile C files with dependency generation
$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Include dependency files (only if they exist)
-include $(wildcard $(DEPS))

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)

# Run the application
run: $(TARGET)
	./$(TARGET)

# Debug build
debug: CFLAGS += -DDEBUG -O0
debug: clean all

# Release build
release: CFLAGS += -O2 -DNDEBUG
release: clean all

# Format code (if clang-format available)
format:
	find src -name "*.c" -o -name "*.h" | xargs clang-format -i

# Static analysis with clang analyzer
analyze:
	@mkdir -p $(BUILD_DIR)/analyze
	@for src in $(SRCS); do \
		echo "Analyzing $$src..."; \
		$(CC) --analyze $(CFLAGS) -o $(BUILD_DIR)/analyze/$$(basename $$src .c).plist $$src 2>&1; \
	done
	@echo "Analysis complete. Results in $(BUILD_DIR)/analyze/"

# Print variables for debugging
print-%:
	@echo $* = $($*)

.PHONY: all clean run debug release format analyze
