ifeq ($(origin CC),default)
CC := gcc
endif
ifeq ($(origin CXX),default)
CXX := g++
endif
ifeq ($(origin RM),default)
RM := del /Q
endif

# 32-bit MinGW cross toolchain (Project64 target)
PJ64_CC  ?= i686-w64-mingw32-gcc
PJ64_CXX ?= i686-w64-mingw32-g++

# build   = 64-bit objects + mupen64plus DLL
# build32 = 32-bit objects + Project64 DLL
BUILD_DIR   := build
BUILD32_DIR := build32

# ---- Flags -----------------------------------------------------------------
SOFTRDP_LOG ?= 0

CFLAGS ?= -O3 -g -flto
CFLAGS += -mavx2 -std=c17 -Wall -Wextra -Wpedantic -MMD -MP -Isrc \
	-DSOFTRDP_ENABLE_LOG=$(SOFTRDP_LOG)

CXXFLAGS ?= -O3 -g -flto
CXXFLAGS += -mavx2 -std=gnu++20 -Drestrict=__restrict -Wall -Wextra -Wpedantic \
	-Wno-narrowing -Wno-c99-extensions \
	-Wno-missing-field-initializers -Wno-missing-designated-field-initializers \
	-Wno-missing-braces -MMD -MP -Isrc \
	-DSOFTRDP_ENABLE_LOG=$(SOFTRDP_LOG)

LDFLAGS ?= -flto
PLUGIN_LIBS := -lopengl32 -lgdi32 -luser32

# ---- Sources ---------------------------------------------------------------
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

PJ64_SRCS := \
	src/plugin/pj64/pj64_gfx.c \
	src/plugin/pj64/pj64_dump.c \
	src/plugin/pj64/pj64_log.c

M64P_SRCS := \
	src/plugin/mupen64plus/mupen64plus_gfx.c

PRESENT_SRCS := \
	src/present/sr_present_gl_win32.c

# ---- Objects ---------------------------------------------------------------
# 64-bit set (mupen64plus): core + present + m64p
CORE_OBJS    := $(patsubst src/core/%.c,$(BUILD_DIR)/core_%.o,$(CORE_SRCS))
PRESENT_OBJS := $(patsubst src/present/%.c,$(BUILD_DIR)/present_%.o,$(PRESENT_SRCS))
M64P_OBJS    := $(patsubst src/plugin/mupen64plus/%.c,$(BUILD_DIR)/m64p_%.o,$(M64P_SRCS))

# 32-bit set (Project64): core + present + pj64
CORE32_OBJS    := $(patsubst src/core/%.c,$(BUILD32_DIR)/core_%.o,$(CORE_SRCS))
PRESENT32_OBJS := $(patsubst src/present/%.c,$(BUILD32_DIR)/present_%.o,$(PRESENT_SRCS))
PJ64_OBJS      := $(patsubst src/plugin/pj64/%.c,$(BUILD32_DIR)/pj64_%.o,$(PJ64_SRCS))

PJ64_DLL := $(BUILD32_DIR)/softrdp-pj64.dll
M64P_DLL := $(BUILD_DIR)/mupen64plus-video-softrdp.dll

# ---- Top-level targets -----------------------------------------------------
.PHONY: all pj64 mupen64plus clean

all: pj64 mupen64plus
pj64: $(PJ64_DLL)
mupen64plus: $(M64P_DLL)

# ---- Directory creation (order-only, never triggers a rebuild) -------------
$(BUILD_DIR) $(BUILD32_DIR):
	@if not exist $@ mkdir $@

# ---- Compile: 64-bit (host) ------------------------------------------------
$(BUILD_DIR)/core_%.o: src/core/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# pipeline.c and fragment.c are compiled as C++.
$(BUILD_DIR)/core_pipeline.o: src/core/pipeline.c | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -x c++ -c $< -o $@
$(BUILD_DIR)/core_fragment.o: src/core/fragment.c | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -x c++ -c $< -o $@

$(BUILD_DIR)/m64p_%.o: src/plugin/mupen64plus/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/present_%.o: src/present/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Compile: 32-bit (MinGW cross) -----------------------------------------
$(BUILD32_DIR)/core_%.o: src/core/%.c | $(BUILD32_DIR)
	$(PJ64_CC) $(CFLAGS) -c $< -o $@

$(BUILD32_DIR)/core_pipeline.o: src/core/pipeline.c | $(BUILD32_DIR)
	$(PJ64_CXX) $(CXXFLAGS) -x c++ -c $< -o $@
$(BUILD32_DIR)/core_fragment.o: src/core/fragment.c | $(BUILD32_DIR)
	$(PJ64_CXX) $(CXXFLAGS) -x c++ -c $< -o $@

$(BUILD32_DIR)/pj64_%.o: src/plugin/pj64/%.c | $(BUILD32_DIR)
	$(PJ64_CC) $(CFLAGS) -c $< -o $@
$(BUILD32_DIR)/present_%.o: src/present/%.c | $(BUILD32_DIR)
	$(PJ64_CC) $(CFLAGS) -c $< -o $@

# ---- Link ------------------------------------------------------------------
$(M64P_DLL): $(CORE_OBJS) $(PRESENT_OBJS) $(M64P_OBJS)
	$(CC) -shared $^ -o $@ $(LDFLAGS) $(PLUGIN_LIBS)

$(PJ64_DLL): $(CORE32_OBJS) $(PRESENT32_OBJS) $(PJ64_OBJS)
	$(PJ64_CC) -shared $^ -o $@ $(LDFLAGS) $(PLUGIN_LIBS)

# ---- Clean -----------------------------------------------------------------
clean:
	@if exist $(BUILD_DIR) $(RM) $(BUILD_DIR)\*.o $(BUILD_DIR)\*.d $(BUILD_DIR)\*.dll
	@if exist $(BUILD32_DIR) $(RM) $(BUILD32_DIR)\*.o $(BUILD32_DIR)\*.d $(BUILD32_DIR)\*.dll

# ---- Auto-generated header dependencies ------------------------------------
# This is what makes incremental builds correct: touching a header now
# recompiles exactly the translation units that include it (and nothing else),
# instead of forcing a full clean rebuild.
DEPS := $(wildcard $(BUILD_DIR)/*.d $(BUILD32_DIR)/*.d)
-include $(DEPS)
