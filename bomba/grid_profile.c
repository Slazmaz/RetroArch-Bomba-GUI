/* Bomba: player profiles, each with its own saves.
 *
 * A profile decides where games load and save: each profile uses a
 * sub-folder named after it inside RetroArch's save and savestate
 * directories. The list lives in
 *    <retroarch.exe dir>/bomba/profiles.txt
 * together with the base directories, since retroarch.cfg ends up holding
 * whichever profile's folders were active when it was saved.
 *
 * A profile only changes saves: emulators that keep their settings next
 * to their saves have that data in one shared place, with just their
 * save folders linked to the current profile's (see profile_link_emus).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

#include <compat/strl.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "grid_profile.h"
#include "../configuration.h"
#include "../defaults.h"
#include "../paths.h"
#include "../runloop.h"
#include "../verbosity.h"

static struct
{
   char names[GRID_PROFILE_MAX][GRID_PROFILE_NAME_LEN];
   char icons[GRID_PROFILE_MAX][PATH_MAX_LENGTH];
   unsigned count;
   unsigned current;
   char base_save[PATH_MAX_LENGTH];
   char base_state[PATH_MAX_LENGTH];
   bool loaded;
} profile_st;

static bool profile_game_running(void)
{
   return (runloop_state_get_ptr()->current_core.flags
         & RETRO_CORE_FLAG_GAME_LOADED) ? true : false;
}

static void profile_file_path(char *s, size_t len)
{
   char app_dir[PATH_MAX_LENGTH];
   fill_pathname_application_dir(app_dir, sizeof(app_dir));
   fill_pathname_join_special(s, app_dir, "bomba/profiles.txt", len);
}

static void profile_save(void)
{
   unsigned i;
   char path[PATH_MAX_LENGTH];
   RFILE *f;

   profile_file_path(path, sizeof(path));
   if (!(f = filestream_open(path, RETRO_VFS_FILE_ACCESS_WRITE,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      return;

   filestream_printf(f, "base_save\t%s\n",  profile_st.base_save);
   filestream_printf(f, "base_state\t%s\n", profile_st.base_state);
   filestream_printf(f, "current\t%u\n",    profile_st.current);
   /* profile \t name \t picture (may be empty) */
   for (i = 0; i < profile_st.count; i++)
      filestream_printf(f, "profile\t%s\t%s\n", profile_st.names[i],
            profile_st.icons[i]);

   filestream_close(f);
}

/* Directory a profile uses, given RetroArch's base directory (or, if that
 * is empty, a folder next to the exe). */
static void profile_dir(unsigned idx, const char *base, const char *fallback,
      char *s, size_t len)
{
   char app_dir[PATH_MAX_LENGTH];
   char root[PATH_MAX_LENGTH];

   if (!string_is_empty(base))
      strlcpy(root, base, sizeof(root));
   else
   {
      fill_pathname_application_dir(app_dir, sizeof(app_dir));
      fill_pathname_join_special(root, app_dir, fallback, sizeof(root));
   }

   fill_pathname_join_special(s, root, profile_st.names[idx], len);
}

/* Same directory, ignoring case, slash style, a trailing slash and ".."
 * (":\..\saves" from the cfg is "<app>\..\saves", the base "<top>\saves"). */
static bool profile_same_dir(const char *a_in, const char *b_in)
{
   char a_buf[PATH_MAX_LENGTH];
   char b_buf[PATH_MAX_LENGTH];
   const char *a = a_buf;
   const char *b = b_buf;

   strlcpy(a_buf, a_in, sizeof(a_buf));
   strlcpy(b_buf, b_in, sizeof(b_buf));
   path_resolve_realpath(a_buf, sizeof(a_buf), false);
   path_resolve_realpath(b_buf, sizeof(b_buf), false);

   for (;; a++, b++)
   {
      char ca = *a == '/' ? '\\' : *a;
      char cb = *b == '/' ? '\\' : *b;
      if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
      if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
      if (ca != cb)
         return (!ca && cb == '\\' && !b[1]) || (!cb && ca == '\\' && !a[1]);
      if (!ca)
         return true;
   }
}

#ifdef _WIN32
static bool profile_wide(const char *in, wchar_t *out, int len)
{
   wchar_t *p;
   if (!MultiByteToWideChar(CP_UTF8, 0, in, -1, out, len))
      return false;
   for (p = out; *p; p++)
      if (*p == L'/')
         *p = L'\\';
   return true;
}

/* Makes the folder link a junction to the folder target, replacing an
 * older junction or an empty folder there. A folder with files in it is
 * left alone (false). */
