#!/bin/bash
# Moves the custom RetroArch fork onto the revision Steam currently ships,
# replaying our commits (branch "custom") on top, then rebuilds.
#
# The tag "upstream" marks the RetroArch revision our commits sit on.
export MSYSTEM=MINGW64
source /etc/profile
set -e

# The rebase below briefly removes this script from src, so run from a
# temp copy. ROOT is the RetroArchWrapper folder (script is in src/wrapper/scripts).
if [ -z "$RAWRAPPER_ROOT" ]; then
   export RAWRAPPER_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
   copy="$(mktemp --suffix=.sh)"
   cp "$0" "$copy"
   exec bash "$copy" "$@"
fi
trap 'rm -f "$0"' EXIT

ROOT="$RAWRAPPER_ROOT"
STEAM_RA="$ROOT/../RetroArch"

cd "$ROOT/src"

new_rev=$(awk '/^Rev:/ {print $2}' "$STEAM_RA/build.txt")
old_rev=$(git rev-parse upstream)

if [ -z "$new_rev" ]; then
   echo "Could not read Rev from $STEAM_RA/build.txt" >&2
   exit 1
fi

if [ -n "$(git status --porcelain)" ]; then
   echo "src has uncommitted changes. Commit them to the 'custom' branch first." >&2
   git status --short >&2
   exit 1
fi

if [ "$new_rev" != "$old_rev" ]; then
   echo "Updating ${old_rev:0:7} -> ${new_rev:0:7}"
   git fetch --depth 1 origin "$new_rev"

   if ! git rebase --onto "$new_rev" upstream custom; then
      git rebase --abort
      echo "Our changes conflict with the new RetroArch version." >&2
      echo "Nothing was changed; the current build keeps working." >&2
      exit 1
   fi

   git tag -f upstream "$new_rev" > /dev/null
   "$ROOT/src/wrapper/scripts/build.sh" --reconfigure
else
   echo "Source already at ${new_rev:0:7}; rebuilding."
   "$ROOT/src/wrapper/scripts/build.sh"
fi
