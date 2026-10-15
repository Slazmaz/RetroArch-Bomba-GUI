#!/bin/bash
# Builds the custom RetroArch fork and stages it into ./build.
# Run from an MSYS2 MINGW64 shell, or:
#   F:\msys64\usr\bin\bash.exe -l wrapper/scripts/build.sh
export MSYSTEM=MINGW64
source /etc/profile
set -e

# This script lives in wrapper/scripts; ROOT is the repo root.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
STEAM_RA="$ROOT/../../RetroArch"
export MIST_PATH="$ROOT/mist-sdk"

cd "$ROOT"

# Same flags as Steam's build.txt, minus the MXE --host triplet.
if [ ! -f config.mk ] || [ "$1" = "--reconfigure" ]; then
   ./configure --disable-qt --enable-steam --enable-mist
fi

make -j"$(nproc)"

if tasklist //FI "IMAGENAME eq retroarch.exe" | grep -qi retroarch.exe; then
   echo "RetroArch is running; close it and run this again to install the build." >&2
   exit 1
fi

mkdir -p "$ROOT/build"
cp retroarch.exe "$ROOT/build/"
# Assets the fork's wrapper code loads from next to the exe (fonts, icons).
mkdir -p "$ROOT/build/wrapper/fonts" "$ROOT/build/wrapper/icons"
cp -u wrapper/fonts/* "$ROOT/build/wrapper/fonts/"
cp -u wrapper/icons/* "$ROOT/build/wrapper/icons/"

# Launcher cores, built separately from RetroArch's makefile.
mkdir -p "$ROOT/build/cores" "$ROOT/build/info"
gcc -O2 -Wall -shared -static-libgcc -I"$ROOT/libretro-common/include" \
   -o "$ROOT/build/cores/eden_libretro.dll" wrapper/cores/eden/eden_libretro.c -ldwmapi -lgdi32 -lole32
gcc -O2 -Wall -static-libgcc -o "$ROOT/build/cores/eden_padinfo.exe" wrapper/cores/eden/eden_padinfo.c
cp wrapper/cores/eden/eden_libretro.info "$ROOT/build/info/"

# Upstream revision this build is based on; the wrapper compares it to Steam's.
git rev-parse upstream > "$ROOT/build/build-rev.txt"

# Copy MinGW runtime DLLs the exe depends on (skip Windows system DLLs).
# Repeat until ldd resolves nothing from /mingw64, to catch nested deps.
cd "$ROOT/build"
for _ in 1 2 3 4 5; do
   missing=$(ldd ./retroarch.exe | awk '/\/mingw64\// {print $3}' | sort -u)
   [ -z "$missing" ] && break
   for dll in $missing; do cp -u "$dll" "$ROOT/build/"; done
done

# mist.dll + steam_appid.txt come from Steam's install.
cp -u "$STEAM_RA/mist.dll" "$STEAM_RA/steam_appid.txt" "$ROOT/build/"

echo "Built: $ROOT/build/retroarch.exe"
