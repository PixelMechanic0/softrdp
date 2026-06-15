ifeq ($(origin CC),default)
CC := gcc
endif

ifeq ($(origin RM),default)
RM := del /Q
endif
BUILD_DIR := build
BUILD32_DIR := build32
PJ64_CC ?= i686-w64-mingw32-gcc

CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Isrc
LDFLAGS ?=

CORE_SRCS := \
	src/core/sr.c \
	src/core/rdp_commands.c \
	src/core/rdp_memory.c \
	src/core/rdp_state.c \
	src/core/framebuffer.c \
	src/core/raster.c \
	src/core/tmem.c \
	src/core/pipeline.c \
	src/core/vi.c

PLUGIN_SRCS := \
	src/plugin/pj64/pj64_gfx.c

CORE_OBJS := $(patsubst src/core/%.c,$(BUILD_DIR)/core_%.o,$(CORE_SRCS))
PLUGIN_OBJS := $(patsubst src/plugin/pj64/%.c,$(BUILD_DIR)/pj64_%.o,$(PLUGIN_SRCS))
CORE32_OBJS := $(patsubst src/core/%.c,$(BUILD32_DIR)/core_%.o,$(CORE_SRCS))
PLUGIN32_OBJS := $(patsubst src/plugin/pj64/%.c,$(BUILD32_DIR)/pj64_%.o,$(PLUGIN_SRCS))

.PHONY: all pj64 clean dirs dirs32

all: $(BUILD_DIR)/softrdp-pj64.dll
pj64: $(BUILD32_DIR)/softrdp-pj64.dll

dirs:
	@if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)

dirs32:
	@if not exist $(BUILD32_DIR) mkdir $(BUILD32_DIR)

$(BUILD_DIR)/core_%.o: src/core/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pj64_%.o: src/plugin/pj64/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD32_DIR)/core_%.o: src/core/%.c | dirs32
	$(PJ64_CC) $(CFLAGS) -c $< -o $@

$(BUILD32_DIR)/pj64_%.o: src/plugin/pj64/%.c | dirs32
	$(PJ64_CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/softrdp-pj64.dll: $(CORE_OBJS) $(PLUGIN_OBJS)
	$(CC) -shared $^ -o $@ $(LDFLAGS)

$(BUILD32_DIR)/softrdp-pj64.dll: $(CORE32_OBJS) $(PLUGIN32_OBJS)
	$(PJ64_CC) -shared $^ -o $@ $(LDFLAGS)

clean:
	@if exist $(BUILD_DIR) $(RM) $(BUILD_DIR)\*.o $(BUILD_DIR)\*.exe $(BUILD_DIR)\*.dll
	@if exist $(BUILD32_DIR) $(RM) $(BUILD32_DIR)\*.o $(BUILD32_DIR)\*.exe $(BUILD32_DIR)\*.dll
