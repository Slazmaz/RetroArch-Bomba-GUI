/* Bomba: install missing cores automatically.
 *
 * When the game grid finds files that no installed core can open, and the
 * extension clearly belongs to one system, the preferred core for it is
 * downloaded in the background:
 *   1. its .info file from libretro/libretro-core-info (tells RetroArch
 *      which extensions the core handles),
 *   2. the core itself through RetroArch's core updater (buildbot URL from
 *      settings), which shows a progress notification and reloads core
 *      info when done.
 * One core at a time; each is attempted at most once per session.
 * Extensions shared by several systems (iso, cue, chd, bin, zip, ...) are
 * deliberately not mapped.
 *
 * Cores that need system files (Dolphin, PPSSPP, blueMSX) then get them
 * the way the menu's Core System Files Downloader does: the pack from
 * <assets url>/system/ is extracted into the system directory. This also
 * runs for cores that were already installed, whenever the grid finds a
 * game for one and the pack's files are missing.
 */

#ifdef HAVE_CONFIG_H
#include "../config.h"   /* HAVE_UPDATE_CORES comes from here */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <features/features_cpu.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "grid_cores.h"
#include "../command.h"
#include "../configuration.h"

#if defined(HAVE_NETWORKING) && defined(HAVE_UPDATE_CORES)

#include "../core_updater_list.h"
#include "../tasks/tasks_internal.h"

#define CORE_INFO_URL "https://raw.githubusercontent.com/libretro/libretro-core-info/master/"

#if defined(_WIN32)
#define CORE_EXT ".dll"
#elif defined(__APPLE__)
#define CORE_EXT ".dylib"
#else
#define CORE_EXT ".so"
#endif

/* Extension -> preferred core (base name, without _libretro<ext>). */
static const struct { const char *ext; const char *core; } core_map[] = {
   { "z64",  "mupen64plus_next" }, { "n64",  "mupen64plus_next" },
   { "v64",  "mupen64plus_next" }, { "ndd",  "mupen64plus_next" },
   { "rvz",  "dolphin" },          { "gcz",  "dolphin" },
   { "gcm",  "dolphin" },          { "wbfs", "dolphin" },
   { "ciso", "dolphin" },          { "wad",  "dolphin" },
   { "nds",  "melondsds" },        { "dsi",  "melondsds" },
   { "cso",  "ppsspp" },           { "pbp",  "ppsspp" },
   { "3ds",  "citra" },            { "cci",  "citra" },
   { "gb",   "gambatte" },         { "gbc",  "gambatte" },
   { "gba",  "mgba" },
   { "nes",  "fceumm" },           { "fds",  "fceumm" },
   { "sfc",  "snes9x" },           { "smc",  "snes9x" },
   { "md",   "genesis_plus_gx" },  { "gen",  "genesis_plus_gx" },
   { "smd",  "genesis_plus_gx" },  { "sms",  "genesis_plus_gx" },
   { "gg",   "genesis_plus_gx" },  { "32x",  "picodrive" },
   { "a26",  "stella" },           { "a78",  "prosystem" },
   { "lnx",  "handy" },            { "pce",  "mednafen_pce_fast" },
   { "ws",   "mednafen_wswan" },   { "wsc",  "mednafen_wswan" },
   { "vb",   "mednafen_vb" },      { "ngp",  "mednafen_ngp" },
   { "ngc",  "mednafen_ngp" },     { "col",  "bluemsx" }
};

/* Core -> its system files pack (<assets url>/system/<zip>), and a file
 * the pack installs, to tell whether it is already in place. */
static const struct { const char *core, *zip, *marker; } sys_map[] = {
   { "dolphin", "Dolphin.zip", "dolphin-emu/Sys/codehandler.bin" },
   { "ppsspp",  "PPSSPP.zip",  "PPSSPP/ppge_atlas.zim" },
   { "bluemsx", "blueMSX.zip", "Databases/msxromdb.xml" }
};

enum core_step
{
   STEP_QUEUED = 0,
   STEP_INFO,        /* .info download in flight */
   STEP_LIST,        /* waiting for the core updater list */
   STEP_CORE,        /* core download in flight */
   STEP_SYS_QUEUED,  /* core installed; its system files are next */
   STEP_SYS_DL,      /* system files pack download in flight */
   STEP_SYS_EXTRACT, /* pack being extracted */
   STEP_DONE,        /* installed */
   STEP_FAILED       /* unavailable or download failed */
};

typedef struct
{
   char core[64];        /* e.g. "dolphin" */
   char display[128];    /* updater's display name, to find its task */
   char zip[PATH_MAX_LENGTH]; /* downloaded system files pack */
   void *task;           /* the pack's HTTP task, to read its progress */
   enum core_step step;
   int progress;         /* 0-100 while downloading, -1 if unknown */
   retro_time_t started; /* when the core download began */
} core_job_t;

/* Give up on a core download that hasn't produced the core by then. */
#define CORE_DOWNLOAD_TIMEOUT_USEC (5LL * 60 * 1000000)
/* How long the core list may keep filling after its first entries. */
#define CORE_LIST_SETTLE_USEC      (15LL * 1000000)

static struct
{
   core_job_t jobs[32];
   unsigned count;
   bool list_requested;
   retro_time_t list_started;
   retro_time_t list_ready;   /* when the list first had entries */
   bool changed;     /* a job finished (installed or failed) since last poll */
} cores_st;

static void cores_paths(char *core_file, size_t core_len,
      char *core_path, size_t path_len, const char *core)
{
   settings_t *settings = config_get_ptr();
   snprintf(core_file, core_len, "%s_libretro%s", core, CORE_EXT);
   fill_pathname_join_special(core_path, settings->paths.directory_libretro,
         core_file, path_len);
}

static void cores_info_path(char *s, size_t len, const char *core)
{
   char name[96];
   settings_t *settings = config_get_ptr();
   const char *dir      = settings->paths.path_libretro_info;

   if (string_is_empty(dir))
      dir = settings->paths.directory_libretro;

   snprintf(name, sizeof(name), "%s_libretro.info", core);
   fill_pathname_join_special(s, dir, name, len);
}

/* Preferred core for a file's extension, or NULL if ambiguous/unknown. */
static const char *cores_for_file(const char *rom_path)
{
   size_t i;
   const char *ext = path_get_extension(rom_path);

   if (string_is_empty(ext))
      return NULL;

   for (i = 0; i < sizeof(core_map) / sizeof(core_map[0]); i++)
      if (string_is_equal_noncase(ext, core_map[i].ext))
         return core_map[i].core;

   return NULL;
}

static core_job_t *cores_find_job(const char *core)
{
   unsigned j;
   for (j = 0; j < cores_st.count; j++)
      if (string_is_equal(cores_st.jobs[j].core, core))
         return &cores_st.jobs[j];
   return NULL;
}

/* Base name of a core file ("dolphin" for ".../dolphin_libretro.dll"). */
static bool cores_name(char *s, size_t len, const char *core_path)
{
   const char *base = string_is_empty(core_path)
      ? NULL : path_basename(core_path);
   const char *end  = base ? strstr(base, "_libretro") : NULL;
   size_t n;

   if (!end || end == base)
      return false;
   n = (size_t)(end - base);
   if (n >= len)
      return false;
   memcpy(s, base, n);
   s[n] = '\0';
   return true;
}

/* System files pack a core needs, or -1 if none. */
static int cores_sys_index(const char *core)
{
   size_t i;
   for (i = 0; i < sizeof(sys_map) / sizeof(sys_map[0]); i++)
      if (string_is_equal(sys_map[i].core, core))
         return (int)i;
   return -1;
}

/* Whether a core needs system files that aren't installed. */
static bool cores_sys_missing(const char *core)
{
#if defined(HAVE_COMPRESSION) && defined(HAVE_ZLIB)
   char path[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();
   int i                = cores_sys_index(core);

   if (i < 0 || string_is_empty(settings->paths.directory_system))
      return false;
   fill_pathname_join_special(path, settings->paths.directory_system,
         sys_map[i].marker, sizeof(path));
   return !path_is_valid(path);
#else
   return false; /* can't extract the pack */
#endif
}

/* The job for a game: by the core it runs with, else by extension. */
static core_job_t *cores_job_for(const char *rom_path, const char *core_path)
{
   char name[64];
   const char *core = NULL;

   if (!string_is_empty(core_path))
   {
      if (cores_name(name, sizeof(name), core_path))
         core = name;
   }
   else
      core = cores_for_file(rom_path);

   return core ? cores_find_job(core) : NULL;
}

enum grid_core_state grid_cores_state(const char *rom_path,
      const char *core_path)
{
   core_job_t *job = cores_job_for(rom_path, core_path);

   if (!job)
      return GRID_CORE_NONE;
   if (job->step == STEP_FAILED)
      return GRID_CORE_FAILED;
   if (job->step == STEP_DONE)
      return GRID_CORE_NONE; /* installed: the core list has it now */
   return GRID_CORE_INSTALLING;
}

static void cores_add_job(const char *core, enum core_step step)
{
   if (cores_st.count >= sizeof(cores_st.jobs) / sizeof(cores_st.jobs[0]))
      return;

   strlcpy(cores_st.jobs[cores_st.count].core, core,
         sizeof(cores_st.jobs[0].core));
   cores_st.jobs[cores_st.count].step = step;
   cores_st.count++;
}

void grid_cores_request(const char *rom_path)
{
   const char *core = cores_for_file(rom_path);

   if (!core || cores_find_job(core))
      return; /* unknown system, or already queued/attempted */

   /* If the core is already installed (its .info was missing or didn't
    * list the extension), the info step still runs and the download
    * step is skipped. */
   cores_add_job(core, STEP_QUEUED);
}

void grid_cores_request_files(const char *core_path)
{
   char core[64];

   if (     !cores_name(core, sizeof(core), core_path)
         || cores_find_job(core)
         || !cores_sys_missing(core))
      return;

   cores_add_job(core, STEP_SYS_QUEUED);
}

/* The core is in place: fetch its system files next if it needs them. */
static void cores_installed(core_job_t *job)
{
   job->step        = cores_sys_missing(job->core)
      ? STEP_SYS_QUEUED : STEP_DONE;
   cores_st.changed = true;
}

static void cores_sys_end(core_job_t *job, bool ok)
{
   if (!string_is_empty(job->zip) && path_is_valid(job->zip))
      filestream_delete(job->zip);
   job->step        = ok ? STEP_DONE : STEP_FAILED;
   cores_st.changed = true;
}

#if defined(HAVE_COMPRESSION) && defined(HAVE_ZLIB)
static void cores_extract_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   core_job_t *job             = (core_job_t*)user_data;
   decompress_task_data_t *dec = (decompress_task_data_t*)task_data;

   if (dec)
   {
      free(dec->source_file);
      free(dec);
   }

   cores_sys_end(job, !error && !cores_sys_missing(job->core));
}
#endif

static void cores_sys_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   core_job_t *job            = (core_job_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   settings_t *settings       = config_get_ptr();
   const char *dir            = settings->paths.directory_system;
   int i                      = cores_sys_index(job->core);

   job->task = NULL;

   if (     !data || data->status != 200 || !data->data || !data->len
         || i < 0 || string_is_empty(dir) || !path_mkdir(dir))
   {
      cores_sys_end(job, false);
      return;
   }

   fill_pathname_join_special(job->zip, dir, sys_map[i].zip,
         sizeof(job->zip));
   if (!filestream_write_file(job->zip, data->data, data->len))
   {
      cores_sys_end(job, false);
      return;
   }

#if defined(HAVE_COMPRESSION) && defined(HAVE_ZLIB)
   job->step     = STEP_SYS_EXTRACT;
   job->progress = -1;
   if (!task_push_decompress(job->zip, dir, NULL, NULL, NULL,
            cores_extract_cb, job, NULL, true))
#endif
      cores_sys_end(job, false);
}

static void cores_sys_start(core_job_t *job)
{
   char url[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();
   const char *base     = settings->paths.network_buildbot_assets_url;
   size_t len           = strlen(base);
   int i                = cores_sys_index(job->core);

   if (i < 0 || !len)
   {
      cores_sys_end(job, false);
      return;
   }

   snprintf(url, sizeof(url), "%s%ssystem/%s", base,
         base[len - 1] == '/' ? "" : "/", sys_map[i].zip);

   job->zip[0]   = '\0';
   job->step     = STEP_SYS_DL;
   job->progress = -1;

   /* Muted: the grid shows progress on the game tiles instead. */
   command_event(CMD_EVENT_NETWORK_INIT, NULL);
   if (!(job->task = task_push_http_transfer(url, true, "GET",
               cores_sys_cb, job)))
      cores_sys_end(job, false);
}

/* Task finder: copies the progress of a job's system files download. */
static bool cores_find_sys_task(retro_task_t *task, void *userdata)
{
   core_job_t *job = (core_job_t*)userdata;

   if (task && task == job->task)
   {
      job->progress = task->progress;
      return true;
   }
   return false;
}

static void cores_info_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   core_job_t *job            = (core_job_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;

   if (data && data->status == 200 && data->data && data->len)
   {
      char path[PATH_MAX_LENGTH];
      cores_info_path(path, sizeof(path), job->core);
      filestream_write_file(path, data->data, data->len);
   }

   /* Continue to the core either way: an info failure only means the
    * core may not be matched to files until info files are updated. */
   job->step = STEP_LIST;
}

static void cores_start(core_job_t *job)
{
   char url[256];
   char info_path[PATH_MAX_LENGTH];

   cores_info_path(info_path, sizeof(info_path), job->core);
   if (path_is_valid(info_path))
   {
      job->step = STEP_LIST;
      return;
   }

   snprintf(url, sizeof(url), CORE_INFO_URL "%s_libretro.info", job->core);
   job->step = STEP_INFO;
   if (!task_push_http_transfer(url, true, "GET", cores_info_cb, job))
      job->step = STEP_LIST;
}

/* Returns true once the job has left STEP_LIST. */
static bool cores_download(core_job_t *job)
{
   char core_file[96];
   char core_path[PATH_MAX_LENGTH];
   char remote[128];
   settings_t *settings      = config_get_ptr();
   core_updater_list_t *list = NULL;

   cores_paths(core_file, sizeof(core_file), core_path, sizeof(core_path),
         job->core);

   /* Already installed (e.g. only the info file was missing). */
   if (path_is_valid(core_path))
   {
      command_event(CMD_EVENT_CORE_INFO_INIT, NULL);
      cores_installed(job);
      return true;
   }

   /* Use RetroArch's cached core list (the menu's Core Downloader shares
    * it); create it once. */
   if (!(list = core_updater_list_get_cached()))
   {
      if (!core_updater_list_init_cached())
      {
         job->step        = STEP_FAILED;
         cores_st.changed = true;
         return true;
      }
      list = core_updater_list_get_cached();
   }

   /* Fetch the buildbot's core list once; wait for it to fill. */
   if (core_updater_list_size(list) == 0)
   {
      if (!cores_st.list_requested)
      {
         cores_st.list_requested = true;
         cores_st.list_started   = cpu_features_get_time_usec();
         command_event(CMD_EVENT_NETWORK_INIT, NULL);
         task_push_get_core_updater_list(list, true, false);
      }
      else if (cpu_features_get_time_usec() - cores_st.list_started
            > CORE_DOWNLOAD_TIMEOUT_USEC)
      {
         job->step        = STEP_FAILED; /* buildbot unreachable */
         cores_st.changed = true;
      }
      return false;
   }

   if (!cores_st.list_ready)
      cores_st.list_ready = cpu_features_get_time_usec();

   snprintf(remote, sizeof(remote), "%s.zip", core_file);

   /* The list is filled on a task thread (alphabetically), so right after
    * it becomes non-empty this core may not be in it yet: keep waiting
    * for a while before deciding the buildbot doesn't have it. */
   {
      const core_updater_list_entry_t *entry = NULL;
      if (    !core_updater_list_get_filename(list, remote, &entry)
            || !entry)
      {
         if (cpu_features_get_time_usec() - cores_st.list_ready
               > CORE_LIST_SETTLE_USEC)
         {
            job->step        = STEP_FAILED; /* not on the buildbot */
            cores_st.changed = true;
         }
         return false;
      }
      if (entry->display_name)
         strlcpy(job->display, entry->display_name, sizeof(job->display));
   }

   job->step     = STEP_CORE;
   job->started  = cpu_features_get_time_usec();
   job->progress = -1;

   /* Muted: the grid shows progress on the game tiles instead. */
   if (!task_push_core_updater_download(list, remote, 0, true,
            false, settings->uints.core_updater_auto_backup_history_size,
            settings->paths.directory_libretro,
            settings->paths.directory_core_assets))
   {
      job->step        = STEP_FAILED;
      cores_st.changed = true;
   }

   return true;
}

/* Task finder: copies the progress of the updater task whose title
 * ("Downloading core: <name>", ...) names this job's core. */
static bool cores_find_task(retro_task_t *task, void *userdata)
{
   core_job_t *job = (core_job_t*)userdata;

   if (     task && task->title && !string_is_empty(job->display)
         && strstr(task->title, job->display))
   {
      job->progress = task->progress;
      return true;
   }
   return false;
}

int grid_cores_progress(const char *rom_path, const char *core_path)
{
   core_job_t *job = cores_job_for(rom_path, core_path);

   if (!job || (job->step != STEP_CORE && job->step != STEP_SYS_DL))
      return -1;
   return job->progress;
}

bool grid_cores_poll(void)
{
   unsigned i;
   bool changed;

   for (i = 0; i < cores_st.count; i++)
   {
      core_job_t *job = &cores_st.jobs[i];

      switch (job->step)
      {
         case STEP_INFO:
            return false; /* one at a time */
         case STEP_LIST:
            cores_download(job);
            return false;
         case STEP_CORE:
         {
            char core_file[96];
            char core_path[PATH_MAX_LENGTH];
            task_finder_data_t find;

            find.func     = cores_find_task;
            find.userdata = job;
            task_queue_find(&find);

            cores_paths(core_file, sizeof(core_file), core_path,
                  sizeof(core_path), job->core);
            if (path_is_valid(core_path))
            {
               /* Reload core info now so the caller's rescan sees the
                * core (the updater task also does this when it ends). */
               command_event(CMD_EVENT_CORE_INFO_INIT, NULL);
               cores_installed(job);
            }
            else if (cpu_features_get_time_usec() - job->started
                  > CORE_DOWNLOAD_TIMEOUT_USEC)
            {
               job->step        = STEP_FAILED; /* move on */
               cores_st.changed = true;
            }
            return false;
         }
         case STEP_QUEUED:
            cores_start(job);
            return false;
         case STEP_SYS_QUEUED:
            cores_sys_start(job);
            return false;
         case STEP_SYS_DL:
         {
            task_finder_data_t find;
            find.func     = cores_find_sys_task;
            find.userdata = job;
            task_queue_find(&find);
            return false;
         }
         case STEP_SYS_EXTRACT:
            return false;
         case STEP_DONE:
         case STEP_FAILED:
            break;
      }
   }

   changed          = cores_st.changed;
   cores_st.changed = false;
   return changed;
}

#else

void grid_cores_request(const char *rom_path) { }
void grid_cores_request_files(const char *core_path) { }
enum grid_core_state grid_cores_state(const char *rom_path,
      const char *core_path)
{
   return GRID_CORE_NONE;
}
int grid_cores_progress(const char *rom_path, const char *core_path)
{
   return -1;
}
bool grid_cores_poll(void) { return false; }

#endif
