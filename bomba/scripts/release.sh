#!/bin/bash
# Packages the build in ./dist as a Bomba GUI release, what the Downloads
# screen's "Bomba GUI" row installs: release/bomba-gui-vX.Y.Z.zip, X.Y.Z
# being BOMBA_VERSION (bomba/bomba_version.h). Steam's files (mist.dll,
# mist\, steam_appid.txt) stay out: they aren't ours to hand out, and the
# installs being updated already have them.
# With --publish, also makes the GitHub release "vX.Y.Z" on origin, at the
# current commit, with the zip attached (the GitHub login comes from git's
# credential helper, the one git push uses).
# Bump BOMBA_VERSION and run build.sh first.
#   F:\msys64\usr\bin\bash.exe -l bomba/scripts/release.sh [--publish]
export MSYSTEM=MINGW64
source /etc/profile
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIST="$ROOT/dist"
OUT="$ROOT/release"
REPO="Slazmaz/RetroArch-Bomba-GUI"

cd "$ROOT"

version=$(sed -n 's/^#define BOMBA_VERSION *"\([0-9]*\.[0-9]*\.[0-9]*\)".*/\1/p' \
   bomba/bomba_version.h)
if [ -z "$version" ]; then
   echo "No major.minor.patch BOMBA_VERSION in bomba/bomba_version.h." >&2
   exit 1
fi
tag="v$version"
zip="$OUT/bomba-gui-$tag.zip"

if [ ! -f "$DIST/app/retroarch.exe" ]; then
   echo "Nothing built yet: run bomba/scripts/build.sh first." >&2
   exit 1
fi
# A build of another version would be offered the same update forever.
if ! grep -aq "Bomba GUI $tag" "$DIST/app/retroarch.exe"; then
   echo "dist isn't a $tag build: run bomba/scripts/build.sh first." >&2
   exit 1
fi

mkdir -p "$OUT"
rm -f "$zip"
# The README (GitHub's, .github/README.md) goes in too, its links into
# the repo made full GitHub links at this commit.
python3 - "$DIST" "$zip" "$ROOT/.github/README.md" \
   "https://github.com/$REPO" "$(git rev-parse HEAD)" <<'EOF'
import os, re, sys, zipfile
dist, out, readme, repo, commit = sys.argv[1:6]
skip = {"app/mist.dll", "app/steam_appid.txt"}
with open(readme, encoding="utf-8") as f:
    text = f.read()
text = re.sub(r"(!\[[^\]]*\]\()\.\./", r"\g<1>%s/raw/%s/" % (repo, commit), text)
text = re.sub(r"(\]\()\.\./", r"\g<1>%s/blob/%s/" % (repo, commit), text)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("README.md", text)
    for top, dirs, files in os.walk(dist):
        rel_top = os.path.relpath(top, dist).replace(os.sep, "/")
        if rel_top == "app/mist" or rel_top.startswith("app/mist/"):
            continue
        for f in sorted(files):
            rel = f if rel_top == "." else rel_top + "/" + f
            if rel not in skip:
                z.write(os.path.join(top, f), rel)
EOF
echo "Packaged: $zip"

[ "$1" = "--publish" ] || exit 0

# Published releases are installed by everyone: only committed, pushed code.
if [ -n "$(git status --porcelain)" ]; then
   echo "Uncommitted changes; commit and push them first." >&2
   exit 1
fi
commit=$(git rev-parse HEAD)
git fetch -q origin
if ! git merge-base --is-ancestor "$commit" origin/master; then
   echo "HEAD isn't on origin/master; push it first." >&2
   exit 1
fi
# origin also has RetroArch's own tags (v1.9.0...); a release must not
# land on one of those.
if git ls-remote --exit-code --tags origin "refs/tags/$tag" > /dev/null; then
   echo "origin already has a tag $tag; bump BOMBA_VERSION." >&2
   exit 1
fi

token=$(printf 'protocol=https\nhost=github.com\n\n' | git credential fill \
   | sed -n 's/^password=//p')
if [ -z "$token" ]; then
   echo "No GitHub login from git's credential helper." >&2
   exit 1
fi
auth=(-H "Authorization: Bearer $token" -H "Accept: application/vnd.github+json")

release=$(curl -sS --fail-with-body "${auth[@]}" \
   -d "{\"tag_name\":\"$tag\",\"target_commitish\":\"$commit\",\"name\":\"Bomba GUI $tag\"}" \
   "https://api.github.com/repos/$REPO/releases")
id=$(printf '%s' "$release" | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')

curl -sS --fail-with-body "${auth[@]}" -H "Content-Type: application/zip" \
   --data-binary "@$zip" \
   "https://uploads.github.com/repos/$REPO/releases/$id/assets?name=$(basename "$zip")" \
   > /dev/null
git fetch -q origin "refs/tags/$tag:refs/tags/$tag"
echo "Published: https://github.com/$REPO/releases/tag/$tag"
