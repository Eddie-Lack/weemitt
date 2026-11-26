# Builds the PC port of Glover.
#
#   make                 Linux build: build/linux/weemitt (32-bit x86)
#   make windows         Windows build: build/windows/weemitt.exe (cross-compiled with MinGW, 32-bit)
#   make linux64         64-bit Linux build: build/linux64/weemitt (docs/port64.md)
#   make windows64       64-bit Windows build: build/windows64/weemitt.exe
#   make headless64      the headless build, 64-bit: build/headless64/weemitt
#   make headless64-mus  the same with the real sound player: build/headless64-mus/weemitt
#   make headless        the test build: build/headless/weemitt, no renderer and the stand-in sound player
#   make headless-displaced   the headless build with port_ram away from the base of its pointer words
#                        (docs/port64.md): build/headless-displaced/weemitt
#   make headless-mus    the headless build with the real sound player: build/headless-mus/weemitt
#   make headless-displaced-mus   the same, displaced: build/headless-displaced-mus/weemitt
#   make space-demo      the Space demo for Linux: build/space-demo/weemitt (src/port/spacedemo.c)
#   make space-demo-windows   the Space demo for Windows: build/space-demo-windows/weemitt.exe
#   make RENDERER=null   a build without graphics
#   make AUDIO=stub      a build with the silent stand-in sound player
#   make studio          the studio for Linux: build/studio/weemitt-studio (the window program with the brute forcer)
#   make studio-windows  the studio for Windows: build/studio-windows/weemitt-studio.exe
#   make gen             regenerate gen/ from a gloveworld checkout (GLOVEWORLD=path, default ../gloveworld)
#   make BITS=64         any build as a 64-bit program
#   make lib64           the game as a library with no window (docs/library.md): build/lib64/libglover.so
#   make lib64-stub      the same with the stand-in sound player and no renderer, for the tests
#   make lib64-hostgfx   the library with glover_host_gfx and no OpenGL renderer: build/lib64-hostgfx/libglover.so
#   make lib-windows64   the library for Windows: build/lib-windows64/glover.dll (and lib-windows64-stub)
#   make libhost64       the library's test host: build/libhost64/glover-host (libhost-windows64 for Windows)
#   make libglshot64     the library's picture test: build/libglshot64/glover-glshot
#   make libcheck        the library's checks (tools/libcheck.py)
#   make lib-macos       the library for macOS on arm64, on macOS: build/lib-macos/libglover.dylib (and lib-macos-stub)
#   make lib-ios         the library for iOS devices (arm64), on macOS: build/lib-ios/glover.framework
#   make lib-iossim      the library for the iOS Simulator on arm64 Macs: build/lib-iossim/glover.framework
#   make lib-xcframework both iOS frameworks joined: build/glover.xcframework
#   make libhost-macos   the test host for macOS: build/libhost-macos/glover-host (libhost-iossim: the Simulator's)
#
# The game's code keeps the N64's 32-bit pointers and memory layout. A 64-bit build stores them as 32-bit words
# (docs/port64.md).

TARGET ?= linux
# The oldest systems that the Apple libraries run on.
MACOS_MIN ?= 11.0
IOS_MIN ?= 15.0
RENDERER ?= gl
# 32: an i386 program (the default). 64: an x86-64 program.
BITS ?= 32
# mus: the real sound player: libmus (src/audio/mus), the synthesizer and its command interpreter (src/audio/synth).
# stub: the stand-in sound player, which plays nothing and writes nothing to the game's memory. The tests use it.
AUDIO ?= mus