static bool profile_junction(const char *link, const char *target)
{
   struct
   {
      DWORD tag;
      WORD  len, reserved;
      WORD  sub_off, sub_len, print_off, print_len;
      WCHAR buf[2 * PATH_MAX_LENGTH + 8];
   } rp;
   wchar_t wl[PATH_MAX_LENGTH], wt[PATH_MAX_LENGTH];
   size_t tl;
   DWORD attr, bytes;
   HANDLE h;
   BOOL ok;

   if (     !profile_wide(link, wl, PATH_MAX_LENGTH)
         || !profile_wide(target, wt, PATH_MAX_LENGTH))
      return false;
   tl = wcslen(wt);
   while (tl > 3 && wt[tl - 1] == L'\\') /* no trailing slash, but "F:\" */
      wt[--tl] = L'\0';

   /* RemoveDirectory takes away only a junction, not what it points to,
    * and fails on a folder that isn't empty. */
   attr = GetFileAttributesW(wl);
   if (attr != INVALID_FILE_ATTRIBUTES)
   {
      if (!(attr & FILE_ATTRIBUTE_DIRECTORY) || !RemoveDirectoryW(wl))
         return false;
   }
   if (!CreateDirectoryW(wl, NULL))
      return false;

   /* Mount point data: "\??\<target>" then the plain path, each ending
    * in a NUL that the lengths leave out. */
   memset(&rp, 0, sizeof(rp));
   rp.tag       = IO_REPARSE_TAG_MOUNT_POINT;
   rp.sub_off   = 0;
   rp.sub_len   = (WORD)((tl + 4) * sizeof(WCHAR));
   rp.print_off = (WORD)(rp.sub_len + sizeof(WCHAR));
   rp.print_len = (WORD)(tl * sizeof(WCHAR));
   memcpy(rp.buf, L"\\??\\", 4 * sizeof(WCHAR));
   memcpy(rp.buf + 4, wt, (tl + 1) * sizeof(WCHAR));
   memcpy(rp.buf + tl + 5, wt, (tl + 1) * sizeof(WCHAR));
   rp.len       = (WORD)(8 + rp.print_off + rp.print_len + sizeof(WCHAR));

   h = CreateFileW(wl, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
   if (h == INVALID_HANDLE_VALUE)
   {
      RemoveDirectoryW(wl);
      return false;
   }
   ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &rp, 8 + rp.len,
         NULL, 0, &bytes, NULL);
   CloseHandle(h);
   if (!ok)
      RemoveDirectoryW(wl);
   return ok ? true : false;
}

/* Links <dir>/<link> to <target_dir>/<target>, creating the target and
 * the link's parent folder first. */
static void profile_link(const char *dir, const char *link,
      const char *target_dir, const char *target)
{
   char l[PATH_MAX_LENGTH], t[PATH_MAX_LENGTH], parent[PATH_MAX_LENGTH];

   fill_pathname_join_special(l, dir, link, sizeof(l));
   fill_pathname_join_special(t, target_dir, target, sizeof(t));
   path_mkdir(t);
   fill_pathname_basedir(parent, l, sizeof(parent));
   path_mkdir(parent);
   if (!profile_junction(l, t))
      RARCH_WARN("[Profile] Couldn't link \"%s\" to \"%s\".\n", l, t);
}
#endif

/* Emulators keeping their settings among their saves (cores that can't
 * be told apart): their data is shared by all profiles, in the system
 * folder, with just the save folders in it linked to the profile's. */
static void profile_link_emus(const char *save_dir)
{
#ifdef _WIN32
   static const char *gc_regions[] = { "USA", "EUR", "JAP" };
   settings_t *settings = config_get_ptr();
   char sys[PATH_MAX_LENGTH], dolphin[PATH_MAX_LENGTH];
   char link[64], target[64];
   unsigned i;

   if (string_is_empty(save_dir))
      return;

   if (!string_is_empty(settings->paths.directory_system))
      strlcpy(sys, settings->paths.directory_system, sizeof(sys));
   else
   {
      char app_dir[PATH_MAX_LENGTH];
      fill_pathname_application_dir(app_dir, sizeof(app_dir));
      fill_pathname_join_special(sys, app_dir, "system", sizeof(sys));
   }

   /* Dolphin: <saves>/dolphin-emu/User. Shared: <system>/dolphin-emu/User,
    * with its memory cards and Wii game saves (title 00010000; 00000001
    * is the Wii's own software) the profile's. */
   fill_pathname_join_special(dolphin, sys, "dolphin-emu", sizeof(dolphin));
   profile_link(save_dir, "dolphin-emu", sys, "dolphin-emu");
   for (i = 0; i < sizeof(gc_regions) / sizeof(gc_regions[0]); i++)
   {
      snprintf(link,   sizeof(link),   "User/GC/%s",  gc_regions[i]);
      snprintf(target, sizeof(target), "GameCube/%s", gc_regions[i]);
      profile_link(dolphin, link, save_dir, target);
   }
   profile_link(dolphin, "User/Wii/title/00010000", save_dir, "Wii");
#endif
}

