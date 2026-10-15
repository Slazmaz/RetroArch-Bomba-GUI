# Sourced by build.sh and update.sh: sets STEAM_RA to Steam's RetroArch
# install (its mist, steam_appid.txt and build.txt), found through Steam's
# library list. BOMBA_STEAM_RA overrides it.
steam_ra()
{
   local steam lib

   if [ -n "$BOMBA_STEAM_RA" ]; then
      STEAM_RA="$BOMBA_STEAM_RA"
      return
   fi

   steam=$(reg query 'HKCU\Software\Valve\Steam' //v SteamPath 2>/dev/null \
      | awk '/SteamPath/ {print $3}')
   if [ -n "$steam" ]; then
      steam=$(cygpath -u "$steam")
      # Every library folder ("path" lines), the main one included.
      while read -r lib; do
         lib=$(cygpath -u "${lib//\\\\/\\}")
         if [ -f "$lib/steamapps/common/RetroArch/retroarch.exe" ]; then
            STEAM_RA="$lib/steamapps/common/RetroArch"
            return
         fi
      done < <(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"/\1/p' \
         "$steam/steamapps/libraryfolders.vdf" 2>/dev/null; echo "$steam")
   fi

   echo "Steam's RetroArch not found; set BOMBA_STEAM_RA to its folder." >&2
   exit 1
}
steam_ra
