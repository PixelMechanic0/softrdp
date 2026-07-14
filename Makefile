ifeq ($(origin CC),default)
CC := gcc
endif

ifeq ($(origin RM),default)
RM := del /Q
endif
BUILD_DIR := build
BUILD32_DIR := build32
PJ64_CC ?= i686-w64-mingw32-gcc

CFLAGS ?= -O3 -g -flto
CFLAGS += -mavx2 -std=c17 -Wall -Wextra -Wpedantic -MMD -MP -Isrc
SOFTRDP_LOG ?= 0
CFLAGS += -DSOFTRDP_ENABLE_LOG=$(SOFTRDP_LOG)
LDFLAGS ?= -flto
PLUGIN_LIBS := -lopengl32 -lgdi32 -luser32

CORE_SRCS := \
	src/core/sr.c \
	src/core/rdp_commands.c \
	src/core/rdp_memory.c \
	src/core/framebuffer.c \
	src/core/combiner.c \
	src/core/blender.c \
	src/core/fragment.c \
	src/core/raster.c \
	src/core/raster_coverage.c \
	src/core/tmem.c \
	src/core/primitive_state.c \
	src/core/span_setup.c \
	src/core/pipeline.c \
	src/core/vi.c

PLUGIN_SRCS := \
	src/plugin/pj64/pj64_gfx.c \
	src/plugin/pj64/pj64_log.c

M64P_SRCS := \
	src/plugin/mupen64plus/mupen64plus_gfx.c

PRESENT_SRCS := \
	src/present/sr_present_gl_win32.c

CORE_OBJS := $(patsubst src/core/%.c,$(BUILD_DIR)/core_%.o,$(CORE_SRCS))
PLUGIN_OBJS := $(patsubst src/plugin/pj64/%.c,$(BUILD_DIR)/pj64_%.o,$(PLUGIN_SRCS))
M64P_OBJS := $(patsubst src/plugin/mupen64plus/%.c,$(BUILD_DIR)/m64p_%.o,$(M64P_SRCS))
PRESENT_OBJS := $(patsubst src/present/%.c,$(BUILD_DIR)/present_%.o,$(PRESENT_SRCS))
CORE32_OBJS := $(patsubst src/core/%.c,$(BUILD32_DIR)/core_%.o,$(CORE_SRCS))
PLUGIN32_OBJS := $(patsubst src/plugin/pj64/%.c,$(BUILD32_DIR)/pj64_%.o,$(PLUGIN_SRCS))
M64P32_OBJS := $(patsubst src/plugin/mupen64plus/%.c,$(BUILD32_DIR)/m64p_%.o,$(M64P_SRCS))
PRESENT32_OBJS := $(patsubst src/present/%.c,$(BUILD32_DIR)/present_%.o,$(PRESENT_SRCS))
.PHONY: all pj64 mupen64plus mupen64plus32 clean dirs dirs32

all: $(BUILD_DIR)/softrdp-pj64.dll $(BUILD_DIR)/mupen64plus-video-softrdp.dll
pj64: $(BUILD32_DIR)/softrdp-pj64.dll $(BUILD32_DIR)/mupen64plus-video-softrdp.dll

mupen64plus: $(BUILD_DIR)/mupen64plus-video-softrdp.dll
mupen64plus32: $(BUILD32_DIR)/mupen64plus-video-softrdp.dll

dirs:
	@if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)

dirs32:
	@if not exist $(BUILD32_DIR) mkdir $(BUILD32_DIR)

$(BUILD_DIR)/core_%.o: src/core/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pj64_%.o: src/plugin/pj64/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/m64p_%.o: src/plugin/mupen64plus/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/present_%.o: src/present/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD32_DIR)/core_%.o: src/core/%.c | dirs32
	$(PJ64_CC) $(CFLAGS) -c $< -o $@

$(BUILD32_DIR)/pj64_%.o: src/plugin/pj64/%.c | dirs32
	$(PJ64_CC) $(CFLAGS) -c $< -o $@

$(BUILD32_DIR)/m64p_%.o: src/plugin/mupen64plus/%.c | dirs32
	$(PJ64_CC) $(CFLAGS) -c $< -o $@

$(BUILD32_DIR)/present_%.o: src/present/%.c | dirs32
	$(PJ64_CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/softrdp-pj64.dll: $(CORE_OBJS) $(PRESENT_OBJS) $(PLUGIN_OBJS)
	$(CC) -shared $^ -o $@ $(LDFLAGS) $(PLUGIN_LIBS)

$(BUILD_DIR)/mupen64plus-video-softrdp.dll: $(CORE_OBJS) $(PRESENT_OBJS) $(M64P_OBJS)
	$(CC) -shared $^ -o $@ $(LDFLAGS) $(PLUGIN_LIBS)

$(BUILD32_DIR)/softrdp-pj64.dll: $(CORE32_OBJS) $(PRESENT32_OBJS) $(PLUGIN32_OBJS)
	$(PJ64_CC) -shared $^ -o $@ $(LDFLAGS) $(PLUGIN_LIBS)

$(BUILD32_DIR)/mupen64plus-video-softrdp.dll: $(CORE32_OBJS) $(PRESENT32_OBJS) $(M64P32_OBJS)
	$(PJ64_CC) -shared $^ -o $@ $(LDFLAGS) $(PLUGIN_LIBS)

clean:
	@if exist $(BUILD_DIR) $(RM) $(BUILD_DIR)\*.o $(BUILD_DIR)\*.d $(BUILD_DIR)\*.exe $(BUILD_DIR)\*.dll
	@if exist $(BUILD32_DIR) $(RM) $(BUILD32_DIR)\*.o $(BUILD32_DIR)\*.d $(BUILD32_DIR)\*.exe $(BUILD32_DIR)\*.dll

-include $(DEPS)
