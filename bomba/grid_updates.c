/* Bomba: the Downloads screen's list (opened from the grid's
 * download button): updates for what is installed, and what can be
 * installed, each only when the player picks it.
 *
 * Two kinds of item:
 *  - Eden (the Switch emulator, <system>/Eden): its latest release on
 *    git.eden-emu.dev. The installed version is kept in
 *    <system>/Eden/bomba-version.txt (Eden's exe doesn't say);
 *    without it an installed Eden counts as out of date. Installing
 *    downloads the Windows amd64 zip and extracts it over the folder (its
 *    "user" folder isn't in the zip, so settings, keys and saves stay).
 *  - Cores: every core on the buildbot's list. Installed ones are compared
 *    by CRC (worked out on a thread, cores can be big). Installing goes
 *    through RetroArch's core updater; a core not installed yet gets its
 *    .info file from libretro-core-info first, so RetroArch knows which
 *    games it runs.
 */

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <compat/strl.h>
#include <features/features_cpu.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "grid_updates.h"
#include "grid_switch.h"

#if defined(HAVE_NETWORKING) && defined(HAVE_UPDATE_CORES) && defined(HAVE_THREADS)

#include <encodings/crc32.h>
#include <file/config_file.h>
#include <rthreads/rthreads.h>

#include "../command.h"
#include "../configuration.h"
#include "../core_info.h"
#include "../core_updater_list.h"
#include "../runloop.h"
#include "../tasks/tasks_internal.h"

#ifdef _WIN32
#include <windows.h>
#include <encodings/utf.h>
#endif

#define EDEN_API      "https://git.eden-emu.dev/api/v1/repos/eden-emu/eden/releases/latest"
/* Release zips, in order of preference: the build Eden ships as its main
 * Windows one, then the plain one. */
static const char *eden_assets[] = {
   "-amd64-clang-pgo.zip",
   "-amd64-gcc-standard.zip"
};
#define EDEN_VERSION_FILE "bomba-version.txt"
#define EDEN_ZIP          "Eden-update.zip"

#define CORE_INFO_URL "https://raw.githubusercontent.com/libretro/libretro-core-info/master/"

#define UPDATES_RECHECK  (30LL * 60 * 1000000) /* usec between checks */
/* Give up on a core list that takes longer than this. */
#define UPDATES_TIMEOUT  (5LL * 60 * 1000000)

/* ---- Cores ------------------------------------------------------------ */

enum upd_core_step
{
   CORE_IDLE = 0,
   CORE_INFO,               /* its .info file downloading */
   CORE_DOWNLOAD            /* the core updater's task running */
};

typedef struct
{
   char remote[128];        /* buildbot file name, "x_libretro.dll.zip" */
   char display[128];
   char path[PATH_MAX_LENGTH];
   char info_path[PATH_MAX_LENGTH];
   char base[64];           /* "x" */
   char console[96];        /* from its .info: systemname */
   char maker[64];          /* ...and manufacturer */
   uint32_t remote_crc;
   uint32_t local_crc;      /* installed ones: worked out on the thread */
   unsigned date;
   bool installed;
   bool locked;
   bool steam;              /* a Steam DLC core (Steam's own build) */
   enum grid_update_section section;
   enum grid_update_state state;
   enum upd_core_step step;
   void *task;
   int progress;
} upd_core_t;

/* CRCs of the installed cores, worked out on a worker thread. It only
 * writes local_crc of installed cores; the array stays put meanwhile. */
typedef struct
{
   upd_core_t *cores;
   size_t count;
   volatile bool done;
   sthread_t *thread;
} upd_crc_job_t;

enum upd_cores_step
{
   CORES_IDLE = 0,
   CORES_LIST,              /* waiting for the buildbot's list */
   CORES_CRC,               /* comparing installed ones on the thread */
   CORES_READY,
   CORES_FAILED
};

/* Every core's .info file (libretro's info.zip), unpacked to
 * <exe dir>/bomba/core-info once a session: what each core is called
 * and which console it plays, also for cores not installed. */
enum upd_info_step
{
   INFO_NONE = 0,
   INFO_DOWNLOAD,
   INFO_EXTRACT,
   INFO_READY,
   INFO_FAILED              /* installed cores still use RetroArch's */
};

/* ---- Eden ------------------------------------------------------------- */

enum upd_eden_step
{
   EDEN_IDLE = 0,           /* not checked (no system directory) */
   EDEN_CHECK,              /* release info in flight */
   EDEN_READY,              /* checked: see eden_state */
   EDEN_DOWNLOAD,
   EDEN_EXTRACT,
   EDEN_REMOVE               /* its folder being deleted */
};

static struct
{
   /* Eden */
   enum upd_eden_step eden_step;
   enum grid_update_state eden_state;
   enum grid_update_section eden_section;
   bool eden_installed_now;  /* eden.exe there when checked */
   char eden_dir[PATH_MAX_LENGTH];
   char eden_installed[32];  /* installed tag, "" if unknown */
   char eden_latest[32];
   char eden_url[1024];
   char eden_zip[PATH_MAX_LENGTH];
   char eden_error[96];
   unsigned eden_date;
   void *eden_task;
   int eden_progress;

   /* Cores */
   enum upd_cores_step cores_step;
   core_updater_list_t *list;
   void *list_task;
   retro_time_t list_started;
   upd_core_t *cores;
   size_t core_count;
   upd_crc_job_t *crc_job;

   /* Core info bundle */
   enum upd_info_step info_step;
   char info_dir[PATH_MAX_LENGTH];
   char info_zip[PATH_MAX_LENGTH];
   bool info_applied;        /* the current cores have their names */

   /* The list shown, rebuilt when something changes: Eden, then the
    * cores. item_core: index into cores, -1 for Eden. */
   grid_update_t *items;
   int *item_core;
   size_t item_count;
   size_t item_cap;

   retro_time_t last_check;
   bool checked;
   bool installed;           /* something finished installing (for poll) */
} upd_st;

