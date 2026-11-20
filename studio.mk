# The studio build (make studio, make studio-windows): the window program with the brute forcer (bruteforce/).
# The Makefile includes this file when STUDIO is 1.
#
# The game's objects are linked into one relocatable object, game.o, as the brute forcer's engine does
# (bruteforce/docs/engine.md). Its writable data goes into the sections gstate_data and gstate_bss, which a savestate
# copies, and the game's calls of the functions in bruteforce/engine/hooks.def go to the engine's hooks. The game's
# memory (gen/ram.S) gets a section of its own, gram_bss. The platform layer, the renderer and the menu stay outside
# the saved state. devview.c is game code that only the overlays read: it stays outside too.

BF := bruteforce

ifeq ($(TARGET),windows)
STUDIO_LD := i686-w64-mingw32-ld
STUDIO_OBJCOPY := i686-w64-mingw32-objcopy
# A state holds pointers to the program's code and data: the program loads at one fixed address, without ASLR and
# without a relocation table (bruteforce/Makefile).
LDFLAGS += -Wl,--disable-dynamicbase -Wl,--disable-reloc-section -Wl,--large-address-aware -Wl,--image-base=0x08000000
else
STUDIO_LD := ld -m elf_i386
STUDIO_OBJCOPY := objcopy
endif

HOST_CFLAGS += -DPORT_STUDIO -I$(BF)/engine -I$(BF)/src
HOST_CXXFLAGS += -DPORT_STUDIO -I$(BF)/engine
BF_CFLAGS := $(ARCH_FLAGS) -std=gnu99 -O2 -g -fno-pie -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
             -I$(BF)/engine -I$(BF)/src -I$(BUILD)/bf -DBUILD_OPT='"studio"' -DGLOVER_EMBEDDED

STUDIO_HOOKS := $(shell sed -n 's/^\(HOOK[01A]\|OFF0\|DRAW0\|SKIP[0P]\)(\([A-Za-z0-9_]*\)[),].*/\2/p' $(BF)/engine/hooks.def)
STUDIO_GAME_OBJS := $(filter-out $(BUILD)/gen/ram.o $(BUILD)/src/port/devview.o,$(GAME_OBJS))
STUDIO_RAM_OBJ := $(BUILD)/gen/ram-studio.o
BF_ENGINE_OBJS := $(patsubst %.c,$(BUILD)/bf/%.o,$(addprefix engine/,engine.c track.c hooks.c split.c)) \
                  $(BUILD)/bf/symbols.o
BF_PROGRAM_OBJS := $(patsubst $(BF)/%.c,$(BUILD)/bf/%.o,$(wildcard $(BF)/src/*.c))
STUDIO_OBJS := $(BUILD)/src/port/studio.o

$(BUILD)/game.o: $(STUDIO_GAME_OBJS) $(BF)/engine/hooks.def
	$(STUDIO_LD) -r -d $(addprefix --wrap=,$(STUDIO_HOOKS)) -o $@.tmp $(STUDIO_GAME_OBJS)
	$(STUDIO_OBJCOPY) --rename-section .data=gstate_data --rename-section .bss=gstate_bss $@.tmp $@
	rm -f $@.tmp
	@python3 $(BF)/tools/checkgame.py $@ $(TARGET)

$(STUDIO_RAM_OBJ): $(BUILD)/gen/ram.o
	$(STUDIO_OBJCOPY) --rename-section .bss=gram_bss $< $@

$(BUILD)/bf/symbols.c: gen/ram.S $(BF)/tools/gensyms.py
	@mkdir -p $(dir $@)
	python3 $(BF)/tools/gensyms.py $< > $@

$(BUILD)/bf/symbols.o: $(BUILD)/bf/symbols.c $(BF)/engine/engine.h
	$(CC) $(BF_CFLAGS) -c $< -o $@

$(BUILD)/bf/%.o: $(BF)/%.c $(wildcard $(BF)/engine/*.h $(BF)/src/*.h) $(BF)/engine/hooks.def
	@mkdir -p $(dir $@)
	$(CC) $(BF_CFLAGS) -c $< -o $@

# The build's identity in a state file (src/port/studio.c): a hash of game.o, which holds the game's code and the
# layout of its variables, and the program's version.
$(BUILD)/src/port/studio.o: src/port/studio.c src/port/studio.h $(BF)/engine/engine.h $(BUILD)/game.o $(BUILD)/build-id
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -DSTUDIO_GAME_HASH=0x$$(sha256sum $(BUILD)/game.o | cut -c1-16)ull \
	    -DSTUDIO_VERSION="\"$$(cat $(BUILD)/build-id)\"" -c $< -o $@

$(BUILD)/src/port/platform.o: src/port/studio.h

$(EXE): $(BUILD)/game.o $(STUDIO_RAM_OBJ) $(BUILD)/src/port/devview.o $(HOST_OBJS) $(BF_ENGINE_OBJS) \
        $(BF_PROGRAM_OBJS) $(STUDIO_OBJS)
	$(CXX) $(ARCH_FLAGS) $(LDFLAGS) -o $@ $^ $(SDL_LIBS) -lm $(if $(filter windows,$(TARGET)),,-lrt -lpthread)

$(BUILD)/src/port/menu.o: src/port/studio.h
