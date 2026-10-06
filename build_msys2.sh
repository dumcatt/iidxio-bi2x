#!/bin/bash
# Build with MSYS2 (MinGW-w64). Started by build_msys2.bat (64-bit) or
# build_msys2_32.bat (32-bit). Usage: build_msys2.sh [64|32]
cd "$(dirname "$0")" || exit 1
arch=${1:-64}

if [ "$arch" = 32 ]; then
    builddir=build32
else
    builddir=build
fi

exec > >(tee "$builddir.log") 2>&1

echo "== $arch-bit build started $(date)"

prefix=
if [ "$arch" = 32 ]; then
    candidates=mingw32
else
    candidates="ucrt64 mingw64"
fi

for p in $candidates; do
    if [ -x "/$p/bin/gcc.exe" ] && [ -x "/$p/bin/cmake.exe" ]; then
        prefix=$p
        break
    fi
done

if [ -z "$prefix" ]; then
    if [ "$arch" = 32 ]; then
        echo "== no 32-bit MinGW gcc+cmake found, installing MINGW32 toolchain"
        pacman -S --needed --noconfirm mingw-w64-i686-gcc \
            mingw-w64-i686-cmake mingw-w64-i686-ninja || exit 1
        prefix=mingw32
    else
        echo "== no MinGW gcc+cmake found, installing UCRT64 toolchain"
        pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc \
            mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja || exit 1
        prefix=ucrt64
    fi
fi

export PATH="/$prefix/bin:/usr/bin:$PATH"
echo "== toolchain: $prefix"
gcc --version | head -1
cmake --version | head -1

if [ ! -f bemanitools/src/main/bemanitools/iidxio.h ]; then
    echo "== fetching bemanitools headers"
    command -v git > /dev/null || pacman -S --needed --noconfirm git || exit 1
    rm -rf bemanitools.tmp
    git clone --depth 1 https://github.com/djhackersdev/bemanitools.git bemanitools.tmp || exit 1
    mkdir -p bemanitools
    cp -r bemanitools.tmp/. bemanitools/
    rm -rf bemanitools.tmp
fi
find bemanitools -name iidxio.h -o -name glue.h | head

gen="MinGW Makefiles"
command -v ninja > /dev/null && gen=Ninja

rm -rf "$builddir"
cmake -S . -B "$builddir" -G "$gen" -DCMAKE_BUILD_TYPE=Release || { echo "BUILD FINISHED rc=1"; exit 1; }
cmake --build "$builddir"
rc=$?

if [ $rc -eq 0 ]; then
    echo "== artifacts"
    ls -la "$builddir"/*.exe "$builddir"/*.dll 2>/dev/null
    echo "== dependency check (should only list Windows system DLLs)"
    for f in "$builddir"/*.exe "$builddir"/*.dll; do
        echo "-- $f"; objdump -f "$f" | grep format; objdump -p "$f" | grep "DLL Name"
    done
    echo "== module extraction test"
    "$builddir"/bi2x-modextract.exe extern/dlls "$builddir"
    rc=$?
fi

echo "BUILD FINISHED rc=$rc"