/* ---- Helpers ---------------------------------------------------------- */

/* The string value of "key" in a JSON text, searching from s. Returns the
 * position after it, or NULL. Good enough for Forgejo's compact JSON. */
static const char *upd_json_str(const char *s, const char *key,
      char *out, size_t len)
{
   char pat[64];
   const char *p, *end;
   size_t n;

   snprintf(pat, sizeof(pat), "\"%s\"", key);
   if (!(p = strstr(s, pat)))
      return NULL;
   p += strlen(pat);
   while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r')
      p++;
   if (*p != '"')
      return NULL;
   p++;
   if (!(end = strchr(p, '"')))
      return NULL;
   n = (size_t)(end - p);
   if (n >= len)
      n = len - 1;
   memcpy(out, p, n);
   out[n] = '\0';
   return end + 1;
}

static bool upd_ends_with(const char *s, const char *suffix)
{
   size_t ls = strlen(s), lx = strlen(suffix);
   return ls >= lx && string_is_equal(s + ls - lx, suffix);
}

/* Whether a task is still in the queue (it is freed once it ends). Only
 * compares pointers, so a finished task is never touched. */
typedef struct { void *task; int progress; bool found; } upd_find_t;

static bool upd_find_task(retro_task_t *task, void *userdata)
{
   upd_find_t *f = (upd_find_t*)userdata;
   if (task && task == f->task)
   {
      f->found    = true;
      f->progress = task->progress;
      return true;
   }
   return false;
}

static bool upd_task_running(void *task, int *progress)
{
   task_finder_data_t find;
   upd_find_t f;

   if (!task)
      return false;
   f.task        = task;
   f.progress    = -1;
   f.found       = false;
   find.func     = upd_find_task;
   find.userdata = &f;
   task_queue_find(&find);
   if (progress)
      *progress = f.found ? f.progress : -1;
   return f.found;
}

