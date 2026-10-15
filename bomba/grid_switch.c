/* Bomba: the player's Switch keys and firmware, for Eden.
 *
 * Both live in Eden's portable "user" folder (<system>/Eden/user):
 *    keys/prod.keys, keys/title.keys
 *    nand/system/Contents/registered/<id>.nca/00   (one folder per NCA,
 *                                                    the NAND layout)
 * They come from a file the player picks on the Downloads screen, or from
 * RetroArch's system folder (<system>/switch/prod.keys, title.keys,
 * firmware.zip or a firmware/ folder), installed whenever those change.
 * Firmware replaces what Eden had; a zip is extracted to a temporary
 * folder first, then the .nca files (anywhere in it) are copied on a
 * worker thread. Firmware .cnmt.nca files go in as <id>.nca, as the NAND
 * keeps them. <user>/bomba-firmware.txt notes where the
 * firmware came from.
 */

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "grid_switch.h"
#include "../configuration.h"
#include "../runloop.h"

#ifdef HAVE_THREADS
#include <rthreads/rthreads.h>
#endif
#if defined(HAVE_COMPRESSION) && defined(HAVE_ZLIB)
#include "../tasks/tasks_internal.h"
#define SW_HAVE_ZIP 1
#endif

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <encodings/utf.h>
#endif

#define SW_FW_MARKER   "bomba-firmware.txt"
#define SW_FW_TMP      "fw-tmp"
#define SW_FW_MIN_NCAS 10   /* fewer: not firmware */

typedef struct
{
   char src[PATH_MAX_LENGTH];   /* folder with the .nca files */
   char label[256];             /* what it came from, for the card */
   char import_key[PATH_MAX_LENGTH + 32]; /* see sw_import_key; "" picked */
   char user[PATH_MAX_LENGTH];
   bool cleanup;                /* src is the temporary folder */
   volatile bool done;
   bool ok;
   char error[96];
#ifdef HAVE_THREADS
   sthread_t *thread;
#endif
} sw_fw_job_t;

static struct
{
   sw_fw_job_t *job;            /* firmware being installed */
   bool extracting;             /* its zip being extracted (job waits) */
   char keys_error[96];
   char fw_error[96];
   bool changed;
} sw_st;

/* ---- Paths ------------------------------------------------------------ */

