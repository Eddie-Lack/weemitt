#!/bin/sh
# Builds SDL2 for Windows with MinGW into deps/SDL2-mingw, for `make windows` (32-bit) or `make windows64`.
#
#   tools/build_sdl_mingw.sh            32-bit: deps/SDL2-mingw/i686-w64-mingw32
#   tools/build_sdl_mingw.sh x86_64     64-bit: deps/SDL2-mingw/x86_64-w64-mingw32
#
# Needs git, cmake, ninja and the MinGW compiler of the architecture (Debian/Ubuntu: mingw-w64 cmake ninja-build).
set -e
cd "$(dirname "$0")/.."
ARCH=${1:-i686}
case "$ARCH" in
    i686) BUILDDIR=deps/SDL2-build ;;
    x86_64) BUILDDIR=deps/SDL2-build-x86_64 ;;
    *) echo "usage: $0 [i686|x86_64]" >&2; exit 1 ;;
esac
HOST=$ARCH-w64-mingw32
mkdir -p deps
[ -d deps/SDL2-src ] || git clone -q --depth 1 --branch release-2.30.x https://github.com/libsdl-org/SDL.git deps/SDL2-src
cat > deps/mingw-$ARCH.cmake <<EOT
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR $ARCH)
set(CMAKE_C_COMPILER $HOST-gcc)
set(CMAKE_CXX_COMPILER $HOST-g++)
set(CMAKE_RC_COMPILER $HOST-windres)
set(CMAKE_FIND_ROOT_PATH /usr/$HOST)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOT
cmake -S deps/SDL2-src -B $BUILDDIR -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/deps/mingw-$ARCH.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/deps/SDL2-mingw/$HOST" \
    -DSDL_SHARED=ON -DSDL_STATIC=ON -DSDL_TEST=OFF
ninja -C $BUILDDIR install
echo "SDL2 for Windows is in deps/SDL2-mingw. Copy deps/SDL2-mingw/$HOST/bin/SDL2.dll next to the program."
