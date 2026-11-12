# Builds MAME 0.289's unmodified SM5xx CPU sources against a minimal framework
# shim and runs them in lockstep with the C port (src/cpu/sm5xx.c).
# Usage: make reference-check MAME_SRC=/path/to/mame (from the project root)
MAME_SRC ?=
BUILD_DIR ?= ../../build/reference
CXX ?= g++
CC ?= gcc
ROOT := $(abspath ../..)
MAME_CPU := $(MAME_SRC)/src/devices/cpu/sm510
MAME_FILES := sm510base.h sm510base.cpp sm510op.cpp sm510.h sm510.cpp sm500.h sm500.cpp sm500op.cpp sm5a.h sm5a.cpp sm511.h sm511.cpp sm530.h sm530.cpp sm530op.cpp
REF_CXXFLAGS := -std=c++17 -O1 -g -Dprotected=public -w

all: $(BUILD_DIR)/compare

$(BUILD_DIR)/src/.copied: $(addprefix $(MAME_CPU)/,$(MAME_FILES)) emu.h sm510d.h
	@test -n "$(MAME_SRC)" || { echo "MAME_SRC must point at a MAME source tree (tested with mame0289)"; exit 1; }
	@mkdir -p $(BUILD_DIR)/src
	cp $(addprefix $(MAME_CPU)/,$(MAME_FILES)) emu.h sm510d.h $(BUILD_DIR)/src/
	@touch $@

$(BUILD_DIR)/compare: $(BUILD_DIR)/src/.copied compare.cpp $(ROOT)/src/cpu/sm5xx.c $(ROOT)/src/cpu/sm5xx.h
	$(CC) -std=c11 -O1 -g -c $(ROOT)/src/cpu/sm5xx.c -o $(BUILD_DIR)/sm5xx.o
	$(CXX) $(REF_CXXFLAGS) -I$(BUILD_DIR)/src -I$(ROOT)/src -o $@ compare.cpp \
		$(addprefix $(BUILD_DIR)/src/,sm510base.cpp sm510op.cpp sm510.cpp sm500.cpp sm500op.cpp sm5a.cpp sm511.cpp sm530.cpp sm530op.cpp) \
		$(BUILD_DIR)/sm5xx.o
