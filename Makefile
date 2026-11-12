CROSS_COMPILE ?=
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
ifeq ($(origin CXX),default)
CXX := $(CROSS_COMPILE)g++
endif
PYTHON ?= python3
CORE_FILENAME := lcd_libretro.so
BUILD_DIR ?= build
CPPFLAGS += -Iinclude -Isrc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -fPIC
CFLAGS += -fvisibility=hidden
LDLIBS += -lm
NO_UNDEFINED ?= -Wl,--no-undefined

CORE_SOURCES := \
	src/cpu/sm5xx.c \
	src/hardware/machine.c \
	src/content/registry.c \
	src/content/loader.c \
	src/content/gnw.c \
	src/content/samples.c \
	src/renderer/lcd_render.c \
	src/util/png.c \
	src/util/file.c \
	src/util/hash.c \
	src/util/inflate.c \
	src/util/zip.c \
	src/diagnostics/game.c \
	src/diagnostics/audio.c \
	src/diagnostics/display.c
SOURCES := src/libretro.c $(CORE_SOURCES)
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
TESTS := cpu_test machine_test loader_test render_test smoke game_test
FIXTURES := $(BUILD_DIR)/fixtures

.PHONY: all clean check tests tools reference-check
all: $(BUILD_DIR)/$(CORE_FILENAME)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJECTS:.o=.d)

$(BUILD_DIR)/$(CORE_FILENAME): $(OBJECTS) link.T
	$(CC) -shared $(NO_UNDEFINED) -Wl,--version-script=link.T $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

TEST_CFLAGS = $(filter-out -fvisibility=hidden,$(CFLAGS))
$(BUILD_DIR)/tests/%: tests/%.c tests/test_support.h $(SOURCES) $(wildcard src/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -Itests -DLCD_FIXTURES='"$(FIXTURES)"' $(TEST_CFLAGS) $(LDFLAGS) -o $@ $< $(SOURCES) $(LDLIBS)

$(FIXTURES)/selftest.gnw: tools/selftest_build.py
	$(PYTHON) -I tools/selftest_build.py --gnw $(FIXTURES)

$(BUILD_DIR)/lcdrun: tools/lcdrun.c include/libretro.h
	@mkdir -p $(dir $@)
	$(CC) -Iinclude $(TEST_CFLAGS) $(LDFLAGS) -o $@ $< -ldl

$(BUILD_DIR)/rtc_find: tools/rtc_find.c $(CORE_SOURCES) $(wildcard src/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) $(LDFLAGS) -o $@ $< $(CORE_SOURCES) $(LDLIBS)

tools: $(BUILD_DIR)/lcdrun $(BUILD_DIR)/rtc_find

tests: $(addprefix $(BUILD_DIR)/tests/,$(TESTS)) $(FIXTURES)/selftest.gnw

check: all tests
	@for t in $(TESTS); do ./$(BUILD_DIR)/tests/$$t || exit 1; done
	@nm -D --defined-only $(BUILD_DIR)/$(CORE_FILENAME) | grep -c ' T retro_' | \
		awk '{ if ($$1 != 25) { print "FAIL: expected 25 retro_ exports, found " $$1; exit 1 } else print "PASS: 25 retro_ symbols exported" }'

reference-check:
	$(MAKE) -C tests/reference MAME_SRC="$(MAME_SRC)" BUILD_DIR="$(abspath $(BUILD_DIR))/reference"
	$(BUILD_DIR)/reference/compare

clean:
	rm -rf $(BUILD_DIR)