static void upd_toast(const char *msg)
{
   runloop_msg_queue_push(msg, strlen(msg), 1, 240, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
}

static void upd_working_detail(char *s, size_t len, const char *what,
      int progress)
{
   if (progress > 0 && progress <= 100)
      snprintf(s, len, "%s %d%%", what, progress);
   else
      snprintf(s, len, "%s...", what);
}

/* ---- The shown list --------------------------------------------------- */

static void upd_add_item(const char *name, const char *detail,
      const char *version, const char *console, const char *maker,
      bool removable, const char *kind, enum grid_update_section section,
      enum grid_update_state state, int progress, unsigned date, int core)
{
   grid_update_t *it;

   if (upd_st.item_count >= upd_st.item_cap)
      return;
   it = &upd_st.items[upd_st.item_count];
   strlcpy(it->name, name, sizeof(it->name));
   strlcpy(it->detail, detail, sizeof(it->detail));
   strlcpy(it->version, version, sizeof(it->version));
   strlcpy(it->console, console, sizeof(it->console));
   snprintf(it->search, sizeof(it->search), "%s\n%s\n%s", name, console,
         maker);
   it->removable = removable;
   it->kind     = kind;
   it->section  = section;
   it->state    = state;
   it->progress = progress;
   it->date     = date;
   upd_st.item_core[upd_st.item_count++] = core;
}

static void upd_eden_detail(char *s, size_t len)
{
   bool update = upd_st.eden_section == GRID_SECTION_UPDATES;

   switch (upd_st.eden_state)
   {
      /* (The card shows the version itself.) */
      case GRID_UPDATE_CURRENT:
         strlcpy(s, "Up to date", len);
         break;
      case GRID_UPDATE_AVAILABLE:
         if (update)
            snprintf(s, len, "%s available", upd_st.eden_latest);
         else
            strlcpy(s, "Not installed", len);
         break;
      case GRID_UPDATE_WORKING:
         if (upd_st.eden_step == EDEN_REMOVE)
         {
            strlcpy(s, "Removing...", len);
            break;
         }
         upd_working_detail(s, len,
               upd_st.eden_step == EDEN_EXTRACT ? "Installing"
               : "Downloading", upd_st.eden_step == EDEN_EXTRACT
               ? -1 : upd_st.eden_progress);
         break;
      case GRID_UPDATE_DONE:
         strlcpy(s, "Up to date", len);
         break;
      case GRID_UPDATE_FAILED:
      default:
         strlcpy(s, upd_st.eden_error, len);
         break;
   }
}

static void upd_core_detail(const upd_core_t *c, char *s, size_t len)
{
   bool update = c->section == GRID_SECTION_UPDATES;

   switch (c->state)
   {
      case GRID_UPDATE_CURRENT:
         strlcpy(s, c->locked ? "Locked (kept as it is)" : "Up to date", len);
         break;
      case GRID_UPDATE_AVAILABLE:
         strlcpy(s, update ? "Update available" : "Not installed", len);
         break;
      case GRID_UPDATE_WORKING:
         upd_working_detail(s, len, c->step == CORE_INFO ? "Preparing"
               : update ? "Updating" : "Installing", c->progress);
         break;
      case GRID_UPDATE_DONE:
         strlcpy(s, "Up to date", len);
         break;
      case GRID_UPDATE_FAILED:
      default:
         strlcpy(s, update ? "Couldn't update" : "Couldn't install", len);
         break;
   }
}

static void upd_rebuild(void)
{
   char detail[128], version[32];
   size_t i;

   upd_st.item_count = 0;

   /* The player's Switch keys and firmware, once Eden is there: A picks a
    * file for them (item_core -2 keys, -3 firmware). */
   if (grid_switch_available())
   {
      static const enum grid_update_state map[] = {
         GRID_UPDATE_AVAILABLE, /* missing */
         GRID_UPDATE_CURRENT,   /* installed */
         GRID_UPDATE_WORKING,
         GRID_UPDATE_FAILED
      };
      enum grid_switch_state s;

      s = grid_switch_keys(detail, sizeof(detail));
      upd_add_item("Switch keys", detail, "", "Nintendo Switch",
            "Nintendo Eden prod.keys title.keys", false, "Setup",
            s == GRID_SWITCH_INSTALLED ? GRID_SECTION_INSTALLED
            : GRID_SECTION_INSTALL, map[s], -1, 0, -2);
      s = grid_switch_firmware(detail, sizeof(detail));
      upd_add_item("Switch firmware", detail, "", "Nintendo Switch",
            "Nintendo Eden", false, "Setup",
            s == GRID_SWITCH_INSTALLED ? GRID_SECTION_INSTALLED
            : GRID_SECTION_INSTALL, map[s], -1, 0, -3);
   }

   if (upd_st.eden_step != EDEN_IDLE && upd_st.eden_step != EDEN_CHECK)
   {
      upd_eden_detail(detail, sizeof(detail));
      /* Its version: what is installed, else what would be. */
      upd_add_item("Eden", detail,
            upd_st.eden_installed_now && !string_is_empty(upd_st.eden_installed)
            ? upd_st.eden_installed : upd_st.eden_latest,
            "Nintendo Switch", "Nintendo",
            upd_st.eden_installed_now
            && upd_st.eden_state != GRID_UPDATE_WORKING, "Emulator",
            upd_st.eden_section,
            upd_st.eden_state,
            upd_st.eden_step == EDEN_DOWNLOAD ? upd_st.eden_progress : -1,
            upd_st.eden_date, -1);
   }

   for (i = 0; i < upd_st.core_count; i++)
   {
      upd_core_t *c = &upd_st.cores[i];

      /* Installed ones show once compared. */
      if (     c->installed && !c->locked
            && c->state == GRID_UPDATE_CURRENT
            && upd_st.cores_step != CORES_READY)
         continue;
      upd_core_detail(c, detail, sizeof(detail));
      /* Its version: Steam's build, or the buildbot build's date. */
      if (c->steam)
         strlcpy(version, "Steam build", sizeof(version));
      else if (c->date)
         snprintf(version, sizeof(version), "%04u-%02u-%02u",
               c->date / 10000, (c->date / 100) % 100, c->date % 100);
      else
         version[0] = '\0';
      upd_add_item(c->display, detail, version, c->console, c->maker,
            c->installed && !c->locked && c->state != GRID_UPDATE_WORKING,
            "Core", c->section, c->state,
            c->step == CORE_DOWNLOAD ? c->progress : -1, c->date, (int)i);
   }
}

/* ---- Eden ------------------------------------------------------------- */

static void upd_eden_fail(const char *why)
{
   upd_st.eden_step  = EDEN_READY;
   upd_st.eden_state = GRID_UPDATE_FAILED;
   strlcpy(upd_st.eden_error, why, sizeof(upd_st.eden_error));
   upd_rebuild();
}

static void upd_eden_read_installed(void)
{
   char path[PATH_MAX_LENGTH];
   void *buf   = NULL;
   int64_t len = 0;

   upd_st.eden_installed[0] = '\0';
   fill_pathname_join_special(path, upd_st.eden_dir, EDEN_VERSION_FILE,
         sizeof(path));
   if (filestream_read_file(path, &buf, &len) && buf)
   {
      strlcpy(upd_st.eden_installed, (const char*)buf,
            sizeof(upd_st.eden_installed));
      string_trim_whitespace_right(upd_st.eden_installed);
   }
   free(buf);
}

static void upd_eden_check_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   char date[32];
   char *json;
   const char *p;
   size_t k;

   upd_st.eden_task = NULL;

   if (     !data || data->status != 200 || !data->data || !data->len
         || !(json = (char*)malloc(data->len + 1)))
   {
      upd_eden_fail("Couldn't check for updates");
      return;
   }
   memcpy(json, data->data, data->len);
   json[data->len] = '\0';

   upd_st.eden_url[0] = '\0';
   if (!upd_json_str(json, "tag_name", upd_st.eden_latest,
            sizeof(upd_st.eden_latest)))
   {
      free(json);
      upd_eden_fail("Couldn't check for updates");
      return;
   }

   /* "2026-06-01T19:57:41+02:00" -> 20260601 */
   upd_st.eden_date = 0;
   if (upd_json_str(json, "published_at", date, sizeof(date)))
   {
      unsigned y, m, d;
      if (sscanf(date, "%u-%u-%u", &y, &m, &d) == 3)
         upd_st.eden_date = y * 10000 + m * 100 + d;
   }

   /* The preferred Windows zip among the release's files. */
   for (k = 0; k < sizeof(eden_assets) / sizeof(eden_assets[0])
         && string_is_empty(upd_st.eden_url); k++)
   {
      char url[1024];
      for (p = json; (p = upd_json_str(p, "browser_download_url", url,
                  sizeof(url))); )
         if (strstr(url, "Windows") && upd_ends_with(url, eden_assets[k]))
         {
            strlcpy(upd_st.eden_url, url, sizeof(upd_st.eden_url));
            break;
         }
   }
   free(json);

   upd_st.eden_step = EDEN_READY;

   if (     upd_st.eden_installed_now
         && string_is_equal(upd_st.eden_installed, upd_st.eden_latest))
   {
      upd_st.eden_section = GRID_SECTION_INSTALLED;
      upd_st.eden_state   = GRID_UPDATE_CURRENT;
   }
   else if (string_is_empty(upd_st.eden_url))
   {
      upd_eden_fail("No Windows build in the latest release");
      return;
   }
   else
      upd_st.eden_state = GRID_UPDATE_AVAILABLE;

   upd_rebuild();
}

