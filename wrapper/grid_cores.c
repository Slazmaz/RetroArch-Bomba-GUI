/* RetroArchWrapper: install missing cores automatically.
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
 */

#ifdef HAVE_CONFIG_H
#include "../config.h"   /* HAVE_UPDATE_CORES comes from here */
#endif

#include <stdio.h>
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

enum core_step
{
   STEP_QUEUED = 0,
   STEP_INFO,      /* .info download in flight */
   STEP_LIST,      /* waiting for the core updater list */
   STEP_CORE,      /* core download in flight */
   STEP_DONE,      /* installed */
   STEP_FAILED     /* unavailable or download failed */
};

typedef struct
{
   char core[64];        /* e.g. "dolphin" */
   char display[128];    /* updater's display name, to find its task */
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

enum grid_core_state grid_cores_state(const char *rom_path)
{
   const char *core = cores_for_file(rom_path);
   core_job_t *job  = core ? cores_find_job(core) : NULL;

   if (!job)
      return GRID_CORE_NONE;
   if (job->step == STEP_FAILED)
      return GRID_CORE_FAILED;
   if (job->step == STEP_DONE)
      return GRID_CORE_NONE; /* installed: the core list has it now */
   return GRID_CORE_INSTALLING;
}

void grid_cores_request(const char *rom_path)
{
   const char *core = cores_for_file(rom_path);

   if (!core || cores_find_job(core))
      return; /* unknown system, or already queued/attempted */

   if (cores_st.count >= sizeof(cores_st.jobs) / sizeof(cores_st.jobs[0]))
      return;

   /* If the core is already installed (its .info was missing or didn't
    * list the extension), the info step still runs and the download
    * step is skipped. */
   strlcpy(cores_st.jobs[cores_st.count].core, core,
         sizeof(cores_st.jobs[0].core));
   cores_st.jobs[cores_st.count].step = STEP_QUEUED;
   cores_st.count++;
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
      job->step        = STEP_DONE;
      cores_st.changed = true;
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

int grid_cores_progress(const char *rom_path)
{
   const char *core = cores_for_file(rom_path);
   core_job_t *job  = core ? cores_find_job(core) : NULL;

   if (!job || job->step != STEP_CORE)
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
               job->step        = STEP_DONE;
               cores_st.changed = true;
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
enum grid_core_state grid_cores_state(const char *rom_path)
{
   return GRID_CORE_NONE;
}
int grid_cores_progress(const char *rom_path) { return -1; }
bool grid_cores_poll(void) { return false; }

#endif
