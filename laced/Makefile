# laced - Lace Database Daemon
# Makefile

CC = clang
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -g
LDFLAGS =

# Source directories
SRC_DIR = src
DB_SRC_DIR = $(SRC_DIR)/db
UTIL_SRC_DIR = $(SRC_DIR)/util

# Include paths
INCLUDES = -I$(SRC_DIR)

# Build directory
BUILD_DIR = build

# Libraries
LIBS = -lcjson -lsqlite3 -lpq -lmariadb

# Output
TARGET = $(BUILD_DIR)/laced

# Daemon source files
DAEMON_SRCS = \
	$(SRC_DIR)/main.c \
	$(SRC_DIR)/server.c \
	$(SRC_DIR)/session.c \
	$(SRC_DIR)/handler.c \
	$(SRC_DIR)/json.c

# Database source files
DB_SRCS = \
	$(DB_SRC_DIR)/db_manager.c \
	$(DB_SRC_DIR)/db_types.c \
	$(DB_SRC_DIR)/db_common.c \
	$(DB_SRC_DIR)/connstr.c \
	$(DB_SRC_DIR)/sqlite/sqlite_driver.c \
	$(DB_SRC_DIR)/postgres/pg_driver.c \
	$(DB_SRC_DIR)/mysql/mysql_driver.c

# Utility source files
UTIL_SRCS = \
	$(UTIL_SRC_DIR)/str.c \
	$(UTIL_SRC_DIR)/mem.c

# All source files
SRCS = $(DAEMON_SRCS) $(DB_SRCS) $(UTIL_SRCS)

# Object files
OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(notdir $(SRCS)))
DEPS = $(OBJS:.o=.d)

# Detect OS
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS - use Homebrew paths
    CFLAGS += $(shell pkg-config --cflags libpq 2>/dev/null || echo "-I/opt/homebrew/opt/libpq/include")
    LDFLAGS += $(shell pkg-config --libs libpq 2>/dev/null || echo "-L/opt/homebrew/opt/libpq/lib")
    CFLAGS += $(shell pkg-config --cflags mariadb 2>/dev/null || echo "-I/opt/homebrew/opt/mariadb/include/mysql")
    LDFLAGS += $(shell pkg-config --libs mariadb 2>/dev/null || echo "-L/opt/homebrew/opt/mariadb/lib")
endif

# Default target
all: $(TARGET)

# Link
$(TARGET): $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

# Compile daemon sources
$(BUILD_DIR)/main.o: $(SRC_DIR)/main.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/server.o: $(SRC_DIR)/server.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/session.o: $(SRC_DIR)/session.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/handler.o: $(SRC_DIR)/handler.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/json.o: $(SRC_DIR)/json.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# Compile database sources
$(BUILD_DIR)/db_manager.o: $(DB_SRC_DIR)/db_manager.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/db_types.o: $(DB_SRC_DIR)/db_types.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/db_common.o: $(DB_SRC_DIR)/db_common.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/connstr.o: $(DB_SRC_DIR)/connstr.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/sqlite_driver.o: $(DB_SRC_DIR)/sqlite/sqlite_driver.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/pg_driver.o: $(DB_SRC_DIR)/postgres/pg_driver.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/mysql_driver.o: $(DB_SRC_DIR)/mysql/mysql_driver.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# Compile utility sources
$(BUILD_DIR)/str.o: $(UTIL_SRC_DIR)/str.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/mem.o: $(UTIL_SRC_DIR)/mem.c
	@mkdir -p $(BUILD_DIR)
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
install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin/
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/

.PHONY: all clean debug release install
