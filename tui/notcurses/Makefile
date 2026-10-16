# Lace notcurses frontend
# Builds build/lace-nc against liblace.a (by path) and notcurses-core. The
# config, connections, recent, saved-workspace, history, utf8, text-input and
# session-converter sources are the ncurses TUI's own, compiled in here, so
# every frontend reads and writes the same files.

CC = clang
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -g
CFLAGS += -Isrc -I../../liblace/include -I../../tui/ncurses/src
LDFLAGS = -L../../liblace/build
LIBS = ../../liblace/build/liblace.a -lm -pthread

PKGS = notcurses-core libcjson
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDFLAGS += $(shell pkg-config --libs-only-L $(PKGS))
LIBS += $(shell pkg-config --libs-only-l $(PKGS))
CFLAGS += $(EXTRA_CFLAGS)

ifeq ($(SANITIZE),1)
    CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
    LDFLAGS += -fsanitize=address,undefined
endif
ifeq ($(SANITIZE),thread)
    CFLAGS += -fsanitize=thread -fno-omit-frame-pointer -O1
    LDFLAGS += -fsanitize=thread
endif

BUILD_DIR = build
SRC_DIR = src
TUI_SRC = ../../tui/ncurses/src

SRC_SUBDIRS = . conn model ui ui/dialogs
SRC_DIRS = $(addprefix $(SRC_DIR)/,$(SRC_SUBDIRS))
SRCS := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))

SHARED_NAMES = config/config config/connections config/recent config/saved_workspaces \
               config/workspace_json core/history core/utf8 core/ui_event \
               core/connstr_display views/text_input platform/posix/platform_posix
SHARED_OBJS = $(addprefix $(BUILD_DIR)/shared/,$(addsuffix .o,$(notdir $(SHARED_NAMES))))

DEPS := $(OBJS:.o=.d) $(SHARED_OBJS:.o=.d)
TARGET = $(BUILD_DIR)/lace-nc

all: $(TARGET)

$(TARGET): $(OBJS) $(SHARED_OBJS) ../../liblace/build/liblace.a
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJS) $(SHARED_OBJS) -o $@ $(LDFLAGS) $(LIBS)
	@if [ -x ../../laced/build/laced ] && [ ! -e $(BUILD_DIR)/laced ]; then \
	  ln -s ../../../laced/build/laced $(BUILD_DIR)/laced; fi

../../liblace/build/liblace.a: FORCE
	$(MAKE) -C ../../liblace
FORCE:

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# One rule per shared source (make cannot map a flat object name back to its
# directory with a pattern)
define SHARED_RULE
$(BUILD_DIR)/shared/$(notdir $(1)).o: $(TUI_SRC)/$(1).c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach s,$(SHARED_NAMES),$(eval $(call SHARED_RULE,$(s))))

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR)

debug: CFLAGS += -DDEBUG -O0
debug: clean all
release: CFLAGS += -O2 -DNDEBUG
release: clean all
run: all
	$(TARGET)

# Unit tests: every object but main.o, so tests call the real code
TEST_BIN = $(BUILD_DIR)/test_nc
TEST_OBJS = $(filter-out %/main.o,$(OBJS)) $(SHARED_OBJS)
$(TEST_BIN): tests/test_nc.c ../../tests/test.h $(TEST_OBJS) ../../liblace/build/liblace.a
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I../../tests $< $(TEST_OBJS) -o $@ $(LDFLAGS) $(LIBS)
test: $(TEST_BIN)
	./$(TEST_BIN)

PREFIX ?= /usr/local
DESTDIR ?=
install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/lace-nc

check-deps:
	@for p in $(PKGS); do pkg-config --exists $$p && echo "  $$p: OK" || echo "  $$p: MISSING"; done
	@echo "Debian/Ubuntu: apt install libnotcurses-dev libcjson-dev"
	@echo "Arch:          pacman -S notcurses cjson"
	@echo "macOS:         brew install notcurses cjson"

.PHONY: all clean debug release run test install check-deps FORCE