static bool sw_eden_user(char *s, size_t len)
{
   char eden[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();

   if (string_is_empty(settings->paths.directory_system))
      return false;
   fill_pathname_join_special(eden, settings->paths.directory_system,
         "Eden", sizeof(eden));
   fill_pathname_join_special(s, eden, "user", len);
   return true;
}

static void sw_registered(const char *user, char *s, size_t len)
{
   fill_pathname_join_special(s, user,
         "nand/system/Contents/registered", len);
}

bool grid_switch_available(void)
{
   char exe[PATH_MAX_LENGTH], eden[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();

   if (string_is_empty(settings->paths.directory_system))
      return false;
   fill_pathname_join_special(eden, settings->paths.directory_system,
         "Eden", sizeof(eden));
   fill_pathname_join_special(exe, eden, "eden.exe", sizeof(exe));
   return path_is_valid(exe);
}

static void sw_toast(const char *msg)
{
   runloop_msg_queue_push(msg, strlen(msg), 1, 240, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
}

static bool sw_eden_running(const char *running_core)
{
   return !string_is_empty(running_core)
      && strstr(path_basename(running_core), "eden_libretro");
}

/* ---- Removing folders ------------------------------------------------- */

#ifdef _WIN32
static bool sw_rmtree_w(const wchar_t *dir)
{
   WIN32_FIND_DATAW fd;
   HANDLE h;
   size_t n    = wcslen(dir);
   bool ok     = true;
   DWORD attr  = GetFileAttributesW(dir);
   wchar_t *pat;

   if (attr == INVALID_FILE_ATTRIBUTES)
      return true; /* already gone */
   /* A junction: the link only. */
   if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
      return RemoveDirectoryW(dir) != 0;

   if (!(pat = (wchar_t*)malloc((n + 3) * sizeof(wchar_t))))
      return false;
   wcscpy(pat, dir);
   wcscat(pat, L"\\*");

   if ((h = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE)
   {
      do
      {
         wchar_t *p;

         if (     !wcscmp(fd.cFileName, L".")
               || !wcscmp(fd.cFileName, L".."))
            continue;
         if (!(p = (wchar_t*)malloc((n + wcslen(fd.cFileName) + 2)
                     * sizeof(wchar_t))))
         {
            ok = false;
            continue;
         }
         wcscpy(p, dir);
         wcscat(p, L"\\");
         wcscat(p, fd.cFileName);

         if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok = sw_rmtree_w(p) && ok;   /* (a junction: just the link) */
         else
         {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
               SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
            ok = DeleteFileW(p) && ok;   /* a file link: just the link */
         }
         free(p);
      } while (FindNextFileW(h, &fd));
      FindClose(h);
   }
   free(pat);

   return RemoveDirectoryW(dir) && ok;
}
#endif

bool grid_switch_rmtree(const char *path)
{
#ifdef _WIN32
   wchar_t *w = utf8_to_utf16_string_alloc(path);
   bool ok    = w && sw_rmtree_w(w);
   free(w);
   return ok;
#else
   return false;
#endif
}

#ifdef _WIN32
HWND win32_get_window(void);
#endif

bool grid_switch_browse(int kind, const char *running_core)
{
#ifdef _WIN32
   static wchar_t last_dir[MAX_PATH];
   wchar_t file[MAX_PATH] = { 0 };
   wchar_t start[MAX_PATH];
   const wchar_t *home = _wgetenv(L"USERPROFILE");
   OPENFILENAMEW ofn;
   char *path;
   bool ok;

   memset(&ofn, 0, sizeof(ofn));
   ofn.lStructSize = sizeof(ofn);
   ofn.hwndOwner   = win32_get_window();
   ofn.lpstrFile   = file;
   ofn.nMaxFile    = MAX_PATH;
   ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
   if (kind == 1)
   {
      ofn.lpstrTitle  = L"Choose your Switch keys (prod.keys)";
      ofn.lpstrFilter = L"Switch keys (*.keys)\0*.keys\0All files\0*.*\0";
   }
   else
   {
      /* A firmware folder of loose .nca files: pick any one of them. */
      ofn.lpstrTitle  = L"Choose Switch firmware (a .zip, or any .nca "
                        L"file in a firmware folder)";
      ofn.lpstrFilter = L"Switch firmware (*.zip, *.nca)\0*.zip;*.nca\0"
                        L"All files\0*.*\0";
   }
   /* Where it was last, else Downloads. */
   if (last_dir[0])
      ofn.lpstrInitialDir = last_dir;
   else if (home)
   {
      _snwprintf(start, MAX_PATH, L"%ls\\Downloads", home);
      start[MAX_PATH - 1] = 0;
      ofn.lpstrInitialDir = start;
   }

   /* RetroArch hides the mouse (a ShowCursor count below zero, on this
    * thread, which the dialog shares) and may keep it inside its window:
    * free and show it for the dialog, then put both back. */
   {
      RECT clip;
      bool clipped = GetClipCursor(&clip) != 0;
      int shows    = 0;
      BOOL picked;

      ClipCursor(NULL);
      do
         shows++;
      while (ShowCursor(TRUE) < 0 && shows < 64);

      picked = GetOpenFileNameW(&ofn);

      while (shows-- > 0)
         ShowCursor(FALSE);
      if (clipped)
         ClipCursor(&clip);

      if (!picked)
         return false; /* cancelled */
   }

   wcsncpy(last_dir, file, MAX_PATH - 1);
   if (ofn.nFileOffset > 0 && ofn.nFileOffset < MAX_PATH)
      last_dir[ofn.nFileOffset - 1] = 0;

   if (!(path = utf16_to_utf8_string_alloc(file)))
      return false;
   if (kind == 1)
      ok = grid_switch_install_keys(path);
   else
   {
      /* An .nca file stands for the folder it is in. */
      if (string_is_equal_noncase(path_get_extension(path), "nca"))
         path_basedir(path);
      ok = grid_switch_install_firmware(path, running_core);
   }
   free(path);
   return ok;
#else
   return false;
#endif
}

/* ---- Keys ------------------------------------------------------------- */

enum grid_switch_state grid_switch_keys(char *detail, size_t len)
{
   char user[PATH_MAX_LENGTH], keys[PATH_MAX_LENGTH];

   if (!string_is_empty(sw_st.keys_error))
   {
      strlcpy(detail, sw_st.keys_error, len);
      return GRID_SWITCH_FAILED;
   }
   if (     sw_eden_user(user, sizeof(user))
         && (fill_pathname_join_special(keys, user, "keys/prod.keys",
               sizeof(keys)), path_is_valid(keys)))
   {
      strlcpy(detail, "Installed", len);
      return GRID_SWITCH_INSTALLED;
   }
   strlcpy(detail, "Missing: choose your prod.keys", len);
   return GRID_SWITCH_MISSING;
}

/* Whether a file's bytes equal a buffer. */
static bool sw_same_file(const char *path, const void *data, int64_t len)
{
   void *buf  = NULL;
   int64_t n  = 0;
   bool same  = filestream_read_file(path, &buf, &n) && buf && n == len
             && !memcmp(buf, data, (size_t)len);
   free(buf);
   return same;
}

/* Copies prod.keys (and title.keys beside it). quiet: no message when
 * nothing changed (the system folder check). Returns true if installed. */
static bool sw_install_keys(const char *path, bool quiet)
{
   char user[PATH_MAX_LENGTH], dir[PATH_MAX_LENGTH], dst[PATH_MAX_LENGTH];
   char src_dir[PATH_MAX_LENGTH], title[PATH_MAX_LENGTH];
   void *buf   = NULL;
   int64_t len = 0;

   if (!sw_eden_user(user, sizeof(user)) || !grid_switch_available())
      return false;

   if (!filestream_read_file(path, &buf, &len) || !buf || len <= 0)
   {
      free(buf);
      strlcpy(sw_st.keys_error, "Couldn't read that file",
            sizeof(sw_st.keys_error));
      sw_st.changed = true;
      return false;
   }

   /* prod.keys is text, "name = hex" lines; these are always in it. */
   {
      char *text = (char*)malloc((size_t)len + 1);
      bool keys  = false;
      if (text)
      {
         memcpy(text, buf, (size_t)len);
         text[len] = '\0';
         keys      = strstr(text, "header_key") || strstr(text, "master_key");
         free(text);
      }
      if (!keys)
      {
         free(buf);
         strlcpy(sw_st.keys_error, "That isn't a prod.keys file",
               sizeof(sw_st.keys_error));
         sw_st.changed = true;
         return false;
      }
   }

   fill_pathname_join_special(dir, user, "keys", sizeof(dir));
   fill_pathname_join_special(dst, dir, "prod.keys", sizeof(dst));
   if (quiet && sw_same_file(dst, buf, len))
   {
      free(buf);
      return false; /* already there */
   }

   path_mkdir(dir);
   if (!filestream_write_file(dst, buf, len))
   {
      free(buf);
      strlcpy(sw_st.keys_error, "Couldn't save the keys",
            sizeof(sw_st.keys_error));
      sw_st.changed = true;
      return false;
   }
   free(buf);

   /* title.keys, if it sits beside it. */
   fill_pathname_basedir(src_dir, path, sizeof(src_dir));
   fill_pathname_join_special(title, src_dir, "title.keys", sizeof(title));
   buf = NULL;
   if (     path_is_valid(title)
         && filestream_read_file(title, &buf, &len) && buf)
   {
      fill_pathname_join_special(dst, dir, "title.keys", sizeof(dst));
      filestream_write_file(dst, buf, len);
   }
   free(buf);

   sw_st.keys_error[0] = '\0';
   sw_st.changed       = true;
   sw_toast("Switch keys installed");
   return true;
}

bool grid_switch_install_keys(const char *path)
{
   return sw_install_keys(path, false);
}

/* ---- Firmware --------------------------------------------------------- */

enum grid_switch_state grid_switch_firmware(char *detail, size_t len)
{
   char user[PATH_MAX_LENGTH], reg[PATH_MAX_LENGTH], marker[PATH_MAX_LENGTH];
   struct string_list *list;
   void *buf   = NULL;
   int64_t n   = 0;
   bool any;

   if (sw_st.job || sw_st.extracting)
   {
      strlcpy(detail, "Installing...", len);
      return GRID_SWITCH_WORKING;
   }
   if (!string_is_empty(sw_st.fw_error))
   {
      strlcpy(detail, sw_st.fw_error, len);
      return GRID_SWITCH_FAILED;
   }
   if (!sw_eden_user(user, sizeof(user)))
   {
      strlcpy(detail, "Missing", len);
      return GRID_SWITCH_MISSING;
   }

   sw_registered(user, reg, sizeof(reg));
   list = path_is_directory(reg)
      ? dir_list_new(reg, NULL, true, true, false, false) : NULL;
   any  = list && list->size > 0;
   if (list)
      string_list_free(list);
   if (!any)
   {
      strlcpy(detail, "Missing: choose a firmware zip or folder", len);
      return GRID_SWITCH_MISSING;
   }

   /* Where it came from, if we installed it. */
   fill_pathname_join_special(marker, user, SW_FW_MARKER, sizeof(marker));
   if (filestream_read_file(marker, &buf, &n) && buf)
   {
      char *nl = strchr((char*)buf, '\n');
      if (nl)
         *nl = '\0';
      snprintf(detail, len, "Installed (%s)", (const char*)buf);
   }
   else
      strlcpy(detail, "Installed", len);
   free(buf);
   return GRID_SWITCH_INSTALLED;
}

/* Copies one file. */
static bool sw_copy_file(const char *src, const char *dst)
{
   uint8_t chunk[65536];
   int64_t n;
   bool ok  = true;
   RFILE *in  = filestream_open(src, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   RFILE *out = in ? filestream_open(dst, RETRO_VFS_FILE_ACCESS_WRITE,
         RETRO_VFS_FILE_ACCESS_HINT_NONE) : NULL;

   if (!in || !out)
      ok = false;
   while (ok && (n = filestream_read(in, chunk, sizeof(chunk))) > 0)
      ok = filestream_write(out, chunk, n) == n;
   if (in)
      filestream_close(in);
   if (out)
      filestream_close(out);
   return ok;
}

static void sw_fw_thread(void *data)
{
   sw_fw_job_t *job = (sw_fw_job_t*)data;
   char reg[PATH_MAX_LENGTH], marker[PATH_MAX_LENGTH];
   struct string_list *list = dir_list_new(job->src, "nca", false, true,
         false, true);
   size_t i, ncas = 0;

   for (i = 0; list && i < list->size; i++)
      if (list->elems[i].attr.i == RARCH_PLAIN_FILE)
         ncas++;

   if (ncas < SW_FW_MIN_NCAS)
   {
      strlcpy(job->error, "No Switch firmware in that", sizeof(job->error));
      goto end;
   }

   /* Replace what Eden had. */
   sw_registered(job->user, reg, sizeof(reg));
   grid_switch_rmtree(reg);
   if (!path_mkdir(reg))
   {
      strlcpy(job->error, "Couldn't write to Eden's folder",
            sizeof(job->error));
      goto end;
   }

   for (i = 0; i < list->size; i++)
   {
      char name[256], dir[PATH_MAX_LENGTH], dst[PATH_MAX_LENGTH];
      const char *src = list->elems[i].data;
      char *cnmt;

      if (list->elems[i].attr.i != RARCH_PLAIN_FILE)
         continue;
      /* "<id>.cnmt.nca" goes in as "<id>.nca". */
      strlcpy(name, path_basename(src), sizeof(name));
      if ((cnmt = strstr(name, ".cnmt.nca")))
         strlcpy(cnmt, ".nca", sizeof(name) - (size_t)(cnmt - name));

      fill_pathname_join_special(dir, reg, name, sizeof(dir));
      fill_pathname_join_special(dst, dir, "00", sizeof(dst));
      if (!path_mkdir(dir) || !sw_copy_file(src, dst))
      {
         strlcpy(job->error, "Couldn't copy the firmware",
               sizeof(job->error));
         goto end;
      }
   }

   /* Where it came from (shown on the card), and the system folder's
    * import key (see sw_import_key). */
   {
      char text[sizeof(job->label) + sizeof(job->import_key) + 4];
      snprintf(text, sizeof(text), "%s\n%s\n", job->label, job->import_key);
      fill_pathname_join_special(marker, job->user, SW_FW_MARKER,
            sizeof(marker));
      filestream_write_file(marker, text, strlen(text));
   }
   job->ok = true;

end:
   if (list)
      string_list_free(list);
   if (job->cleanup)
      grid_switch_rmtree(job->src);
   job->done = true;
}

static void sw_fw_start(void)
{
#ifdef HAVE_THREADS
   if ((sw_st.job->thread = sthread_create(sw_fw_thread, sw_st.job)))
      return;
#endif
   strlcpy(sw_st.fw_error, "Couldn't install", sizeof(sw_st.fw_error));
   free(sw_st.job);
   sw_st.job = NULL;
}

#ifdef SW_HAVE_ZIP
static void sw_fw_extract_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   decompress_task_data_t *dec = (decompress_task_data_t*)task_data;

   if (dec)
   {
      free(dec->source_file);
      free(dec);
   }
   sw_st.extracting = false;
   sw_st.changed    = true;
   if (!sw_st.job)
      return;
   if (!string_is_empty(error))
   {
      grid_switch_rmtree(sw_st.job->src);
      strlcpy(sw_st.fw_error, "Couldn't open that zip",
            sizeof(sw_st.fw_error));
      free(sw_st.job);
      sw_st.job = NULL;
      return;
   }
   sw_fw_start();
}
#endif

static bool sw_install_firmware(const char *path, const char *running_core,
      const char *import_key)
{
   char user[PATH_MAX_LENGTH];

   if (sw_st.job || sw_st.extracting)
      return false;
   if (!sw_eden_user(user, sizeof(user)) || !grid_switch_available())
      return false;
   if (sw_eden_running(running_core))
   {
      sw_toast("Close the Switch game to change the firmware");
      return false;
   }

   if (!(sw_st.job = (sw_fw_job_t*)calloc(1, sizeof(*sw_st.job))))
      return false;
   strlcpy(sw_st.job->user, user, sizeof(sw_st.job->user));
   strlcpy(sw_st.job->label, path_basename(path), sizeof(sw_st.job->label));
   strlcpy(sw_st.job->import_key, import_key ? import_key : "",
         sizeof(sw_st.job->import_key));
   sw_st.fw_error[0] = '\0';
   sw_st.changed     = true;

   if (path_is_directory(path))
   {
      strlcpy(sw_st.job->src, path, sizeof(sw_st.job->src));
      sw_fw_start();
      return sw_st.job != NULL;
   }

#ifdef SW_HAVE_ZIP
   if (string_is_equal_noncase(path_get_extension(path), "zip"))
   {
      char eden[PATH_MAX_LENGTH];
      fill_pathname_basedir(eden, user, sizeof(eden));
      fill_pathname_join_special(sw_st.job->src, eden, SW_FW_TMP,
            sizeof(sw_st.job->src));
      sw_st.job->cleanup = true;
      grid_switch_rmtree(sw_st.job->src);
      path_mkdir(sw_st.job->src);
      sw_st.extracting = true;
      if (task_push_decompress(path, sw_st.job->src, NULL, NULL, NULL,
               sw_fw_extract_cb, NULL, NULL, true))
         return true;
      sw_st.extracting = false;
   }
#endif

   strlcpy(sw_st.fw_error, "Choose a firmware zip or folder",
         sizeof(sw_st.fw_error));
   free(sw_st.job);
   sw_st.job = NULL;
   return false;
}

bool grid_switch_install_firmware(const char *path, const char *running_core)
{
   return sw_install_firmware(path, running_core, NULL);
}

/* ---- System folder ---------------------------------------------------- */

/* What identifies a firmware source in the system folder, so it is
 * installed again only when it changes: its path and size (a folder: its
 * .nca files' count and total size). */
static void sw_import_key(const char *path, char *s, size_t len)
{
   uint64_t size = 0;
   size_t count  = 0;

   if (path_is_directory(path))
   {
      struct string_list *list = dir_list_new(path, "nca", false, true,
            false, true);
      size_t i;
      for (i = 0; list && i < list->size; i++)
      {
         int32_t sz = path_get_size(list->elems[i].data);
         if (sz > 0)
            size += (uint64_t)sz;
         count++;
      }
      if (list)
         string_list_free(list);
   }
   else
      size = (uint64_t)path_get_size(path);
   snprintf(s, len, "%s|%u|%llu", path, (unsigned)count,
         (unsigned long long)size);
}

void grid_switch_import_system(const char *running_core)
{
   char sys[PATH_MAX_LENGTH], p[PATH_MAX_LENGTH], user[PATH_MAX_LENGTH];
   char key[PATH_MAX_LENGTH + 32], marker[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();
   void *buf   = NULL;
   int64_t n   = 0;
   bool same   = false;

   if (     string_is_empty(settings->paths.directory_system)
         || !grid_switch_available() || !sw_eden_user(user, sizeof(user)))
      return;
   fill_pathname_join_special(sys, settings->paths.directory_system,
         "switch", sizeof(sys));
   if (!path_is_directory(sys))
      return;

   fill_pathname_join_special(p, sys, "prod.keys", sizeof(p));
   if (path_is_valid(p))
      sw_install_keys(p, true);

   fill_pathname_join_special(p, sys, "firmware.zip", sizeof(p));
   if (!path_is_valid(p))
   {
      fill_pathname_join_special(p, sys, "firmware", sizeof(p));
      if (!path_is_directory(p))
         return;
   }
   if (sw_st.job || sw_st.extracting || sw_eden_running(running_core))
      return;

   /* Installed from this already (the marker's second line)? */
   sw_import_key(p, key, sizeof(key));
   fill_pathname_join_special(marker, user, SW_FW_MARKER, sizeof(marker));
   if (filestream_read_file(marker, &buf, &n) && buf)
   {
      char *line = strchr((char*)buf, '\n');
      char *end  = line ? strchr(line + 1, '\n') : NULL;
      if (end)
         *end = '\0';
      same = line && string_is_equal(line + 1, key);
   }
   free(buf);
   if (!same)
      sw_install_firmware(p, running_core, key);
}

/* ---- Per frame -------------------------------------------------------- */

bool grid_switch_poll(void)
{
   bool changed;

   if (sw_st.job && !sw_st.extracting && sw_st.job->done)
   {
#ifdef HAVE_THREADS
      sthread_join(sw_st.job->thread);
#endif
      if (sw_st.job->ok)
         sw_toast("Switch firmware installed");
      else
         strlcpy(sw_st.fw_error, sw_st.job->error, sizeof(sw_st.fw_error));
      free(sw_st.job);
      sw_st.job     = NULL;
      sw_st.changed = true;
   }

   changed       = sw_st.changed;
   sw_st.changed = false;
   return changed;
}