static void upd_eden_check(void)
{
   char exe[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();

   if (string_is_empty(settings->paths.directory_system))
   {
      upd_st.eden_step = EDEN_IDLE;
      return;
   }
   fill_pathname_join_special(upd_st.eden_dir,
         settings->paths.directory_system, "Eden", sizeof(upd_st.eden_dir));
   fill_pathname_join_special(exe, upd_st.eden_dir, "eden.exe", sizeof(exe));

   upd_st.eden_installed_now = path_is_valid(exe);
   upd_eden_read_installed();
   /* Where it shows until the check says otherwise (a failed check
    * leaves it there, saying so). */
   upd_st.eden_section = upd_st.eden_installed_now
      ? GRID_SECTION_UPDATES : GRID_SECTION_INSTALL;

   upd_st.eden_step  = EDEN_CHECK;
   if (!(upd_st.eden_task = task_push_http_transfer(EDEN_API, true, "GET",
               upd_eden_check_cb, NULL)))
      upd_eden_fail("Couldn't check for updates");
}

static void upd_eden_extract_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   decompress_task_data_t *dec = (decompress_task_data_t*)task_data;
   char path[PATH_MAX_LENGTH];
   bool update = upd_st.eden_section == GRID_SECTION_UPDATES;

   if (dec)
   {
      free(dec->source_file);
      free(dec);
   }

   if (!string_is_empty(error))
   {
      /* The zip stays, in case it helps to look at it. */
      upd_eden_fail(update ? "Couldn't update" : "Couldn't install");
      return;
   }

   filestream_delete(upd_st.eden_zip);
   fill_pathname_join_special(path, upd_st.eden_dir, EDEN_VERSION_FILE,
         sizeof(path));
   filestream_write_file(path, upd_st.eden_latest,
         strlen(upd_st.eden_latest));
   strlcpy(upd_st.eden_installed, upd_st.eden_latest,
         sizeof(upd_st.eden_installed));

   /* A fresh install: Eden keeps its settings, keys and saves in a "user"
    * folder next to it only if one is there (else in AppData). */
   fill_pathname_join_special(path, upd_st.eden_dir, "user", sizeof(path));
   path_mkdir(path);

   upd_st.eden_installed_now = true;
   upd_st.eden_step          = EDEN_READY;
   upd_st.eden_state         = GRID_UPDATE_DONE;
   upd_st.eden_section       = GRID_SECTION_INSTALLED;
   upd_st.installed          = true;
   upd_rebuild();
   upd_toast(update ? "Eden updated" : "Eden installed");
}

static void upd_eden_download_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;

   upd_st.eden_task = NULL;

   if (!data || data->status != 200 || !data->data || !data->len)
   {
      upd_eden_fail("Download failed");
      return;
   }

   fill_pathname_join_special(upd_st.eden_zip,
         config_get_ptr()->paths.directory_system, EDEN_ZIP,
         sizeof(upd_st.eden_zip));
   if (!filestream_write_file(upd_st.eden_zip, data->data, data->len))
   {
      upd_eden_fail("Couldn't save the download");
      return;
   }

   path_mkdir(upd_st.eden_dir);
   upd_st.eden_step = EDEN_EXTRACT;
   upd_rebuild();
#if defined(HAVE_COMPRESSION) && defined(HAVE_ZLIB)
   if (!task_push_decompress(upd_st.eden_zip, upd_st.eden_dir, NULL, NULL,
            NULL, upd_eden_extract_cb, NULL, NULL, true))
#endif
      upd_eden_fail("Couldn't install");
}

static void upd_eden_install(const char *running_core)
{
   if (     upd_st.eden_step != EDEN_READY
         || (upd_st.eden_state != GRID_UPDATE_AVAILABLE
          && upd_st.eden_state != GRID_UPDATE_FAILED))
      return;

   /* A failed check: check again rather than install. */
   if (string_is_empty(upd_st.eden_url))
   {
      upd_eden_check();
      upd_rebuild();
      return;
   }

   /* Eden's files can't be replaced while it runs. */
   if (     !string_is_empty(running_core)
         && strstr(path_basename(running_core), "eden_libretro"))
   {
      upd_toast("Close the Switch game to update Eden");
      return;
   }

   upd_st.eden_step     = EDEN_DOWNLOAD;
   upd_st.eden_state    = GRID_UPDATE_WORKING;
   upd_st.eden_progress = -1;
   command_event(CMD_EVENT_NETWORK_INIT, NULL);
   if (!(upd_st.eden_task = task_push_http_transfer(upd_st.eden_url, true,
               "GET", upd_eden_download_cb, NULL)))
   {
      upd_eden_fail("Download failed");
      return;
   }
   upd_rebuild();
}

/* Removing Eden: its whole folder, on a worker thread (shader caches can
 * be big). Junctions in it (the core links Eden's save folder to the
 * profile's saves) are removed as links: what they point to stays, so
 * saves are kept. Keys, firmware and Eden's settings go with it. */
typedef struct
{
   char path[PATH_MAX_LENGTH];
   volatile bool done;
   bool ok;
   sthread_t *thread;
} upd_rm_job_t;

static upd_rm_job_t *upd_eden_rm_job;

static void upd_rm_thread(void *data)
{
   upd_rm_job_t *job = (upd_rm_job_t*)data;
   job->ok           = grid_switch_rmtree(job->path);
   job->done         = true;
}

