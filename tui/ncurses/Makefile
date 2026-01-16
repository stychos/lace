# Lace TUI ncurses frontend
# Makefile

CC = clang
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -g
CFLAGS += -I. -I../../liblace/include -I/usr/include/cjson
LDFLAGS = -L../../liblace/build
LIBS = -llace -lncursesw -lmenuw -lcjson

# Build directory
BUILD_DIR = build

# Source directories
SRC_DIRS = . app async config core platform/posix util viewmodel views

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