GAME_SRCS := $(sort $(wildcard src/game/*.c))
# These files of the runtime read the game's structures. They are C compiled as C++ with C linkage, as the game's
# files are (docs/port64.md).
PORT_CXX_SRCS := src/port/frame.c src/port/spacedemo.c src/port/loaders.c src/port/os.c src/port/sprite.c \
                 src/port/libultra.c src/port/devview.c src/port/debugpad.c src/port/gu.c src/port/portmath.c \
                 src/port/hostworld.c src/port/drawmark.c
PORT_SRCS := src/port/frame.c src/port/spacedemo.c src/port/loaders.c src/port/os.c src/port/inputlatch.c src/port/gu.c src/port/sprite.c \
             src/port/libultra.c src/port/devview.c src/port/debugpad.c src/port/portmath.c gen/ramtab.c gen/romsyms.c \
             gen/layouts.c
# The host world (docs/host-world.md): in weemitt's programs and the library. The brute forcer reads PORT_SRCS and
# leaves it out.
HOSTWORLD_SRCS := src/port/hostworld.c src/port/drawmark.c
ifeq ($(AUDIO),stub)
AUDIO_SRCS := src/port/audio_stub.c
else
AUDIO_SRCS := $(sort $(wildcard src/audio/*.c src/audio/mus/*.c src/audio/synth/*.c))
AUDIO_DEFINES := -DPORT_AUDIO_MUS
endif
PLATFORM_SRCS := src/port/platform.c src/port/inputevent.c src/port/settings.c src/port/log.c src/port/record.c src/port/hostscene.c \
                 src/port/rom.c src/port/statehash.c
# The menu is C++ (Dear ImGui, third_party/imgui). Builds without a renderer get a stand-in that does nothing.
ifeq ($(RENDERER),null)
GFX_SRCS := src/port/gfx_null.c src/port/menu_null.c
CXX_SRCS :=
else ifeq ($(RENDERER),hostgfx)
# Only the library: the display list walk and the translation for the host's interpreter (glover_host_gfx), with no
# OpenGL. gfx_sink_null.c stands in for gfx_render.c, and nothing for gfx_tex.c's decoded textures.
ifneq ($(LIB),1)
$(error RENDERER=hostgfx is for the library only (LIB=1))
endif
GFX_SRCS := src/gfx/gfx_dl.c src/gfx/gfx_interp.c src/gfx/gfx_hostgfx.c src/gfx/gfx_mem.c src/gfx/gfx_host.c \
            src/gfx/gfx_tmem.c src/gfx/gfx_sink_null.c
CXX_SRCS :=
else
GFX_SRCS := $(filter-out src/gfx/gfx_sink_null.c,$(sort $(wildcard src/gfx/*.c)))
IMGUI_SRCS := $(addprefix third_party/imgui/,imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp \
              backends/imgui_impl_sdl2.cpp backends/imgui_impl_opengl3.cpp)
CXX_SRCS := src/port/menu.cpp $(IMGUI_SRCS)
endif
ASM_SRCS := gen/ram.S

ifeq ($(TARGET),windows)
MINGW := $(if $(filter 64,$(BITS)),x86_64,i686)-w64-mingw32
CC := $(MINGW)-gcc
CXX := $(MINGW)-g++
BUILD ?= build/windows
EXE := $(BUILD)/weemitt$(if $(filter 1,$(STUDIO)),-studio).exe
SDL_PREFIX ?= deps/SDL2-mingw/$(MINGW)
ifeq ($(wildcard $(SDL_PREFIX)/include/SDL2/SDL.h),)
ifneq ($(LIB),1)
$(error $(SDL_PREFIX) has no SDL2 for Windows. Run tools/build_sdl_mingw.sh$(if $(filter 64,$(BITS)), x86_64), or set SDL_PREFIX (docs/building.md))
endif
endif
SDL_CFLAGS := -I$(SDL_PREFIX)/include -I$(SDL_PREFIX)/include/SDL2
SDL_LIBS := -L$(SDL_PREFIX)/lib -lmingw32 -lSDL2main -lSDL2 -mwindows
LDFLAGS := -static-libgcc -static-libstdc++
# The 64-bit program keeps MinGW's image base, 0x140000000, which lies above 4 GB: a pointer cut to 32 bits faults.
# MinGW lays out bitfields the Microsoft way by default. The game's structures need the N64's layout: with the
# Microsoft one, the display-list entry (Gfx) takes 16 bytes instead of 8, and a level's lists overrun their buffers.
GAME_LAYOUT_FLAGS := -mno-ms-bitfields
else ifneq ($(filter macos ios iossim,$(TARGET)),)
# Apple's systems on arm64, built with Xcode's clang (docs/library.md): only the library (LIB=1 BITS=64).
ifneq ($(LIB)$(BITS),164)
$(error TARGET=$(TARGET) builds only the library (LIB=1 BITS=64))
endif
APPLE_SDK := $(if $(filter macos,$(TARGET)),macosx,$(if $(filter ios,$(TARGET)),iphoneos,iphonesimulator))
APPLE_TRIPLE := $(if $(filter macos,$(TARGET)),arm64-apple-macos$(MACOS_MIN),arm64-apple-ios$(IOS_MIN)$(if $(filter iossim,$(TARGET)),-simulator))
APPLE_CC ?= xcrun --sdk $(APPLE_SDK) clang
CC := $(APPLE_CC)
CXX := $(APPLE_CC)++
BUILD ?= build/lib-$(TARGET)
else
CC := gcc
CXX := g++
BUILD ?= build/linux
EXE := $(BUILD)/weemitt$(if $(filter 1,$(STUDIO)),-studio)
ifeq ($(BITS),64)
SDL_CFLAGS := -I/usr/include/SDL2
SDL_LIBS := -lSDL2
# A position-independent program loads above 4 GB: a pointer cut to 32 bits faults.
LDFLAGS := -pie -Wl,-z,noexecstack
else
# Debian and Ubuntu install only the 64-bit SDL2 headers; they work for a 32-bit build with the 32-bit runtime
# library (libsdl2-2.0-0:i386). The one architecture-specific header is copied next to the build.
SDL_CFLAGS := -I$(BUILD)/sdl-include -I/usr/include/SDL2
SDL_LIBS := -L/usr/lib/i386-linux-gnu -l:libSDL2-2.0.so.0
LDFLAGS := -no-pie -Wl,-z,noexecstack
endif
endif

# The game's code: N64 structure alignment (doubles and 64-bit integers on 8 bytes; x86-64 has it already),
# unsigned char, wrapping arithmetic, no reordering of floating-point operations, SSE arithmetic (no x87 excess
# precision).
ifeq ($(BITS),64)
ARCH_FLAGS := -m64
PIC_FLAGS := $(if $(filter windows,$(TARGET)),,-fpie)
else
ARCH_FLAGS := -m32 -msse2 -mfpmath=sse
ALIGN_FLAGS := -malign-double
PIC_FLAGS := -fno-pie
endif
# Apple: the target triple chooses arm64 and the system. Clang has no -fno-aggressive-loop-optimizations; -fwrapv
# already keeps signed overflow defined. Clang refuses narrowing conversions in the game's C++ initializers, which GCC
# accepts. libc++'s <math.h> and <stdlib.h> add C++ overloads that port_pre.h refuses, and libc++ has no switch like
# libstdc++'s to leave them out: the game's files, which include only C headers, are compiled without its headers.
# Apple's SDK turns sprintf and others into macros when _FORTIFY_SOURCE is on; the game's headers declare them again.
ifneq ($(APPLE_TRIPLE),)
ARCH_FLAGS := -target $(APPLE_TRIPLE)
COMPILER_GAME_FLAGS := -D_FORTIFY_SOURCE=0
CLANG_FLAGS := -nostdinc++ -Wno-c++11-narrowing
else
COMPILER_GAME_FLAGS := -fno-aggressive-loop-optimizations
endif
# LIB=1: the game as a library (docs/library.md), 32-bit or 64-bit. Its code is position-independent on Linux (a PE
# DLL uses base relocations), and only the functions of glover.h are visible outside it.
ifeq ($(LIB),1)
PIC_FLAGS := $(if $(filter windows,$(TARGET)),,-fPIC)
LIB_FLAGS := -fvisibility=hidden -DPORT_LIB
endif
GAME_FLAGS := $(ARCH_FLAGS) $(GAME_LAYOUT_FLAGS) $(ALIGN_FLAGS) $(LIB_FLAGS) -O2 -g -funsigned-char -fno-strict-aliasing -fwrapv \
              -ffp-contract=off $(COMPILER_GAME_FLAGS) -fno-delete-null-pointer-checks $(PIC_FLAGS) -w \
              -DM2CTX -D_LANGUAGE_C -DF3DEX_GBI -D_FINALROM -DNDEBUG -DPORT \
              -Igen -Isrc/port -Iinclude -Iinclude/PR -Isrc/game -include src/port/port_pre.h
# DISPLACED=1: the test build of the pointer representation (docs/port64.md). port_ram lies 32 MB above the base of
# the stored words, behind a guard that faults on any use of a word as a pointer. Every stored pointer is checked.
ifeq ($(DISPLACED),1)
PTR_FLAGS := -DPORT_PTR_DISPLACED -DPORT_PTR_CHECK
GAME_FLAGS += $(PTR_FLAGS)
endif
# Most of the runtime (src/port) and gen/ are C. The game's files (src/game), the sound player (src/audio) and the
# runtime's files in PORT_CXX_SRCS are C compiled as C++, with C linkage (docs/port64.md). -fno-finite-loops keeps C's rule for loops: C++ lets GCC assume that every loop without
# side effects ends.
GAME_CFLAGS := -std=gnu89 $(GAME_FLAGS)
GAME_CXXFLAGS := -x c++ -std=gnu++14 -fno-exceptions -fno-rtti -fno-finite-loops $(CLANG_FLAGS) $(GAME_FLAGS)
# The platform layer and the renderer talk to SDL and OpenGL, so they keep the system's structure layout.
HOST_CFLAGS := $(ARCH_FLAGS) -std=gnu99 -O2 -g $(PIC_FLAGS) -Wall -Wno-unused-function -Isrc/port -Isrc/gfx $(SDL_CFLAGS) \
               $(AUDIO_DEFINES) $(PTR_FLAGS)
HOST_CFLAGS += $(LIB_FLAGS)
# SPACE_DEMO=1: the platform layer starts the Space demo without options.
ifeq ($(SPACE_DEMO),1)
HOST_CFLAGS += -DPORT_SPACE_DEMO
endif

GAME_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(GAME_SRCS) $(PORT_SRCS) $(HOSTWORLD_SRCS) $(AUDIO_SRCS)) $(patsubst %.S,$(BUILD)/%.o,$(ASM_SRCS))
HOST_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(PLATFORM_SRCS) $(GFX_SRCS)) $(patsubst %.cpp,$(BUILD)/%.o,$(CXX_SRCS))
# The menu and Dear ImGui are C++ for the host: they do not get the game's flags.
HOST_CXXFLAGS := $(ARCH_FLAGS) -std=gnu++14 -O2 -g $(PIC_FLAGS) -fno-exceptions -fno-rtti -Isrc/port -Ithird_party/imgui \
                 $(SDL_CFLAGS)

# The library's objects: the game, the sound player, the ROM checks, the memory hash and src/lib. With the renderer,
# src/gfx too. Not the platform layer, the menu or SDL.
LIB_SRCS := src/lib/glover.c src/lib/host.c src/lib/audio.c src/lib/coll.c src/lib/view.c \
            $(if $(filter gl,$(RENDERER)),src/lib/gl.c,src/lib/gl_null.c) \
            $(if $(filter null,$(RENDERER)),src/lib/hostgfx_null.c,src/lib/hostgfx.c)
LIB_OBJS := $(GAME_OBJS) $(BUILD)/src/port/rom.o $(BUILD)/src/port/statehash.o $(patsubst %.c,$(BUILD)/%.o,$(LIB_SRCS)) \
            $(if $(filter null,$(RENDERER)),,$(patsubst %.c,$(BUILD)/%.o,$(GFX_SRCS)))
LIB_OUT := $(BUILD)/$(if $(filter windows,$(TARGET)),glover.dll,$(if $(filter macos,$(TARGET)),libglover.dylib,$(if $(APPLE_TRIPLE),glover.framework/glover,libglover.so)))

all: $(if $(filter 1,$(LIB)),$(LIB_OUT),$(EXE))

# The sound player that the build directory was last built with. The file changes only when AUDIO changes, and the
# platform layer, which opens the sound device only with the real player, is then compiled again.
AUDIO_MODE := $(BUILD)/audio-mode
$(shell mkdir -p $(BUILD) && (echo $(AUDIO) | cmp -s - $(AUDIO_MODE) || echo $(AUDIO) > $(AUDIO_MODE)))
# The renderer that the build directory was last linked with, in the same way: a change of RENDERER links the program
# again. Without it, `make RENDERER=null` and a later `make` would leave the program without its renderer.
RENDERER_MODE := $(BUILD)/renderer-mode
$(shell mkdir -p $(BUILD) && (echo $(RENDERER) | cmp -s - $(RENDERER_MODE) || echo $(RENDERER) > $(RENDERER_MODE)))

windows:
	$(MAKE) TARGET=windows

headless:
	$(MAKE) BUILD=build/headless RENDERER=null AUDIO=stub

headless-displaced:
	$(MAKE) BUILD=build/headless-displaced RENDERER=null AUDIO=stub DISPLACED=1

headless-mus:
	$(MAKE) BUILD=build/headless-mus RENDERER=null AUDIO=mus

headless-displaced-mus:
	$(MAKE) BUILD=build/headless-displaced-mus RENDERER=null AUDIO=mus DISPLACED=1

# The 64-bit builds (docs/port64.md).
linux64:
	$(MAKE) BITS=64 BUILD=build/linux64

windows64:
	$(MAKE) TARGET=windows BITS=64 BUILD=build/windows64

headless64:
	$(MAKE) BITS=64 BUILD=build/headless64 RENDERER=null AUDIO=stub

headless64-mus:
	$(MAKE) BITS=64 BUILD=build/headless64-mus RENDERER=null AUDIO=mus

# The library (docs/library.md). The stub build has the stand-in sound player and no renderer: regress.py's
# baseline holds the stand-in's memory.
lib64:
	$(MAKE) LIB=1 BITS=64 BUILD=build/lib64 RENDERER=gl AUDIO=mus

# The library without OpenGL: glover_host_gfx only (the iOS libraries are built this way).
lib64-hostgfx:
	$(MAKE) LIB=1 BITS=64 BUILD=build/lib64-hostgfx RENDERER=hostgfx AUDIO=mus

lib64-stub:
	$(MAKE) LIB=1 BITS=64 BUILD=build/lib64-stub RENDERER=null AUDIO=stub

lib-windows64:
	$(MAKE) LIB=1 BITS=64 TARGET=windows BUILD=build/lib-windows64 RENDERER=gl AUDIO=mus

lib-windows64-stub:
	$(MAKE) LIB=1 BITS=64 TARGET=windows BUILD=build/lib-windows64-stub RENDERER=null AUDIO=stub

lib32:
	$(MAKE) LIB=1 BITS=32 BUILD=build/lib32 RENDERER=gl AUDIO=mus

lib32-stub:
	$(MAKE) LIB=1 BITS=32 BUILD=build/lib32-stub RENDERER=null AUDIO=stub

lib-windows32:
	$(MAKE) LIB=1 BITS=32 TARGET=windows BUILD=build/lib-windows32 RENDERER=gl AUDIO=mus

lib-windows32-stub:
	$(MAKE) LIB=1 BITS=32 TARGET=windows BUILD=build/lib-windows32-stub RENDERER=null AUDIO=stub

# Apple's systems, on macOS with Xcode (docs/library.md). The libraries have glover_host_gfx and no OpenGL renderer.
# The stub build of macOS is for the comparison with lib64-stub.
lib-macos:
	$(MAKE) LIB=1 BITS=64 TARGET=macos BUILD=build/lib-macos RENDERER=hostgfx AUDIO=mus

lib-macos-stub:
	$(MAKE) LIB=1 BITS=64 TARGET=macos BUILD=build/lib-macos-stub RENDERER=null AUDIO=stub

lib-ios:
	$(MAKE) LIB=1 BITS=64 TARGET=ios BUILD=build/lib-ios RENDERER=hostgfx AUDIO=mus

lib-iossim:
	$(MAKE) LIB=1 BITS=64 TARGET=iossim BUILD=build/lib-iossim RENDERER=hostgfx AUDIO=mus

lib-xcframework: lib-ios lib-iossim
	rm -rf build/glover.xcframework
	xcodebuild -create-xcframework -framework build/lib-ios/glover.framework \
	    -framework build/lib-iossim/glover.framework -output build/glover.xcframework

# The test host (tools/libhost/libhost.c): plain C, no SDL. It loads the library at run time. The clash host also
# defines names that the game defines; the library must keep its own.
LIBHOST_FLAGS := -std=gnu99 -O2 -g -Wall -Wextra -Isrc/lib -Isrc/port
# The test host reads host scenes with the headless mode's parser.
LIBHOST_SRCS := tools/libhost/libhost.c src/port/hostscene.c
LIBHOST_DEPS := $(LIBHOST_SRCS) src/lib/glover.h src/port/hostscene.h
LIBHOST64 := -m64 $(LIBHOST_FLAGS)
LIBHOST32 := -m32 -msse2 -mfpmath=sse $(LIBHOST_FLAGS)
libhost64: build/libhost64/glover-host build/libhost64/glover-host-clash
libhost32: build/libhost32/glover-host build/libhost32/glover-host-clash

build/libhost64/glover-host: $(LIBHOST_DEPS)
	@mkdir -p $(dir $@)
	gcc $(LIBHOST64) -o $@ $(LIBHOST_SRCS) -ldl -lpthread -lm

build/libhost64/glover-host-clash: $(LIBHOST_DEPS)
	@mkdir -p $(dir $@)
	gcc $(LIBHOST64) -DCLASH -rdynamic -o $@ $(LIBHOST_SRCS) -ldl -lpthread -lm

build/libhost32/glover-host: $(LIBHOST_DEPS)
	@mkdir -p $(dir $@)
	gcc $(LIBHOST32) -o $@ $(LIBHOST_SRCS) -ldl -lpthread -lm

build/libhost32/glover-host-clash: $(LIBHOST_DEPS)
	@mkdir -p $(dir $@)
	gcc $(LIBHOST32) -DCLASH -rdynamic -o $@ $(LIBHOST_SRCS) -ldl -lpthread -lm

# The picture test (tools/libhost/glshot.c). It uses SDL for its window; the library does not.
libglshot64: build/libglshot64/glover-glshot
libglshot32: build/libglshot32/glover-glshot

build/libglshot64/glover-glshot: tools/libhost/glshot.c src/lib/glover.h src/port/hostscene.c src/port/hostscene.h
	@mkdir -p $(dir $@)
	gcc $(LIBHOST64) -I/usr/include/SDL2 -o $@ $< src/port/hostscene.c -lSDL2 -ldl -lm

# As for the 32-bit programs, SDL's configuration header comes from the 64-bit package (sdl-include, below).
build/libglshot32/glover-glshot: tools/libhost/glshot.c src/lib/glover.h src/port/hostscene.c src/port/hostscene.h
	@mkdir -p $(dir $@)sdl-include/SDL2
	@if [ -d /usr/include/x86_64-linux-gnu/SDL2 ]; then cp /usr/include/x86_64-linux-gnu/SDL2/* $(dir $@)sdl-include/SDL2; fi
	gcc $(LIBHOST32) -I$(dir $@)sdl-include -I/usr/include/SDL2 -o $@ $< src/port/hostscene.c -L/usr/lib/i386-linux-gnu -l:libSDL2-2.0.so.0 -ldl -lm

libhost-windows64: build/libhost-windows64/glover-host.exe
libhost-windows32: build/libhost-windows32/glover-host.exe

build/libhost-windows64/glover-host.exe: $(LIBHOST_DEPS)
	@mkdir -p $(dir $@)
	x86_64-w64-mingw32-gcc $(LIBHOST64) -static -o $@ $(LIBHOST_SRCS) -lm

build/libhost-windows32/glover-host.exe: $(LIBHOST_DEPS)
	@mkdir -p $(dir $@)
	i686-w64-mingw32-gcc $(LIBHOST32) -static -o $@ $(LIBHOST_SRCS) -lm

# The test host for macOS, and for the iOS Simulator (run there with xcrun simctl spawn).
libhost-macos: build/libhost-macos/glover-host
libhost-iossim: build/libhost-iossim/glover-host

build/libhost-macos/glover-host: $(LIBHOST_DEPS)
	@mkdir -p $(dir $@)
	xcrun --sdk macosx clang -target arm64-apple-macos$(MACOS_MIN) $(LIBHOST_FLAGS) -o $@ $(LIBHOST_SRCS) -lm

build/libhost-iossim/glover-host: $(LIBHOST_DEPS)
	@mkdir -p $(dir $@)
	xcrun --sdk iphonesimulator clang -target arm64-apple-ios$(IOS_MIN)-simulator $(LIBHOST_FLAGS) -o $@ \
	    $(LIBHOST_SRCS) -lm

libcheck: lib64 lib64-hostgfx lib64-stub libhost64 headless64 headless64-mus lib32 lib32-stub libhost32 headless headless-mus
	python3 tools/libcheck.py

space-demo:
	$(MAKE) BUILD=build/space-demo SPACE_DEMO=1

space-demo-windows:
	$(MAKE) TARGET=windows BUILD=build/space-demo-windows SPACE_DEMO=1

# The studio uses the stand-in sound player: the real one keeps state outside the game's memory, which a savestate
# would not restore.
studio:
	$(MAKE) BUILD=build/studio STUDIO=1 AUDIO=stub

studio-windows:
	$(MAKE) TARGET=windows BUILD=build/studio-windows STUDIO=1 AUDIO=stub

# The library's link (docs/library.md). A version script keeps every symbol but glover_* local, and -z defs makes
# sure that the library needs nothing but the C library and libm.
$(BUILD)/libglover.so: $(LIB_OBJS) src/lib/glover.map
	$(CC) $(ARCH_FLAGS) -shared -o $@ -Wl,-soname,libglover.so -Wl,--version-script=src/lib/glover.map \
	    -Wl,-z,defs,-z,noexecstack,-z,relro,-z,now -static-libgcc $(LIB_OBJS) -lm

# On Windows only the functions marked GLOVER_API (dllexport) are exported.
$(BUILD)/glover.dll: $(LIB_OBJS)
	$(CC) $(ARCH_FLAGS) -shared -o $@ -Wl,--out-implib,$(BUILD)/libglover.dll.a -static-libgcc -static $(LIB_OBJS) -lm

# Apple: the names of glover.syms are exported, with Mach-O's underscore before each; nothing else is. The library
# needs only libSystem.
$(BUILD)/glover.exports: src/lib/glover.syms
	@mkdir -p $(dir $@)
	sed 's/^/_/' $< > $@

$(BUILD)/libglover.dylib: $(LIB_OBJS) $(BUILD)/glover.exports
	$(CC) $(ARCH_FLAGS) -dynamiclib -o $@ -install_name @rpath/libglover.dylib \
	    -Wl,-exported_symbols_list,$(BUILD)/glover.exports $(LIB_OBJS) -lm

# iOS takes a library in a framework: the binary named as the framework, its Info.plist and the header.
$(BUILD)/glover.framework/glover: $(LIB_OBJS) $(BUILD)/glover.exports src/lib/Info.plist.in src/lib/glover.h
	@mkdir -p $(dir $@)Headers
	$(CC) $(ARCH_FLAGS) -dynamiclib -o $@ -install_name @rpath/glover.framework/glover \
	    -Wl,-exported_symbols_list,$(BUILD)/glover.exports $(LIB_OBJS) -lm
	sed -e 's/@PLATFORM@/$(if $(filter ios,$(TARGET)),iPhoneOS,iPhoneSimulator)/' -e 's/@MINIMUM_OS@/$(IOS_MIN)/' \
	    src/lib/Info.plist.in > $(dir $@)Info.plist
	cp src/lib/glover.h $(dir $@)Headers/

ifeq ($(STUDIO),1)
include studio.mk
else
$(EXE): $(GAME_OBJS) $(HOST_OBJS) $(RENDERER_MODE)
	$(if $(CXX_SRCS),$(CXX),$(CC)) $(ARCH_FLAGS) $(LDFLAGS) -o $@ $(filter-out $(RENDERER_MODE),$^) $(SDL_LIBS) -lm
endif

# The headers that each game object includes (-MMD): the game's headers hold inline code (Ptr32, docs/port64.md).
-include $(GAME_OBJS:.o=.d)

$(GAME_OBJS) $(HOST_OBJS): | $(BUILD)/sdl-include/SDL2/_real_SDL_config.h

$(BUILD)/sdl-include/SDL2/_real_SDL_config.h:
	@mkdir -p $(dir $@)
	@if [ -d /usr/include/x86_64-linux-gnu/SDL2 ]; then cp /usr/include/x86_64-linux-gnu/SDL2/* $(dir $@); fi
	@touch $@

$(BUILD)/src/game/%.o: src/game/%.c src/port/port_pre.h src/port/port_fixups.h src/port/ptr32.h
	@mkdir -p $(dir $@)
	$(CXX) $(GAME_CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/port/platform.o: src/port/platform.c src/port/record.h src/port/frame.h src/gfx/gfx.h src/gfx/gfx_host.h src/port/menu.h src/port/settings.h src/port/inputevent.h src/port/log.h src/port/rom.h src/port/statehash.h src/port/hostworld.h src/port/hostscene.h $(AUDIO_MODE)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD)/src/port/hostscene.o: src/port/hostscene.c src/port/hostscene.h
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD)/src/port/settings.o: src/port/settings.c src/port/settings.h
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD)/src/port/inputevent.o: src/port/inputevent.c src/port/inputevent.h src/port/settings.h
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD)/src/port/rom.o: src/port/rom.c src/port/rom.h
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD)/src/port/statehash.o: src/port/statehash.c src/port/statehash.h src/port/ptr32.h
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD)/src/port/log.o: src/port/log.c src/port/log.h src/port/frame.h
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

# The id of the source tree goes into the recordings (record.c). The file changes only when the id does.
BUILD_ID := $(shell git describe --always --dirty 2>/dev/null || echo unknown)
BUILD_ID_FILE := $(BUILD)/build-id
$(shell mkdir -p $(BUILD) && (echo $(BUILD_ID) | cmp -s - $(BUILD_ID_FILE) || echo $(BUILD_ID) > $(BUILD_ID_FILE)))

$(BUILD)/src/port/record.o: src/port/record.c src/port/record.h $(BUILD_ID_FILE) $(AUDIO_MODE)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -DPORT_BUILD_ID='"$(BUILD_ID)"' -c $< -o $@

$(BUILD)/src/lib/glover.o: src/lib/glover.c $(wildcard src/lib/*.h) src/port/frame.h src/port/drawmark.h src/port/rom.h src/port/statehash.h src/port/ptr32.h \
                          $(BUILD_ID_FILE) $(AUDIO_MODE)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -Wextra -DGLOVER_BUILD -DGLOVER_BUILD_ID='"$(BUILD_ID)"' -DGLOVER_RENDERER='"$(RENDERER)"' \
	    $(if $(APPLE_TRIPLE),-DGLOVER_APPLE_SYSTEM='"$(TARGET)-arm64"') -c $< -o $@

$(BUILD)/src/lib/%.o: src/lib/%.c $(wildcard src/lib/*.h) src/port/frame.h src/port/drawmark.h src/port/ptr32.h $(AUDIO_MODE)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -Wextra -DGLOVER_BUILD -c $< -o $@

$(BUILD)/src/port/menu.o: src/port/menu.cpp src/port/menu.h src/port/settings.h src/port/log.h src/port/record.h
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_CXXFLAGS) -c $< -o $@

$(BUILD)/src/port/menu_null.o: src/port/menu_null.c src/port/menu.h
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD)/third_party/%.o: third_party/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_CXXFLAGS) -Wno-stringop-overflow -c $< -o $@

$(BUILD)/src/port/gfx_null.o: src/port/gfx_null.c src/gfx/gfx.h src/gfx/gfx_host.h
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD)/src/gfx/%.o: src/gfx/%.c $(wildcard src/gfx/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

$(patsubst %.c,$(BUILD)/%.o,$(PORT_CXX_SRCS)): $(BUILD)/%.o: %.c src/port/port.h src/port/port_pre.h src/port/ptr32.h src/port/hostworld.h src/port/porthooks.h
	@mkdir -p $(dir $@)
	$(CXX) $(GAME_CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/src/port/%.o: src/port/%.c src/port/port.h src/port/port_pre.h src/port/ptr32.h
	@mkdir -p $(dir $@)
	$(CC) $(GAME_CFLAGS) -MMD -MP -c $< -o $@

# The sound player is game code: it keeps the N64's structure layout and its float arithmetic must match the ROM's.
# It is C compiled as C++ with C linkage, as the game's files are.
$(BUILD)/src/audio/%.o: src/audio/%.c $(wildcard src/audio/*/*.h) src/port/port_pre.h src/port/ptr32.h
	@mkdir -p $(dir $@)
	$(CXX) $(GAME_CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/gen/%.o: gen/%.c
	@mkdir -p $(dir $@)
	$(CC) $(GAME_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/gen/ram.o: gen/ram.S
	@mkdir -p $(dir $@)
	$(CC) $(ARCH_FLAGS) $(PTR_FLAGS) -c $< -o $@

GLOVEWORLD ?= ../gloveworld
# The Python for `make gen` and `make check`: gloveworld's virtual environment when it exists, or else python3. Both
# need pyelftools (docs/building.md, "The tools").
PYTHON ?= $(if $(wildcard $(GLOVEWORLD)/.venv/bin/python),$(GLOVEWORLD)/.venv/bin/python,python3)

gen:
	cd tools/gen && GLOVEWORLD=$(abspath $(GLOVEWORLD)) $(abspath $(PYTHON)) gen_types.py
	cd tools/gen && GLOVEWORLD=$(abspath $(GLOVEWORLD)) $(abspath $(PYTHON)) gen_ram.py

# The checks of the pointer representation (docs/port64.md): every slot that the loader relocates must hold a
# Ptr32, Fn32 or PortMesg, or a number; the displaced build's memory must equal the headless build's, word for word;
# a few runs of the displaced build must be the same as the regression baseline; and with the real sound player, the
# displaced build's memory must equal the headless build's too.
check: all
	$(PYTHON) tools/check_ptr_slots.py
	$(MAKE) headless
	$(MAKE) headless-displaced
	python3 tools/ptrdiff.py
	python3 tools/regress.py check --exe build/headless-displaced/weemitt --runs level05 level13 level36
	$(MAKE) headless-mus
	$(MAKE) headless-displaced-mus
	python3 tools/ptrdiff.py --audio

# The casts between pointers and 32-bit integers that a 64-bit build rejects, and the other errors of a 64-bit
# compile (docs/port64.md). It prints their count.
CHECK64_FLAGS := -m64 -fsyntax-only -fmax-errors=0 -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast \
                 $(filter-out $(ARCH_FLAGS) $(ALIGN_FLAGS) -O2 -g -w,$(GAME_FLAGS)) -DPORT_AUDIO_MUS
CHECK64_AUDIO := $(sort $(wildcard src/audio/*/*.c))
# The program's C files that do not include the game's headers: the platform layer, the renderer, the studio and
# the brute forcer.
CHECK64_HOST := $(PLATFORM_SRCS) src/port/inputlatch.c src/port/studio.c $(wildcard src/gfx/*.c) \
                $(sort $(wildcard bruteforce/engine/*.c bruteforce/src/*.c))
CHECK64_HOST_FLAGS := -std=gnu99 -m64 -fsyntax-only -fmax-errors=0 -Werror=pointer-to-int-cast \
                      -Werror=int-to-pointer-cast -DPORT_STUDIO -DSTUDIO_GAME_HASH=0 -DSTUDIO_VERSION='""' \
                      -Isrc/port -Isrc/gfx -Ibruteforce/engine -Ibruteforce/src $(SDL_CFLAGS)
check64:
	@mkdir -p build
	@for f in $(GAME_SRCS) $(PORT_CXX_SRCS); do g++ -x c++ -std=gnu++14 $(CHECK64_FLAGS) $$f 2>&1; done \
	    > build/check64.log; \
	for f in $(CHECK64_AUDIO); do g++ -x c++ -std=gnu++14 $(CHECK64_FLAGS) $$f 2>&1; done >> build/check64.log; \
	echo "check64: $$(grep -c 'error:' build/check64.log) errors (build/check64.log)"; \
	for f in $(CHECK64_HOST); do gcc $(CHECK64_HOST_FLAGS) $$f 2>&1; done > build/check64-host.log; \
	echo "check64: $$(grep -c 'error:' build/check64-host.log) errors in the host files (build/check64-host.log)"

clean:
	rm -rf build/linux build/windows build/headless build/space-demo build/space-demo-windows build/studio \
	       build/studio-windows build/headless-displaced build/headless-mus build/headless-displaced-mus \
	       build/linux64 build/windows64 build/headless64 build/headless64-mus build/lib64 build/lib64-hostgfx build/lib64-stub build/libhost64 \
	       build/lib-windows64 build/lib-windows64-stub build/libhost-windows64 build/libglshot64 build/lib32 \
	       build/lib32-stub build/libhost32 build/lib-windows32 build/lib-windows32-stub build/libhost-windows32 \
	       build/libglshot32 build/lib-macos build/lib-macos-stub build/lib-ios build/lib-iossim build/glover.xcframework \
	       build/libhost-macos build/libhost-iossim

.PHONY: all windows headless headless-displaced headless-mus headless-displaced-mus linux64 windows64 headless64 headless64-mus space-demo space-demo-windows studio studio-windows gen check check64 clean lib64 lib64-hostgfx lib64-stub libhost64 libcheck \
        lib-windows64 lib-windows64-stub libhost-windows64 libglshot64 lib32 lib32-stub libhost32 lib-windows32 \
        lib-windows32-stub libhost-windows32 libglshot32 lib-macos lib-macos-stub lib-ios lib-iossim lib-xcframework \
        libhost-macos libhost-iossim
