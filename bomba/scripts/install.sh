#!/bin/bash
# Copies the build in ./dist into an install (BOMBA_INSTALL, else
# F:\RetroArch-Bomba-GUI): the program (app\, bomba.exe). The user's own
# files there (settings, retroarch.cfg, saves, roms...) are left alone; the
# empty settings and config are only put in when missing.
export MSYSTEM=MINGW64
source /etc/profile
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIST="$ROOT/dist"
INSTALL="$(cygpath -u "${BOMBA_INSTALL:-F:\\RetroArch-Bomba-GUI}")"

if [ ! -f "$DIST/app/retroarch.exe" ]; then
   echo "Nothing built yet: run bomba/scripts/build.sh first." >&2
   exit 1
fi
if tasklist //FI "IMAGENAME eq retroarch.exe" | grep -qi retroarch.exe; then
   echo "RetroArch is running; close it and run this again." >&2
   exit 1
fi

mkdir -p "$INSTALL/app"
for f in "$DIST/app"/*; do
   [ "$(basename "$f")" = "retroarch.cfg" ] && continue
   cp -r "$f" "$INSTALL/app/"
done
cp -n "$DIST/app/retroarch.cfg" "$INSTALL/app/"
cp "$DIST/bomba.exe" "$INSTALL/"
cp -n "$DIST/bomba-settings.ini" "$INSTALL/"
echo "Installed into $INSTALL"
