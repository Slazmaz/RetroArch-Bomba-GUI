#!/bin/bash
# Moves the custom RetroArch fork onto the revision Steam currently ships,
# replaying our commits (branch "master") on top, then rebuilds.
#
# The tag "upstream" marks the RetroArch revision our commits sit on.
export MSYSTEM=MINGW64
source /etc/profile
set -e

# The rebase below briefly removes this script from the tree, so run from a
# temp copy. ROOT is the repo root (script is in bomba/scripts).
if [ -z "$BOMBA_ROOT" ]; then
   export BOMBA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
   copy="$(mktemp --suffix=.sh)"
   cp "$0" "$copy"
   exec bash "$copy" "$@"
fi
trap 'rm -f "$0"' EXIT

ROOT="$BOMBA_ROOT"
STEAM_RA="$ROOT/../RetroArch"

cd "$ROOT"

new_rev=$(awk '/^Rev:/ {print $2}' "$STEAM_RA/build.txt")
old_rev=$(git rev-parse upstream)

if [ -z "$new_rev" ]; then
   echo "Could not read Rev from $STEAM_RA/build.txt" >&2
   exit 1
fi

if [ -n "$(git status --porcelain)" ]; then
   echo "Uncommitted changes. Commit them to the 'master' branch first." >&2
   git status --short >&2
   exit 1
fi

if [ "$new_rev" != "$old_rev" ]; then
   echo "Updating ${old_rev:0:7} -> ${new_rev:0:7}"
   git fetch --depth 1 origin "$new_rev"

   if ! git rebase --onto "$new_rev" upstream master; then
      git rebase --abort
      echo "Our changes conflict with the new RetroArch version." >&2
      echo "Nothing was changed; the current build keeps working." >&2
      exit 1
   fi

   git tag -f upstream "$new_rev" > /dev/null
   "$ROOT/bomba/scripts/build.sh" --reconfigure
else
   echo "Source already at ${new_rev:0:7}; rebuilding."
   "$ROOT/bomba/scripts/build.sh"
fi
