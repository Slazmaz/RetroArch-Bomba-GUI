#!/bin/bash
# Builds the custom RetroArch fork and stages it into ./dist, laid out as
# an install: bomba.exe (the launcher) and app\ with the program (exe, its DLLs, mist, Bomba's assets, launcher
# cores) and a template retroarch.cfg. No user data. dist is what gets
# distributed, and what install.sh copies into an install.
# Run from an MSYS2 MINGW64 shell, or:
#   F:\msys64\usr\bin\bash.exe -l bomba/scripts/build.sh
export MSYSTEM=MINGW64
source /etc/profile
set -e

# This script lives in bomba/scripts; ROOT is the repo root.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIST="$ROOT/dist"
APP="$DIST/app"
source "$ROOT/bomba/scripts/steam_ra.sh"
# mist headers are vendored in bomba/mist; mist.dll is loaded at runtime
# (steam/mist_load.c), not linked.
export MIST_PATH="$ROOT/bomba/mist"

cd "$ROOT"

# Same flags as Steam's build.txt, minus the MXE --host triplet.
if [ ! -f config.mk ] || [ "$1" = "--reconfigure" ]; then
   ./configure --disable-qt --enable-steam --enable-mist
fi

make -j"$(nproc)" MIST_PATH="$MIST_PATH"

rm -rf "$DIST"
mkdir -p "$APP"
cp retroarch.exe "$APP/"
# Assets Bomba's code loads from next to the exe (fonts, icons).
mkdir -p "$APP/bomba/fonts" "$APP/bomba/icons"
cp bomba/fonts/* "$APP/bomba/fonts/"
cp bomba/icons/* "$APP/bomba/icons/"

# Launcher cores, built separately from RetroArch's makefile.
mkdir -p "$APP/cores" "$APP/info"
gcc -O2 -Wall -shared -static-libgcc -I"$ROOT/libretro-common/include" \
   -o "$APP/cores/eden_libretro.dll" bomba/cores/eden/eden_libretro.c -ldwmapi -lgdi32 -lole32
gcc -O2 -Wall -static-libgcc -o "$APP/cores/eden_padinfo.exe" bomba/cores/eden/eden_padinfo.c
cp bomba/cores/eden/eden_libretro.info "$APP/info/"

# bomba.exe (started by hand or by Steam's launch option) runs launch.ps1;
# RetroArch's icon.
windres -I bomba/launcher bomba/launcher/bomba_launcher.rc \
   -O coff -o "$ROOT/obj-unix/bomba_launcher.res"
gcc -O2 -Wall -municode -mwindows -static-libgcc -o "$DIST/bomba.exe" \
   bomba/launcher/bomba_launcher.c "$ROOT/obj-unix/bomba_launcher.res"
cp bomba/scripts/launch.ps1 "$APP/"
# RetroArch's config, empty, pointing at the install's top folders.
cp bomba/retroarch.cfg "$APP/"

# Upstream revision this build is based on; the launch script compares it to Steam's.
git rev-parse refs/tags/upstream > "$APP/build-rev.txt"

# Copy the MinGW DLLs the exe depends on, and theirs: every imported DLL
# that /mingw64/bin has (Windows' own aren't there). Read from the import
# tables, not ldd, which reports a DLL found anywhere on PATH as found.
deps()
{
   objdump -p "$1" | awk '/DLL Name:/ {print $3}'
}
queue=$(deps "$APP/retroarch.exe")
while [ -n "$queue" ]; do
   next=""
   for dll in $queue; do
      src="/mingw64/bin/$dll"
      if [ -f "$src" ] && [ ! -f "$APP/$dll" ]; then
         cp "$src" "$APP/"
         next="$next $(deps "$src")"
      fi
   done
   queue="$next"
done

# mist (Steam's helper process) + steam_appid.txt come from Steam's install.
cp "$STEAM_RA/mist.dll" "$STEAM_RA/steam_appid.txt" "$APP/"
mkdir -p "$APP/mist"
cp -r "$STEAM_RA/mist/." "$APP/mist/"

echo "Built: $DIST (install it with bomba/scripts/install.cmd)"
