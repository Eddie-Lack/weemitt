# Builds the brute forcer: one program, glover, with a command for each tool (src/main.c).
#
#   make                 Linux build: build/linux/glover
#   make windows         Windows build: build/windows/glover.exe (cross-compiled with MinGW-w64, 32-bit)
#   make test            the engine tests (tests/README.md)
#   make OPT=fast        a faster build of the game's code (see GAME_OPT below)
#
# The game's code comes from weemitt (the folder above): its game and runtime sources, compiled with weemitt's own
# flags, which this Makefile reads from weemitt's Makefile. It changes none of them. A build with OPT other than base adds
# GAME_OPT after them (see below).

TARGET ?= linux
W := ..
OPT ?= base

ifeq ($(TARGET),windows)
CC := i686-w64-mingw32-gcc
CXX := i686-w64-mingw32-g++
LD := i686-w64-mingw32-ld
OBJCOPY := i686-w64-mingw32-objcopy
EXE := .exe
BUILD ?= build/windows$(if $(filter-out base,$(OPT)),-$(OPT))
# The game's data holds pointers to the program's code and data. A state saved in one worker loads in another only when
# both hold these at the same addresses. So the program loads at one fixed address: no ASLR and no relocation table,
# which Windows would need to move it. The address is near the Linux build's (0x08048000). Lower down, the ROM offsets
# that the game's data holds (up to 0x77DE20) would look like pointers to the program to engine_check_hash. The
# program may use 4 GB of address space.
LDFLAGS := -static -Wl,--disable-dynamicbase -Wl,--disable-reloc-section -Wl,--large-address-aware \
           -Wl,--image-base=0x08000000
LIBS := -lm
else
CC := gcc
CXX := g++
LD := ld -m elf_i386
OBJCOPY := objcopy
EXE :=
BUILD ?= build/linux$(if $(filter-out base,$(OPT)),-$(OPT))
LDFLAGS := -no-pie -Wl,-z,noexecstack
LIBS := -lm -lrt -lpthread
endif

# weemitt's flags and source lists for its headless build (no renderer, the silent stand-in sound player). The game's
# file list leaves out devview.c: it only draws the Developer menu's views and keeps a large buffer of lines.
WVARS := $(BUILD)/weemitt.mk
$(shell mkdir -p $(BUILD) && $(MAKE) -s --no-print-directory -C $(W) -pn TARGET=$(TARGET) RENDERER=null AUDIO=stub nothing 2>/dev/null \
	| grep -E '^(GAME_CFLAGS|GAME_CXXFLAGS|GAME_SRCS|PORT_SRCS|PORT_CXX_SRCS|AUDIO_SRCS|ASM_SRCS|ARCH_FLAGS) :=' \
	| sed -e 's| -I| -I$(W)/|g' -e 's|-include src/|-include $(W)/src/|' -e 's|^\([A-Z_]*\) :=|W_\1 :=|' > $(WVARS).tmp \
	&& (cmp -s $(WVARS).tmp $(WVARS) || mv $(WVARS).tmp $(WVARS)); rm -f $(WVARS).tmp)
include $(WVARS)
ifeq ($(and $(W_GAME_CFLAGS),$(W_GAME_CXXFLAGS)),)
$(error cannot read the build flags from $(W)/Makefile)
endif

# The game's code keeps weemitt's flags exactly. GAME_OPT is added after them and may only change speed, never the
# result. tests/run.py --base compares a build's state trace with the base build's.
ifeq ($(OPT),base)
GAME_OPT :=
else ifeq ($(OPT),fast)
GAME_OPT := -O3 -fno-stack-protector -U_FORTIFY_SOURCE
else ifeq ($(OPT),znver4)
GAME_OPT := -O3 -fno-stack-protector -U_FORTIFY_SOURCE -march=znver4
else
GAME_OPT := $(OPT_FLAGS)
endif

GAME_CFLAGS := $(W_GAME_CFLAGS) $(GAME_OPT)
GAME_CXXFLAGS := $(W_GAME_CXXFLAGS) $(GAME_OPT)
GAME_SRCS := $(addprefix $(W)/,$(W_GAME_SRCS) $(filter-out src/port/devview.c,$(W_PORT_SRCS)) $(W_AUDIO_SRCS))
GAME_OBJS := $(patsubst $(W)/%.c,$(BUILD)/game/%.o,$(GAME_SRCS))
RAM_OBJ := $(BUILD)/game/gen/ram.o

