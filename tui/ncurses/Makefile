# Lace ncurses frontend Makefile

# Compiler and flags
CC = clang
CFLAGS = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L
CFLAGS += -I../../liblace/include
CFLAGS += -Isrc

# ncurses flags
NCURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null || echo "-I/usr/include/ncursesw")
NCURSES_LIBS := $(shell pkg-config --libs ncursesw 2>/dev/null || echo "-lncursesw")
CFLAGS += $(NCURSES_CFLAGS)

# liblace
LIBLACE = ../../liblace/build/liblace.a
LIBLACE_DEPS = $(wildcard ../../liblace/include/*.h)

# cJSON (needed by liblace)
CJSON_LIBS := $(shell pkg-config --libs libcjson 2>/dev/null || echo "-lcjson")

# Directories
SRC_DIR = src
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj

# Sources and objects
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

# Target
TARGET = $(BUILD_DIR)/lace

# Build type
DEBUG ?= 0
ifeq ($(DEBUG), 1)
    CFLAGS += -g -O0 -DDEBUG
else
    CFLAGS += -O2 -DNDEBUG
endif

# Default target
.PHONY: all
all: $(TARGET)

# Link
$(TARGET): $(OBJS) $(LIBLACE) | $(BUILD_DIR)
	$(CC) $(OBJS) $(LIBLACE) $(NCURSES_LIBS) $(CJSON_LIBS) -o $@
	@echo "Built: $@"

# Compile
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Create directories
$(BUILD_DIR):
	mkdir -p $@

$(OBJ_DIR):
	mkdir -p $@

# Build liblace if needed
$(LIBLACE): $(LIBLACE_DEPS)
	$(MAKE) -C ../../liblace

# Clean
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

# Full clean (including liblace)
.PHONY: distclean
distclean: clean
	$(MAKE) -C ../../liblace clean

# Debug build
.PHONY: debug
debug:
	$(MAKE) DEBUG=1

# Run
.PHONY: run
run: $(TARGET)
	./$(TARGET)

# Header files
HEADERS = $(wildcard $(SRC_DIR)/*.h)

# Dependencies on headers
$(OBJ_DIR)/main.o: $(SRC_DIR)/main.c $(HEADERS)
$(OBJ_DIR)/app.o: $(SRC_DIR)/app.c $(HEADERS)
$(OBJ_DIR)/tui.o: $(SRC_DIR)/tui.c $(HEADERS)
$(OBJ_DIR)/edit.o: $(SRC_DIR)/edit.c $(HEADERS)
$(OBJ_DIR)/dialogs.o: $(SRC_DIR)/dialogs.c $(HEADERS)
$(OBJ_DIR)/filters.o: $(SRC_DIR)/filters.c $(HEADERS)
$(OBJ_DIR)/sidebar.o: $(SRC_DIR)/sidebar.c $(HEADERS)
$(OBJ_DIR)/navigation.o: $(SRC_DIR)/navigation.c $(HEADERS)
$(OBJ_DIR)/query.o: $(SRC_DIR)/query.c $(HEADERS)
$(OBJ_DIR)/config.o: $(SRC_DIR)/config.c $(HEADERS)
$(OBJ_DIR)/session.o: $(SRC_DIR)/session.c $(HEADERS)
$(OBJ_DIR)/connect.o: $(SRC_DIR)/connect.c $(HEADERS)

.PHONY: help
help:
	@echo "Lace ncurses frontend"
	@echo ""
	@echo "Targets:"
	@echo "  all        Build the frontend (default)"
	@echo "  clean      Remove build artifacts"
	@echo "  distclean  Remove all artifacts including liblace"
	@echo "  debug      Build with debug symbols"
	@echo "  run        Build and run"
	@echo ""
	@echo "Variables:"
	@echo "  DEBUG=1    Enable debug build"