static bool upd_eden_remove(const char *running_core)
{
   if (     upd_eden_rm_job || !upd_st.eden_installed_now
         || upd_st.eden_state == GRID_UPDATE_WORKING
         || string_is_empty(upd_st.eden_dir))
      return false;

   if (     !string_is_empty(running_core)
         && strstr(path_basename(running_core), "eden_libretro"))
   {
      upd_toast("Close the Switch game to remove Eden");
      return false;
   }

#ifdef _WIN32
   /* Saves must be in the profile, behind the link: if Eden's save folder
    * is a real one with something in it, they'd go too. */
   {
      char save[PATH_MAX_LENGTH];
      wchar_t *w;
      bool real_with_files = false;

      fill_pathname_join_special(save, upd_st.eden_dir,
            "user\\nand\\user\\save", sizeof(save));
      if ((w = utf8_to_utf16_string_alloc(save)))
      {
         DWORD attr = GetFileAttributesW(w);
         if (     attr != INVALID_FILE_ATTRIBUTES
               && !(attr & FILE_ATTRIBUTE_REPARSE_POINT))
         {
            wchar_t pat[PATH_MAX_LENGTH + 4];
            WIN32_FIND_DATAW fd;
            HANDLE h;

            wcsncpy(pat, w, PATH_MAX_LENGTH);
            pat[PATH_MAX_LENGTH] = 0;
            wcscat(pat, L"\\*");
            if ((h = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE)
            {
               do
                  if (     wcscmp(fd.cFileName, L".")
                        && wcscmp(fd.cFileName, L".."))
                     real_with_files = true;
               while (!real_with_files && FindNextFileW(h, &fd));
               FindClose(h);
            }
         }
         free(w);
      }
      if (real_with_files)
      {
         upd_toast("Not removing Eden: its saves aren't in your profile "
               "yet (play a Switch game once first)");
         return false;
      }
   }
#endif

   if (!(upd_eden_rm_job = (upd_rm_job_t*)calloc(1,
               sizeof(*upd_eden_rm_job))))
      return false;
   strlcpy(upd_eden_rm_job->path, upd_st.eden_dir,
         sizeof(upd_eden_rm_job->path));
   if (!(upd_eden_rm_job->thread = sthread_create(upd_rm_thread,
               upd_eden_rm_job)))
   {
      free(upd_eden_rm_job);
      upd_eden_rm_job = NULL;
      return false;
   }

   upd_st.eden_step  = EDEN_REMOVE;
   upd_st.eden_state = GRID_UPDATE_WORKING;
   upd_rebuild();
   return true;
}

/* Per frame: Eden's removal finished? */
static void upd_eden_remove_poll(void)
{
   char exe[PATH_MAX_LENGTH];

   if (!upd_eden_rm_job || !upd_eden_rm_job->done)
      return;
   sthread_join(upd_eden_rm_job->thread);
   free(upd_eden_rm_job);
   upd_eden_rm_job = NULL;

   fill_pathname_join_special(exe, upd_st.eden_dir, "eden.exe", sizeof(exe));
   upd_st.eden_step = EDEN_READY;
   if (path_is_valid(exe))
   {
      /* Something in it was in use: it stays (maybe partly). */
      upd_st.eden_state = GRID_UPDATE_FAILED;
      strlcpy(upd_st.eden_error, "Couldn't remove", sizeof(upd_st.eden_error));
      upd_toast("Couldn't remove Eden; close anything using its files");
   }
   else
   {
      upd_st.eden_installed_now = false;
      upd_st.eden_installed[0]  = '\0';
      upd_st.eden_section       = GRID_SECTION_INSTALL;
      upd_st.eden_state         = string_is_empty(upd_st.eden_url)
         ? GRID_UPDATE_FAILED : GRID_UPDATE_AVAILABLE;
      strlcpy(upd_st.eden_error, "Couldn't check for updates",
            sizeof(upd_st.eden_error));
      upd_st.installed          = true;
      upd_toast("Removed Eden (your Switch saves are kept)");
   }
   upd_rebuild();
}

/* ---- Cores ------------------------------------------------------------ */

static uint32_t upd_file_crc(const char *path)
{
   uint8_t buf[65536];
   uint32_t crc = 0;
   int64_t n;
   RFILE *f     = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (!f)
      return 0;
   while ((n = filestream_read(f, buf, sizeof(buf))) > 0)
      crc = encoding_crc32(crc, buf, (size_t)n);
   filestream_close(f);
   return crc;
}

static void upd_crc_thread(void *data)
{
   upd_crc_job_t *job = (upd_crc_job_t*)data;
   size_t i;

   for (i = 0; i < job->count; i++)
      if (job->cores[i].installed && !job->cores[i].locked)
         job->cores[i].local_crc = upd_file_crc(job->cores[i].path);
   job->done = true;
}

/* "ppsspp_libretro.dll.zip" -> base "ppsspp"; and, when the list had no
 * name for it (no .info file yet), "ppsspp" as the name too. */
static void upd_core_names(upd_core_t *c, const char *display)
{
   const char *end = strstr(c->remote, "_libretro");
   size_t n        = end ? (size_t)(end - c->remote) : strlen(c->remote);

   if (n >= sizeof(c->base))
      n = sizeof(c->base) - 1;
   memcpy(c->base, c->remote, n);
   c->base[n] = '\0';

   if (!string_is_empty(display) && !string_is_equal(display, c->remote))
      strlcpy(c->display, display, sizeof(c->display));
   else
   {
      char *s;
      strlcpy(c->display, c->base, sizeof(c->display));
      for (s = c->display; *s; s++)
         if (*s == '_')
            *s = ' ';
   }
}

static void upd_cores_free(void)
{
   free(upd_st.cores);
   free(upd_st.items);
   free(upd_st.item_core);
   upd_st.cores      = NULL;
   upd_st.items      = NULL;
   upd_st.item_core  = NULL;
   upd_st.core_count = 0;
   upd_st.item_count = 0;
   upd_st.item_cap   = 0;
}

/* Room for the shown list: Eden and every core. */
static bool upd_alloc_items(size_t cores)
{
   free(upd_st.items);
   free(upd_st.item_core);
   upd_st.item_cap   = cores + 3; /* + Eden, Switch keys, firmware */
   upd_st.item_count = 0;
   upd_st.items      = (grid_update_t*)calloc(upd_st.item_cap,
         sizeof(*upd_st.items));
   upd_st.item_core  = (int*)calloc(upd_st.item_cap, sizeof(int));
   if (!upd_st.items || !upd_st.item_core)
   {
      upd_st.item_cap = 0;
      return false;
   }
   return true;
}

/* The buildbot's list is in: note every core on it and start comparing
 * the installed ones. */
static void upd_cores_list_ready(void)
{
   size_t i, n = core_updater_list_size(upd_st.list);

   upd_cores_free();
   upd_st.info_applied = false;
   if (     !n
         || !(upd_st.cores = (upd_core_t*)calloc(n, sizeof(upd_core_t)))
         || !upd_alloc_items(n)
         || !(upd_st.crc_job = (upd_crc_job_t*)calloc(1,
               sizeof(*upd_st.crc_job))))
   {
      upd_cores_free();
      upd_alloc_items(0);
      upd_st.cores_step = CORES_FAILED;
      upd_rebuild();
      return;
   }

   for (i = 0; i < n; i++)
   {
      const core_updater_list_entry_t *e = NULL;
      upd_core_t *c;

      if (     !core_updater_list_get_index(upd_st.list, i, &e) || !e
            || string_is_empty(e->local_core_path)
            || string_is_empty(e->remote_filename))
         continue;

      c = &upd_st.cores[upd_st.core_count++];
      strlcpy(c->remote, e->remote_filename, sizeof(c->remote));
      upd_core_names(c, e->display_name);
      strlcpy(c->path, e->local_core_path, sizeof(c->path));
      if (!string_is_empty(e->local_info_path))
         strlcpy(c->info_path, e->local_info_path, sizeof(c->info_path));
      c->remote_crc = e->crc;
      c->date       = e->date.year * 10000 + e->date.month * 100
                    + e->date.day;
      c->installed  = path_is_valid(e->local_core_path);
      /* Locked cores are kept as they are. */
      c->locked     = c->installed
                    && core_info_get_core_lock(e->local_core_path, false);
      /* Steam DLC cores are Steam's own builds, so they never match the
       * buildbot's: updating one swaps in the buildbot's (which Steam may
       * put back when it updates RetroArch). Steam puts <core>-LICENSE.txt
       * next to each one. */
      if (c->installed)
      {
         char dir[PATH_MAX_LENGTH], lic[PATH_MAX_LENGTH], name[96];
         fill_pathname_basedir(dir, c->path, sizeof(dir));
         snprintf(name, sizeof(name), "%s-LICENSE.txt", c->base);
         fill_pathname_join_special(lic, dir, name, sizeof(lic));
         c->steam = path_is_valid(lic);
      }
      c->progress   = -1;
      if (c->installed)
      {
         c->section = c->locked ? GRID_SECTION_INSTALLED : GRID_SECTION_UPDATES;
         c->state   = GRID_UPDATE_CURRENT;
      }
      else
      {
         c->section = GRID_SECTION_INSTALL;
         c->state   = GRID_UPDATE_AVAILABLE;
      }
   }

   upd_st.crc_job->cores = upd_st.cores;
   upd_st.crc_job->count = upd_st.core_count;
   upd_st.cores_step     = CORES_CRC;
   if (!(upd_st.crc_job->thread = sthread_create(upd_crc_thread,
               upd_st.crc_job)))
   {
      free(upd_st.crc_job);
      upd_st.crc_job    = NULL;
      upd_st.cores_step = CORES_FAILED;
   }
   upd_rebuild();
}

static bool upd_cores_busy(void)
{
   size_t i;
   if (upd_st.cores_step == CORES_LIST || upd_st.cores_step == CORES_CRC)
      return true;
   for (i = 0; i < upd_st.core_count; i++)
      if (upd_st.cores[i].state == GRID_UPDATE_WORKING)
         return true;
   return false;
}

static void upd_cores_check(void)
{
   if (upd_cores_busy())
      return;

   upd_cores_free();
   upd_alloc_items(0);   /* Eden's row until the cores are in */
   if (!upd_st.list && !(upd_st.list = core_updater_list_init()))
   {
      upd_st.cores_step = CORES_FAILED;
      return;
   }
   core_updater_list_reset(upd_st.list);
   upd_st.cores_step   = CORES_LIST;
   upd_st.list_started = cpu_features_get_time_usec();
   if (!(upd_st.list_task = task_push_get_core_updater_list(upd_st.list,
               true, false)))
      upd_st.cores_step = CORES_FAILED;
}

static void upd_core_download(upd_core_t *c)
{
   settings_t *settings = config_get_ptr();

   c->step     = CORE_DOWNLOAD;
   c->progress = -1;
   if (!(c->task = task_push_core_updater_download(upd_st.list, c->remote,
               c->local_crc, true,
               settings->bools.core_updater_auto_backup,
               settings->uints.core_updater_auto_backup_history_size,
               settings->paths.directory_libretro,
               settings->paths.directory_core_assets)))
   {
      c->step  = CORE_IDLE;
      c->state = GRID_UPDATE_FAILED;
   }
}

static void upd_core_info_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   upd_core_t *c              = (upd_core_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;

   /* Without it the core still installs; it just may not be matched to
    * games until the info files are updated. */
   if (     data && data->status == 200 && data->data && data->len
         && !string_is_empty(c->info_path))
      filestream_write_file(c->info_path, data->data, data->len);

   upd_core_download(c);
   upd_rebuild();
}

static void upd_core_install(upd_core_t *c, const char *running_core)
{
   if (     c->state != GRID_UPDATE_AVAILABLE
         && c->state != GRID_UPDATE_FAILED)
      return;

   if (     !string_is_empty(running_core)
         && string_is_equal_noncase(path_basename(c->path),
               path_basename(running_core)))
   {
      char msg[192];
      snprintf(msg, sizeof(msg), "Close the game to update %s", c->display);
      upd_toast(msg);
      return;
   }

   command_event(CMD_EVENT_NETWORK_INIT, NULL);
   c->state    = GRID_UPDATE_WORKING;
   c->progress = -1;

   /* New core: its .info file first. */
   if (     !c->installed && !string_is_empty(c->info_path)
         && !path_is_valid(c->info_path))
   {
      char url[256];
      snprintf(url, sizeof(url), CORE_INFO_URL "%s_libretro.info", c->base);
      c->step = CORE_INFO;
      if ((c->task = task_push_http_transfer(url, true, "GET",
                  upd_core_info_cb, c)))
         return;
   }

   upd_core_download(c);
}

/* ---- Core info bundle -------------------------------------------------- */

static void upd_info_extract_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   decompress_task_data_t *dec = (decompress_task_data_t*)task_data;

   if (dec)
   {
      free(dec->source_file);
      free(dec);
   }
   filestream_delete(upd_st.info_zip);
   upd_st.info_step = string_is_empty(error) ? INFO_READY : INFO_FAILED;
}