# The engine and the programs are ordinary C for the host.
HOST_CFLAGS := $(W_ARCH_FLAGS) -std=gnu99 -O2 -g -fno-pie -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
               -Iengine -Isrc -I$(BUILD) -DBUILD_OPT='"$(OPT)"'

# The flags every object is built with. The file changes only when they change, and every object depends on it.
FLAGS_STAMP := $(BUILD)/flags
$(shell mkdir -p $(BUILD) && (echo '$(CC) $(GAME_CFLAGS) | $(CXX) $(GAME_CXXFLAGS) | $(HOST_CFLAGS)' | cmp -s - $(FLAGS_STAMP) \
	|| echo '$(CC) $(GAME_CFLAGS) | $(CXX) $(GAME_CXXFLAGS) | $(HOST_CFLAGS)' > $(FLAGS_STAMP)))

ENGINE_SRCS := engine/engine.c engine/track.c engine/split.c engine/host.c engine/hooks.c
# The game's functions that engine/hooks.c hooks or turns off.
HOOKS := $(shell sed -n 's/^\(HOOK[01A]\|OFF0\|DRAW0\|SKIP[0P]\)(\([A-Za-z0-9_]*\)[),].*/\2/p' engine/hooks.def)
ENGINE_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(ENGINE_SRCS)) $(BUILD)/symbols.o
LIB := $(BUILD)/libglover.a

PROGRAM_SRCS := $(wildcard src/*.c)
PROGRAM_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(PROGRAM_SRCS))
PROGRAM := $(BUILD)/glover$(EXE)

all: $(PROGRAM)

windows:
	$(MAKE) TARGET=windows

# The game's code in one relocatable object. Its writable data goes into two sections of its own, gstate_data and
# gstate_bss. A savestate copies exactly these and nothing of the C library or the programs. -d puts common symbols
# into .bss first. The game's memory (port_ram, gen/ram.S) stays apart: a savestate copies only the parts in use.
# --wrap sends the game's calls of the functions in engine/hooks.def to the engine's hooks.
$(BUILD)/game.o: $(GAME_OBJS) engine/hooks.def
	$(LD) -r -d $(addprefix --wrap=,$(HOOKS)) -o $@.tmp $(GAME_OBJS)
	$(OBJCOPY) --rename-section .data=gstate_data --rename-section .bss=gstate_bss $@.tmp $@
	rm -f $@.tmp
	@python3 tools/checkgame.py $@ $(TARGET)

$(LIB): $(BUILD)/game.o $(RAM_OBJ) $(ENGINE_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/game/%.o: $(W)/%.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(GAME_CFLAGS) -MMD -MP -c $< -o $@

# The game's files (src/game) and the runtime's files in PORT_CXX_SRCS are C compiled as C++, as in weemitt's build.
$(BUILD)/game/src/game/%.o: $(W)/src/game/%.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(GAME_CXXFLAGS) -MMD -MP -c $< -o $@

$(patsubst %.c,$(BUILD)/game/%.o,$(W_PORT_CXX_SRCS)): $(BUILD)/game/%.o: $(W)/%.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(GAME_CXXFLAGS) -MMD -MP -c $< -o $@

# The game's memory gets a section of its own too: the engine finds where its last part (port_hw) ends.
$(RAM_OBJ): $(W)/gen/ram.S $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(W_ARCH_FLAGS) -c $< -o $@.tmp
	$(OBJCOPY) --rename-section .bss=gram_bss $@.tmp $@
	rm -f $@.tmp

# The N64 address of every game global, read from weemitt's memory layout.
$(BUILD)/symbols.c: $(W)/gen/ram.S tools/gensyms.py
	python3 tools/gensyms.py $< > $@

$(BUILD)/symbols.o: $(BUILD)/symbols.c engine/engine.h $(FLAGS_STAMP)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD)/engine/%.o: engine/%.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/%.o: src/%.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -MMD -MP -c $< -o $@

$(PROGRAM): $(PROGRAM_OBJS) $(LIB)
	$(CC) $(W_ARCH_FLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

-include $(GAME_OBJS:.o=.d) $(ENGINE_OBJS:.o=.d) $(PROGRAM_OBJS:.o=.d)

# run.py reads the ROM from $GLOVER_ROM, which make passes on.
test: all
	python3 tests/run.py --build $(BUILD)

clean:
	rm -rf build

.PHONY: all windows test clean
# Keep the objects that pattern rules make on the way. Delete a target whose recipe fails (a game.o that failed
# checkgame.py, a half-written symbols.c).
.SECONDARY:
.DELETE_ON_ERROR:
