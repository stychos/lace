# laced - Lace Database Daemon
# Makefile

CC = clang
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -g
LDFLAGS = -L../liblace/build

# Source directories
SRC_DIR = src
DB_SRC_DIR = $(SRC_DIR)/db
UTIL_SRC_DIR = $(SRC_DIR)/util

# Include paths
INCLUDES = -I$(SRC_DIR) -I../liblace/include
INCLUDES += $(shell pkg-config --cflags libcjson 2>/dev/null || echo "-I/opt/homebrew/opt/cjson/include")

# cJSON library path (needed for linking)
LDFLAGS += $(shell pkg-config --libs-only-L libcjson 2>/dev/null || echo "-L/opt/homebrew/opt/cjson/lib")

# Build directory
BUILD_DIR = build

# Libraries (liblace now provides str, mem, connstr utilities)
LIBS = -llace -lcjson -lsqlite3 -lpq -lmariadb -lpthread

# Output
TARGET = $(BUILD_DIR)/laced

# Daemon source files
DAEMON_SRCS = \
	$(SRC_DIR)/main.c \
	$(SRC_DIR)/server.c \
	$(SRC_DIR)/session.c \
	$(SRC_DIR)/handler.c \
	$(SRC_DIR)/json.c \
	$(SRC_DIR)/async.c \
	$(SRC_DIR)/socket.c \
	$(SRC_DIR)/log.c \
	$(SRC_DIR)/crash.c

# Database source files
# Note: connstr.c is now in liblace
DB_SRCS = \
	$(DB_SRC_DIR)/db_manager.c \
	$(DB_SRC_DIR)/db_types.c \
	$(DB_SRC_DIR)/db_common.c \
	$(DB_SRC_DIR)/sqlite/sqlite_driver.c \
	$(DB_SRC_DIR)/postgres/pg_driver.c \
	$(DB_SRC_DIR)/mysql/mysql_driver.c

# Utility source files - now in liblace, not compiled here
# UTIL_SRCS = (removed - str.c, mem.c now in liblace)

# All source files
SRCS = $(DAEMON_SRCS) $(DB_SRCS)

# Object files
OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(notdir $(SRCS)))
DEPS = $(OBJS:.o=.d)

# Database client libraries. pkg-config is consulted on every platform; the
# Homebrew paths are fallbacks for macOS installs without .pc files.
# mariadb's .pc points at the directory that holds mysql.h itself
# (Homebrew: .../include/mysql, Debian: /usr/include/mariadb), so the driver
# includes <mysql.h>, not <mysql/mysql.h>.
INCLUDES += $(shell pkg-config --cflags libpq 2>/dev/null || echo "-I/opt/homebrew/opt/libpq/include")
LDFLAGS  += $(shell pkg-config --libs-only-L libpq 2>/dev/null || echo "-L/opt/homebrew/opt/libpq/lib")
INCLUDES += $(shell pkg-config --cflags mariadb 2>/dev/null || echo "-I/opt/homebrew/opt/mariadb/include/mysql")
LDFLAGS  += $(shell pkg-config --libs-only-L mariadb 2>/dev/null || echo "-L/opt/homebrew/opt/mariadb/lib")

# make SANITIZE=1 builds with AddressSanitizer + UndefinedBehaviorSanitizer.
# Run `make clean` first so sanitized and plain objects do not mix.
ifeq ($(SANITIZE),1)
    CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
    LDFLAGS += -fsanitize=address,undefined
endif

# Default target
all: ../liblace/build/liblace.a $(TARGET)

# Build liblace first
../liblace/build/liblace.a:
	$(MAKE) -C ../liblace

# Link
$(TARGET): $(OBJS) ../liblace/build/liblace.a
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

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

$(BUILD_DIR)/async.o: $(SRC_DIR)/async.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/socket.o: $(SRC_DIR)/socket.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/log.o: $(SRC_DIR)/log.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/crash.o: $(SRC_DIR)/crash.c
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

# connstr.c is now in liblace - removed from here

$(BUILD_DIR)/sqlite_driver.o: $(DB_SRC_DIR)/sqlite/sqlite_driver.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/pg_driver.o: $(DB_SRC_DIR)/postgres/pg_driver.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD_DIR)/mysql_driver.o: $(DB_SRC_DIR)/mysql/mysql_driver.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# Utility sources (str.c, mem.c) are now in liblace - removed from here

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