static void upd_info_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;

   if (     !data || data->status != 200 || !data->data || !data->len
         || !path_mkdir(upd_st.info_dir)
         || !filestream_write_file(upd_st.info_zip, data->data, data->len))
   {
      upd_st.info_step = INFO_FAILED;
      return;
   }

   upd_st.info_step = INFO_EXTRACT;
#if defined(HAVE_COMPRESSION) && defined(HAVE_ZLIB)
   if (!task_push_decompress(upd_st.info_zip, upd_st.info_dir, NULL, NULL,
            NULL, upd_info_extract_cb, NULL, NULL, true))
#endif
      upd_st.info_step = INFO_FAILED;
}

/* Fetches the bundle (once a session; a failed try is retried with the
 * next check). */
static void upd_info_fetch(void)
{
   char app_dir[PATH_MAX_LENGTH], url[PATH_MAX_LENGTH];
   const char *base = config_get_ptr()->paths.network_buildbot_assets_url;
   size_t len       = strlen(base);

   if (upd_st.info_step != INFO_NONE && upd_st.info_step != INFO_FAILED)
      return;
   if (!len)
   {
      upd_st.info_step = INFO_FAILED;
      return;
   }

   fill_pathname_application_dir(app_dir, sizeof(app_dir));
   fill_pathname_join_special(upd_st.info_dir, app_dir, "bomba/core-info",
         sizeof(upd_st.info_dir));
   fill_pathname_join_special(upd_st.info_zip, app_dir,
         "bomba/core-info.zip", sizeof(upd_st.info_zip));
   snprintf(url, sizeof(url), "%s%sfrontend/info.zip", base,
         base[len - 1] == '/' ? "" : "/");

   upd_st.info_step = INFO_DOWNLOAD;
   if (!task_push_http_transfer(url, true, "GET", upd_info_cb, NULL))
      upd_st.info_step = INFO_FAILED;
}