/* Points RetroArch's save directories at the current profile. */
static void profile_apply(void)
{
   char dir[PATH_MAX_LENGTH];

   profile_dir(profile_st.current, profile_st.base_save, "saves",
         dir, sizeof(dir));
   if (!string_is_empty(dir))
      path_mkdir(dir);
   dir_set(RARCH_DIR_SAVEFILE, dir);
   profile_link_emus(dir);

   profile_dir(profile_st.current, profile_st.base_state, "states",
         dir, sizeof(dir));
   if (!string_is_empty(dir))
      path_mkdir(dir);
   dir_set(RARCH_DIR_SAVESTATE, dir);
}

void grid_profile_init(void)
{
   char path[PATH_MAX_LENGTH];
   char line[PATH_MAX_LENGTH + 32];
   bool have_base = false;
   RFILE *f;

   if (profile_st.loaded)
      return;
   profile_st.loaded = true;

   profile_file_path(path, sizeof(path));
   if ((f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
   {
      while (filestream_gets(f, line, sizeof(line)))
      {
         char *val = strchr(line, '\t');

         line[strcspn(line, "\r\n")] = '\0';
         if (!val)
            continue;
         *val++ = '\0';

         if (string_is_equal(line, "base_save"))
         {
            strlcpy(profile_st.base_save, val, sizeof(profile_st.base_save));
            have_base = true;
         }
         else if (string_is_equal(line, "base_state"))
            strlcpy(profile_st.base_state, val, sizeof(profile_st.base_state));
         else if (string_is_equal(line, "current"))
            profile_st.current = (unsigned)strtoul(val, NULL, 10);
         else if (string_is_equal(line, "profile")
               && profile_st.count < GRID_PROFILE_MAX
               && !string_is_empty(val))
         {
            char *icon = strchr(val, '\t');
            if (icon)
               *icon++ = '\0';
            strlcpy(profile_st.names[profile_st.count], val,
                  GRID_PROFILE_NAME_LEN);
            strlcpy(profile_st.icons[profile_st.count], icon ? icon : "",
                  sizeof(profile_st.icons[0]));
            profile_st.count++;
         }
      }
      filestream_close(f);
   }

   if (profile_st.count == 0)
      strlcpy(profile_st.names[profile_st.count++], GRID_PROFILE_DEFAULT,
            GRID_PROFILE_NAME_LEN);
   if (profile_st.current >= profile_st.count)
      profile_st.current = 0;

   /* First run: today's directories become the base ones. */
   if (!have_base)
   {
      strlcpy(profile_st.base_save, dir_get_ptr(RARCH_DIR_SAVEFILE),
            sizeof(profile_st.base_save));
      strlcpy(profile_st.base_state, dir_get_ptr(RARCH_DIR_SAVESTATE),
            sizeof(profile_st.base_state));
   }
   else
   {
      /* RetroArch's directory isn't the current profile's folder: it was
       * changed in Settings since, so it becomes the new base. */
      char dir[PATH_MAX_LENGTH];
      const char *cur = dir_get_ptr(RARCH_DIR_SAVEFILE);

      profile_dir(profile_st.current, profile_st.base_save, "saves",
            dir, sizeof(dir));
      if (!string_is_empty(cur) && !profile_same_dir(cur, dir))
         strlcpy(profile_st.base_save, cur, sizeof(profile_st.base_save));

      cur = dir_get_ptr(RARCH_DIR_SAVESTATE);
      profile_dir(profile_st.current, profile_st.base_state, "states",
            dir, sizeof(dir));
      if (!string_is_empty(cur) && !profile_same_dir(cur, dir))
         strlcpy(profile_st.base_state, cur, sizeof(profile_st.base_state));
   }

   profile_apply();
   profile_save();
}

unsigned grid_profile_count(void)   { return profile_st.count; }
unsigned grid_profile_current(void) { return profile_st.current; }

const char *grid_profile_name(unsigned idx)
{
   return idx < profile_st.count ? profile_st.names[idx] : "";
}

uint32_t grid_profile_color(unsigned idx)
{
   static const uint32_t colors[] = {
      0xe0533dff, 0x3d8be0ff, 0x4cb860ff, 0xe0a23dff,
      0x9b5de5ff, 0x2ec4b6ff, 0xf15bb5ff, 0x8d99aeff
   };
   return colors[idx % (sizeof(colors) / sizeof(colors[0]))];
}

const char *grid_profile_icon(unsigned idx)
{
   return idx < profile_st.count ? profile_st.icons[idx] : "";
}

void grid_profile_set_icon(unsigned idx, const char *path)
{
   if (idx >= profile_st.count)
      return;
   strlcpy(profile_st.icons[idx], path ? path : "",
         sizeof(profile_st.icons[0]));
   profile_save();
}

bool grid_profile_select(unsigned idx)
{
   if (idx >= profile_st.count || profile_game_running())
      return false;

   profile_st.current = idx;
   profile_apply();
   profile_save();
   return true;
}

/* Cleans a profile name (it becomes a folder name: safe characters only,
 * no trailing spaces or dots) into clean. False if it ends up empty or
 * another profile (not skip) already has it. */
static bool profile_clean_name(const char *name, unsigned skip,
      char *clean, size_t len)
{
   size_t i, n = 0;

   for (i = 0; name && name[i] && n + 1 < len; i++)
      if (!strchr("\\/:*?\"<>|\t", name[i]))
         clean[n++] = name[i];
   clean[n] = '\0';
   while (n > 0 && (clean[n - 1] == ' ' || clean[n - 1] == '.'))
      clean[--n] = '\0';

   if (string_is_empty(clean))
      return false;
   for (i = 0; i < profile_st.count; i++)
      if (i != skip && string_is_equal_noncase(profile_st.names[i], clean))
         return false;
   return true;
}

bool grid_profile_rename(unsigned idx, const char *name)
{
   char clean[GRID_PROFILE_NAME_LEN];

   if (idx >= profile_st.count
         || (idx == profile_st.current && profile_game_running())
         || !profile_clean_name(name, idx, clean, sizeof(clean)))
      return false;

   if (string_is_equal(clean, profile_st.names[idx]))
      return true;

   /* Profiles keep their saves in folders named after them: move those. */
   {
      char old_dir[2][PATH_MAX_LENGTH], new_dir[2][PATH_MAX_LENGTH];
      char old_name[GRID_PROFILE_NAME_LEN];
      bool moved[2] = { false, false };
      unsigned k;
      const char *bases[2], *fallbacks[2];

      bases[0] = profile_st.base_save;  fallbacks[0] = "saves";
      bases[1] = profile_st.base_state; fallbacks[1] = "states";

      strlcpy(old_name, profile_st.names[idx], sizeof(old_name));
      for (k = 0; k < 2; k++)
      {
         profile_dir(idx, bases[k], fallbacks[k], old_dir[k],
               sizeof(old_dir[k]));
         strlcpy(profile_st.names[idx], clean, GRID_PROFILE_NAME_LEN);
         profile_dir(idx, bases[k], fallbacks[k], new_dir[k],
               sizeof(new_dir[k]));
         strlcpy(profile_st.names[idx], old_name, GRID_PROFILE_NAME_LEN);
      }

      for (k = 0; k < 2; k++)
      {
         if (!path_is_directory(old_dir[k]))
            continue; /* nothing saved yet */
         if (     path_is_valid(new_dir[k])
               || filestream_rename(old_dir[k], new_dir[k]) != 0)
         {
            /* Put back what was already moved, so nothing is orphaned. */
            if (k == 1 && moved[0])
               filestream_rename(new_dir[0], old_dir[0]);
            return false;
         }
         moved[k] = true;
      }
   }

   strlcpy(profile_st.names[idx], clean, GRID_PROFILE_NAME_LEN);
   if (idx == profile_st.current)
      profile_apply();
   profile_save();
   return true;
}

bool grid_profile_add(const char *name)
{
   char clean[GRID_PROFILE_NAME_LEN];

   if (     profile_st.count >= GRID_PROFILE_MAX || profile_game_running()
         || !profile_clean_name(name, GRID_PROFILE_MAX, clean, sizeof(clean)))
      return false;

   profile_st.icons[profile_st.count][0] = '\0';
   strlcpy(profile_st.names[profile_st.count++], clean, GRID_PROFILE_NAME_LEN);
   return grid_profile_select(profile_st.count - 1);
}

bool grid_profile_remove(unsigned idx)
{
   unsigned i;

   if (idx == 0 || idx >= profile_st.count || idx == profile_st.current)
      return false;

   for (i = idx; i + 1 < profile_st.count; i++)
   {
      memcpy(profile_st.names[i], profile_st.names[i + 1],
            GRID_PROFILE_NAME_LEN);
      memcpy(profile_st.icons[i], profile_st.icons[i + 1],
            sizeof(profile_st.icons[0]));
   }
   profile_st.count--;
   if (profile_st.current > idx)
      profile_st.current--;

   profile_save();
   return true;
}
