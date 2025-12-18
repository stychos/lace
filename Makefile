# Lace - Database Viewer and Manager
# Built with clang

CC = clang
CFLAGS = -Wall -Wextra -std=c11 -Isrc -g
LDFLAGS = -lncursesw -lssl -lcrypto -lpanel -lform -lmenu -lsqlite3 -lmariadb -lpq

# Source directories
SRC_DIRS = src src/db src/db/sqlite src/db/postgres src/db/mysql \
           src/tui src/tui/views src/tui/widgets src/net src/util

# Find all C source files
SRCS = $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

TARGET = lace

# Default target
all: $(TARGET)

# Link the final executable
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# Compile C files with dependency generation
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Include dependency files
-include $(DEPS)

# Clean build artifacts
clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

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

# Print variables for debugging
print-%:
	@echo $* = $($*)

.PHONY: all clean run debug release format