/* Names each core and its console from its .info: the bundle's, else
 * (bundle not there) RetroArch's own for installed cores. */
static void upd_info_apply(void)
{
   size_t i;

   if (     upd_st.info_applied || !upd_st.core_count
         || (upd_st.info_step != INFO_READY
          && upd_st.info_step != INFO_FAILED))
      return;
   upd_st.info_applied = true;

   for (i = 0; i < upd_st.core_count; i++)
   {
      upd_core_t *c = &upd_st.cores[i];
      char path[PATH_MAX_LENGTH], name[128], value[128];
      config_file_t *conf = NULL;

      if (upd_st.info_step == INFO_READY)
      {
         snprintf(name, sizeof(name), "%s_libretro.info", c->base);
         fill_pathname_join_special(path, upd_st.info_dir, name,
               sizeof(path));
         conf = config_file_new(path);
      }
      if (!conf && !string_is_empty(c->info_path))
         conf = config_file_new(c->info_path);
      if (!conf)
         continue;

      if (config_get_array(conf, "corename", value, sizeof(value)))
         strlcpy(c->display, value, sizeof(c->display));
      if (config_get_array(conf, "systemname", value, sizeof(value)))
         strlcpy(c->console, value, sizeof(c->console));
      if (config_get_array(conf, "manufacturer", value, sizeof(value)))
         strlcpy(c->maker, value, sizeof(c->maker));
      config_file_free(conf);
   }
   upd_rebuild();
}

/* ---- Public ----------------------------------------------------------- */

void grid_updates_check(bool force, const char *running_core)
{
   retro_time_t now = cpu_features_get_time_usec();

   if (     !force && upd_st.checked
         && now - upd_st.last_check < UPDATES_RECHECK)
      return;
   if (     upd_st.eden_step == EDEN_CHECK
         || upd_st.eden_step == EDEN_DOWNLOAD
         || upd_st.eden_step == EDEN_EXTRACT
         || upd_st.eden_step == EDEN_REMOVE
         || upd_cores_busy())
      return;

   upd_st.checked    = true;
   upd_st.last_check = now;

   command_event(CMD_EVENT_NETWORK_INIT, NULL);
   upd_eden_check();
   upd_cores_check();
   upd_info_fetch();
   /* Keys / firmware dropped into the system folder. */
   grid_switch_import_system(running_core);
   upd_rebuild();
}

bool grid_updates_checking(void)
{
   return upd_st.eden_step == EDEN_CHECK
      || upd_st.cores_step == CORES_LIST || upd_st.cores_step == CORES_CRC;
}

bool grid_updates_cores_failed(void)
{
   return upd_st.cores_step == CORES_FAILED;
}

bool grid_updates_poll(void)
{
   size_t i;
   bool installed;

   upd_info_apply();
   upd_eden_remove_poll();
   if (grid_switch_poll())
      upd_rebuild();

   /* Eden's download progress. */
   if (upd_st.eden_step == EDEN_DOWNLOAD && upd_st.eden_task)
   {
      int p;
      if (     upd_task_running(upd_st.eden_task, &p)
            && p != upd_st.eden_progress)
      {
         upd_st.eden_progress = p;
         upd_rebuild();
      }
   }

   /* The core list: done once its task has gone. */
   if (upd_st.cores_step == CORES_LIST)
   {
      if (!upd_task_running(upd_st.list_task, NULL))
      {
         upd_st.list_task = NULL;
         upd_cores_list_ready();
      }
      else if (cpu_features_get_time_usec() - upd_st.list_started
            > UPDATES_TIMEOUT)
      {
         upd_st.cores_step = CORES_FAILED;
         upd_rebuild();
      }
   }

   /* CRCs worked out: which installed cores are behind. */
   if (     upd_st.cores_step == CORES_CRC && upd_st.crc_job
         && upd_st.crc_job->done)
   {
      sthread_join(upd_st.crc_job->thread);
      free(upd_st.crc_job);
      upd_st.crc_job = NULL;
      for (i = 0; i < upd_st.core_count; i++)
      {
         upd_core_t *c = &upd_st.cores[i];
         if (!c->installed || c->locked)
            continue;
         if (c->local_crc && c->local_crc == c->remote_crc)
         {
            c->section = GRID_SECTION_INSTALLED;
            c->state   = GRID_UPDATE_CURRENT;
         }
         else
         {
            c->section = GRID_SECTION_UPDATES;
            c->state   = GRID_UPDATE_AVAILABLE;
         }
      }
      upd_st.cores_step = CORES_READY;
      upd_rebuild();
   }

   /* Core downloads: done once their task has gone (it waits for its
    * extraction and reloads core info itself); failed if the core on
    * disk still isn't the buildbot's. */
   for (i = 0; i < upd_st.core_count; i++)
   {
      upd_core_t *c = &upd_st.cores[i];
      int p;

      if (c->state != GRID_UPDATE_WORKING || c->step != CORE_DOWNLOAD)
         continue;
      if (upd_task_running(c->task, &p))
      {
         if (p != c->progress)
         {
            c->progress = p;
            upd_rebuild();
         }
         continue;
      }

      c->task      = NULL;
      c->step      = CORE_IDLE;
      c->local_crc = upd_file_crc(c->path);
      if (c->local_crc && c->local_crc == c->remote_crc)
      {
         c->state         = GRID_UPDATE_DONE;
         c->section       = GRID_SECTION_INSTALLED;
         c->installed     = true;
         c->steam         = false; /* the buildbot's build now */
         upd_st.installed = true;
      }
      else
         c->state = GRID_UPDATE_FAILED;
      upd_rebuild();
   }

   installed        = upd_st.installed;
   upd_st.installed = false;
   return installed;
}

size_t grid_updates_count(void) { return upd_st.item_count; }

const grid_update_t *grid_updates_get(size_t idx)
{
   return idx < upd_st.item_count ? &upd_st.items[idx] : NULL;
}

int grid_updates_pick(size_t idx)
{
   if (idx >= upd_st.item_count)
      return 0;
   return upd_st.item_core[idx] == -2 ? 1
        : upd_st.item_core[idx] == -3 ? 2 : 0;
}

unsigned grid_updates_available(void)
{
   size_t i;
   unsigned n = 0;
   for (i = 0; i < upd_st.item_count; i++)
      if (     upd_st.items[i].section == GRID_SECTION_UPDATES
            && upd_st.items[i].state == GRID_UPDATE_AVAILABLE)
         n++;
   return n;
}

void grid_updates_install(size_t idx, const char *running_core)
{
   int core;

   if (idx >= upd_st.item_count)
      return;
   core = upd_st.item_core[idx];

   if (core == -1)
      upd_eden_install(running_core);
   else if ((size_t)core < upd_st.core_count)
      upd_core_install(&upd_st.cores[core], running_core);

   upd_rebuild();
}

void grid_updates_install_all(const char *running_core)
{
   size_t i;

   if (     upd_st.eden_section == GRID_SECTION_UPDATES
         && upd_st.eden_state == GRID_UPDATE_AVAILABLE)
      upd_eden_install(running_core);
   for (i = 0; i < upd_st.core_count; i++)
      if (     upd_st.cores[i].section == GRID_SECTION_UPDATES
            && upd_st.cores[i].state == GRID_UPDATE_AVAILABLE)
         upd_core_install(&upd_st.cores[i], running_core);
   upd_rebuild();
}

bool grid_updates_remove(size_t idx, const char *running_core)
{
   upd_core_t *c;
   char msg[192];
   int core;

   if (idx >= upd_st.item_count || !upd_st.items[idx].removable)
      return false;
   core = upd_st.item_core[idx];
   if (core == -1)
      return upd_eden_remove(running_core);
   if (core < 0)
      return false;
   if ((size_t)core >= upd_st.core_count)
      return false;
   c = &upd_st.cores[core];

   if (     !string_is_empty(running_core)
         && string_is_equal_noncase(path_basename(c->path),
               path_basename(running_core)))
   {
      snprintf(msg, sizeof(msg), "Close the game to remove %s", c->display);
      upd_toast(msg);
      return false;
   }

   if (filestream_delete(c->path) != 0 && path_is_valid(c->path))
   {
      snprintf(msg, sizeof(msg), "Couldn't remove %s", c->display);
      upd_toast(msg);
      return false;
   }
   /* Its .info too, so RetroArch forgets it (installing fetches it
    * again). */
   if (!string_is_empty(c->info_path))
      filestream_delete(c->info_path);

   /* Steam reinstalls its DLC cores until the DLC is uninstalled. */
   snprintf(msg, sizeof(msg), c->steam
         ? "Removed %s (uninstall its DLC in Steam to keep it off)"
         : "Removed %s", c->display);
   upd_toast(msg);

   c->installed     = false;
   c->steam         = false;
   c->locked        = false;
   c->local_crc     = 0;
   c->section       = GRID_SECTION_INSTALL;
   c->state         = GRID_UPDATE_AVAILABLE;
   upd_st.installed = true; /* core info and the games list change */
   upd_rebuild();
   return true;
}

#else

bool grid_updates_remove(size_t idx, const char *running_core) { return false; }
void grid_updates_check(bool force, const char *running_core) { }
int grid_updates_pick(size_t idx) { return 0; }
bool grid_updates_checking(void) { return false; }
bool grid_updates_cores_failed(void) { return false; }
bool grid_updates_poll(void) { return false; }
size_t grid_updates_count(void) { return 0; }
const grid_update_t *grid_updates_get(size_t idx) { return NULL; }
unsigned grid_updates_available(void) { return 0; }
void grid_updates_install(size_t idx, const char *running_core) { }
void grid_updates_install_all(const char *running_core) { }

#endif
