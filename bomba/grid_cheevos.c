/* Bomba: a game's achievements, for the game menu.
 *
 * Fetches the game's achievement set, then the player's unlocks, from
 * RetroAchievements with the login in settings. Badges are cached in
 *    <thumbnails>/RetroAchievements Grid/badges/
 * (the locked look for achievements not yet unlocked) and downloaded one
 * at a time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "grid_cheevos.h"
#include "../configuration.h"

#define CHEEVOS_MAX 512

#if defined(HAVE_CHEEVOS) && defined(HAVE_NETWORKING)

#include <rc_api_runtime.h>
#include <rc_api_user.h>

#include "../tasks/tasks_internal.h"
#include "../cheevos/cheevos_locals.h"
#include "../cheevos/cheevos_client.h"

typedef struct
{
   grid_cheevo_t info;
   unsigned id;
   char url_locked[512];
   char url_unlocked[512];
   char badge_url[512];   /* the look to show, picked once unlocks are in */
   bool badge_tried;
} cheevos_entry_t;

static struct
{
   cheevos_entry_t *entries;
   size_t count;
   unsigned game_id;
   unsigned generation;   /* bumps per open, to drop stale replies */
   enum grid_cheevos_state state;
   bool badge_busy;
   bool changed;
   char dir[PATH_MAX_LENGTH];
} cheevos_st;

/* Progress per game (unlocked of total), for the home screen; looked up
 * one game at a time: its set (core achievements), then its unlocks.
 * Cached in <thumbnails>/RetroAchievements Grid/progress.txt so counts
 * show at once next launch; a game is only looked up again (when next
 * selected) after it was played. */
#define PROGRESS_MAX 1024

typedef struct
{
   unsigned game_id;
   unsigned unlocked;
   unsigned total;        /* 0: lookup failed or no achievements */
   bool stale;            /* played since looked up */
} progress_entry_t;

static struct
{
   progress_entry_t entries[PROGRESS_MAX];
   size_t count;
   size_t next;           /* slot to reuse once full */
   unsigned *ids;         /* core achievement ids of the game being looked up */
   unsigned id_count;
   unsigned busy_id;      /* game being looked up, 0: none */
   unsigned generation;   /* bumps per reset, to drop stale replies */
   bool loaded;           /* cache file read */
} progress_st;

static bool cheevos_login(const char **user, const char **token)
{
   settings_t *settings = config_get_ptr();
   *user  = settings->arrays.cheevos_username;
   *token = settings->arrays.cheevos_token;
   return !string_is_empty(*user) && !string_is_empty(*token);
}

/* Cache file for a badge URL: its file name in the badges folder. */
static void cheevos_badge_path(const char *url, char *s, size_t len)
{
   fill_pathname_join_special(s, cheevos_st.dir, path_basename(url), len);
}

static void cheevos_server_response(rc_api_server_response_t *r,
      http_transfer_data_t *data)
{
   memset(r, 0, sizeof(*r));
   r->body             = data->data;
   r->body_length      = data->len;
   r->http_status_code = data->status;
}

static void progress_store(unsigned game_id, unsigned unlocked,
      unsigned total);

/* Picks each achievement's badge look now that unlocks are known: the
 * unlocked badge, or the locked (greyed) one. The game's progress is
 * updated from them too, unless the unlocks couldn't be fetched. */
static void cheevos_finish_list(bool unlocks_ok)
{
   size_t i;
   unsigned unlocked = 0;
   for (i = 0; i < cheevos_st.count; i++)
   {
      cheevos_entry_t *e = &cheevos_st.entries[i];
      char path[PATH_MAX_LENGTH];

      if (e->info.unlocked)
         unlocked++;

      strlcpy(e->badge_url, e->info.unlocked ? e->url_unlocked
            : e->url_locked, sizeof(e->badge_url));
      if (string_is_empty(e->badge_url))
         continue;
      cheevos_badge_path(e->badge_url, path, sizeof(path));
      if (path_is_valid(path))
         strlcpy(e->info.badge, path, sizeof(e->info.badge));
   }
   if (unlocks_ok)
      progress_store(cheevos_st.game_id, unlocked,
            (unsigned)cheevos_st.count);
   cheevos_st.state   = GRID_CHEEVOS_READY;
   cheevos_st.changed = true;
}

static void cheevos_unlocks_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   size_t i, k;
   bool ok = false;
   rc_api_server_response_t server_response;
   rc_api_fetch_user_unlocks_response_t response;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;

   if ((uintptr_t)user_data != cheevos_st.generation)
      return; /* another game was opened meanwhile */

   /* Without unlocks, still show the list (all as locked). */
   if (data && data->data && data->status == 200)
   {
      cheevos_server_response(&server_response, data);
      if (     rc_api_process_fetch_user_unlocks_server_response(&response,
                  &server_response) == RC_OK
            && response.response.succeeded)
      {
         ok = true;
         for (k = 0; k < response.num_achievement_ids; k++)
            for (i = 0; i < cheevos_st.count; i++)
               if (cheevos_st.entries[i].id == response.achievement_ids[k])
                  cheevos_st.entries[i].info.unlocked = true;
      }
      rc_api_destroy_fetch_user_unlocks_response(&response);
   }

   cheevos_finish_list(ok);
}

static void cheevos_game_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   size_t i;
   char ua[256];
   const char *user, *token;
   rc_api_request_t request;
   rc_api_server_response_t server_response;
   rc_api_fetch_game_data_response_t response;
   rc_api_fetch_user_unlocks_request_t params;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   unsigned gen               = (unsigned)(uintptr_t)user_data;

   if (gen != cheevos_st.generation)
      return;

   if (!data || !data->data || data->status != 200)
   {
      cheevos_st.state   = GRID_CHEEVOS_ERROR;
      cheevos_st.changed = true;
      return;
   }

   cheevos_server_response(&server_response, data);
   if (     rc_api_process_fetch_game_data_server_response(&response,
               &server_response) != RC_OK
         || !response.response.succeeded)
   {
      rc_api_destroy_fetch_game_data_response(&response);
      cheevos_st.state   = GRID_CHEEVOS_ERROR;
      cheevos_st.changed = true;
      return;
   }

   /* Core achievements only (not unofficial ones). */
   cheevos_st.count = 0;
   for (i = 0; i < response.num_achievements
         && cheevos_st.count < CHEEVOS_MAX; i++)
   {
      const rc_api_achievement_definition_t *a = &response.achievements[i];
      cheevos_entry_t *e = &cheevos_st.entries[cheevos_st.count];

      if (a->category != RC_ACHIEVEMENT_CATEGORY_CORE)
         continue;

      memset(e, 0, sizeof(*e));
      e->id          = a->id;
      e->info.points = a->points;
      e->info.rarity = a->rarity;
      strlcpy(e->info.title, a->title ? a->title : "",
            sizeof(e->info.title));
      strlcpy(e->info.description, a->description ? a->description : "",
            sizeof(e->info.description));
      strlcpy(e->url_locked, a->badge_locked_url ? a->badge_locked_url : "",
            sizeof(e->url_locked));
      strlcpy(e->url_unlocked, a->badge_url ? a->badge_url : "",
            sizeof(e->url_unlocked));
      cheevos_st.count++;
   }
   rc_api_destroy_fetch_game_data_response(&response);

   /* Then which of them the player has unlocked. */
   if (!cheevos_login(&user, &token))
   {
      cheevos_finish_list(false);
      return;
   }

   memset(&params, 0, sizeof(params));
   params.username = user;
   params.api_token = token;
   params.game_id  = cheevos_st.game_id;
   params.hardcore = 0; /* softcore unlocks include hardcore ones */

   if (rc_api_init_fetch_user_unlocks_request(&request, &params) != RC_OK)
   {
      cheevos_finish_list(false);
      return;
   }

   rcheevos_get_user_agent(get_rcheevos_locals(), ua, sizeof(ua));
   if (!task_push_http_post_transfer_with_user_agent(request.url,
            request.post_data, true, "POST", ua, cheevos_unlocks_cb,
            (void*)(uintptr_t)gen))
      cheevos_finish_list(false);
   rc_api_destroy_request(&request);
}

void grid_cheevos_open(unsigned game_id)
{
   char ua[256];
   char ra_dir[PATH_MAX_LENGTH];
   const char *user, *token;
   rc_api_request_t request;
   rc_api_fetch_game_data_request_t params;
   settings_t *settings = config_get_ptr();

   cheevos_st.generation++;
   cheevos_st.count   = 0;
   cheevos_st.game_id = game_id;
   cheevos_st.changed = true;

   if (!cheevos_st.entries)
      cheevos_st.entries = (cheevos_entry_t*)calloc(CHEEVOS_MAX,
            sizeof(cheevos_entry_t));

   if (!cheevos_login(&user, &token))
   {
      cheevos_st.state = GRID_CHEEVOS_LOGGED_OUT;
      return;
   }
   if (!game_id)
   {
      cheevos_st.state = GRID_CHEEVOS_NO_GAME;
      return;
   }
   if (!cheevos_st.entries)
   {
      cheevos_st.state = GRID_CHEEVOS_ERROR;
      return;
   }

   if (!string_is_empty(settings->paths.directory_thumbnails))
   {
      fill_pathname_join_special(ra_dir,
            settings->paths.directory_thumbnails, "RetroAchievements Grid",
            sizeof(ra_dir));
      fill_pathname_join_special(cheevos_st.dir, ra_dir, "badges",
            sizeof(cheevos_st.dir));
   }

   memset(&params, 0, sizeof(params));
   params.username  = user;
   params.api_token = token;
   params.game_id   = game_id;

   if (rc_api_init_fetch_game_data_request(&request, &params) != RC_OK)
   {
      cheevos_st.state = GRID_CHEEVOS_ERROR;
      return;
   }

   cheevos_st.state = GRID_CHEEVOS_LOADING;
   rcheevos_get_user_agent(get_rcheevos_locals(), ua, sizeof(ua));
   if (!task_push_http_post_transfer_with_user_agent(request.url,
            request.post_data, true, "POST", ua, cheevos_game_cb,
            (void*)(uintptr_t)cheevos_st.generation))
      cheevos_st.state = GRID_CHEEVOS_ERROR;
   rc_api_destroy_request(&request);
}

static void cheevos_badge_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   size_t idx                 = (size_t)(uintptr_t)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   char path[PATH_MAX_LENGTH];
   cheevos_entry_t *e;

   cheevos_st.badge_busy = false;

   if (     idx >= cheevos_st.count || !data || data->status != 200
         || !data->data || !data->len)
      return;

   e = &cheevos_st.entries[idx];
   path_mkdir(cheevos_st.dir);
   cheevos_badge_path(e->badge_url, path, sizeof(path));
   if (filestream_write_file(path, data->data, data->len))
   {
      strlcpy(e->info.badge, path, sizeof(e->info.badge));
      cheevos_st.changed = true;
   }
}

bool grid_cheevos_poll(void)
{
   size_t i;
   bool changed;

   if (     cheevos_st.state == GRID_CHEEVOS_READY && !cheevos_st.badge_busy
         && !string_is_empty(cheevos_st.dir))
   {
      for (i = 0; i < cheevos_st.count; i++)
      {
         cheevos_entry_t *e = &cheevos_st.entries[i];
         if (     string_is_empty(e->info.badge) && !e->badge_tried
               && !string_is_empty(e->badge_url))
         {
            e->badge_tried        = true;
            cheevos_st.badge_busy = true;
            if (!task_push_http_transfer(e->badge_url, true, "GET",
                     cheevos_badge_cb, (void*)(uintptr_t)i))
               cheevos_st.badge_busy = false;
            break;
         }
      }
   }

   changed            = cheevos_st.changed;
   cheevos_st.changed = false;
   return changed;
}

enum grid_cheevos_state grid_cheevos_state(void) { return cheevos_st.state; }
size_t grid_cheevos_count(void) { return cheevos_st.count; }

const grid_cheevo_t *grid_cheevos_get(size_t idx)
{
   return idx < cheevos_st.count ? &cheevos_st.entries[idx].info : NULL;
}

static progress_entry_t *progress_find(unsigned game_id)
{
   size_t i;
   for (i = 0; i < progress_st.count; i++)
      if (progress_st.entries[i].game_id == game_id)
         return &progress_st.entries[i];
   return NULL;
}

/* Cache file, for the account in settings: a "user" line, then
 * game_id \t unlocked \t total per game. */
static bool progress_path(char *s, size_t len)
{
   char ra_dir[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();

   if (string_is_empty(settings->paths.directory_thumbnails))
      return false;
   fill_pathname_join_special(ra_dir, settings->paths.directory_thumbnails,
         "RetroAchievements Grid", sizeof(ra_dir));
   path_mkdir(ra_dir);
   fill_pathname_join_special(s, ra_dir, "progress.txt", len);
   return true;
}

static void progress_save(void)
{
   size_t i;
   const char *user, *token;
   char path[PATH_MAX_LENGTH];
   RFILE *f;

   if (     !cheevos_login(&user, &token)
         || !progress_path(path, sizeof(path))
         || !(f = filestream_open(path, RETRO_VFS_FILE_ACCESS_WRITE,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      return;

   filestream_printf(f, "user\t%s\n", user);
   /* Failed lookups (total 0) aren't kept: they retry next launch. */
   for (i = 0; i < progress_st.count; i++)
      if (progress_st.entries[i].total)
         filestream_printf(f, "%u\t%u\t%u\n", progress_st.entries[i].game_id,
               progress_st.entries[i].unlocked, progress_st.entries[i].total);
   filestream_close(f);
}

static void progress_load(void)
{
   const char *user, *token;
   char path[PATH_MAX_LENGTH];
   char line[256];
   RFILE *f;

   progress_st.loaded = true;

   if (     !cheevos_login(&user, &token)
         || !progress_path(path, sizeof(path))
         || !(f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      return;

   if (!filestream_gets(f, line, sizeof(line)))
      line[0] = '\0';
   line[strcspn(line, "\r\n")] = '\0';

   /* Another account's progress doesn't apply. */
   if (!strncmp(line, "user\t", 5) && string_is_equal(line + 5, user))
   {
      while (     progress_st.count < PROGRESS_MAX
               && filestream_gets(f, line, sizeof(line)))
      {
         unsigned id, unlocked, total;
         if (     sscanf(line, "%u\t%u\t%u", &id, &unlocked, &total) == 3
               && id && total)
         {
            progress_entry_t *e = &progress_st.entries[progress_st.count++];
            e->game_id  = id;
            e->unlocked = unlocked;
            e->total    = total;
            e->stale    = false;
         }
      }
   }

   filestream_close(f);
}

/* Remembers a game's progress (total 0: lookup failed), and saves it. */
static void progress_store(unsigned game_id, unsigned unlocked,
      unsigned total)
{
   progress_entry_t *e = progress_find(game_id);

   if (!e)
   {
      if (progress_st.count < PROGRESS_MAX)
         e = &progress_st.entries[progress_st.count++];
      else
      {
         e = &progress_st.entries[progress_st.next];
         progress_st.next = (progress_st.next + 1) % PROGRESS_MAX;
      }
   }
   e->game_id  = game_id;
   e->unlocked = unlocked;
   e->total    = total;
   e->stale    = false;
   if (total)
      progress_save();
}

/* Lookup over: what was found. */
static void progress_finish(unsigned game_id, unsigned unlocked,
      unsigned total)
{
   progress_st.busy_id = 0;
   progress_store(game_id, unlocked, total);
}

/* Lookup failed: keep what is cached (not retried until played again),
 * or remember the failure for this session. */
static void progress_fail(unsigned game_id)
{
   progress_entry_t *e = progress_find(game_id);

   progress_st.busy_id = 0;
   if (e)
      e->stale = false;
   else
      progress_store(game_id, 0, 0);
}

static void progress_unlocks_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   unsigned i, k, unlocked = 0;
   bool ok = false;
   rc_api_server_response_t server_response;
   rc_api_fetch_user_unlocks_response_t response;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;

   if ((unsigned)(uintptr_t)user_data != progress_st.generation)
      return;

   if (data && data->data && data->status == 200)
   {
      cheevos_server_response(&server_response, data);
      if (     rc_api_process_fetch_user_unlocks_server_response(&response,
                  &server_response) == RC_OK
            && response.response.succeeded)
      {
         ok = true;
         for (k = 0; k < response.num_achievement_ids; k++)
            for (i = 0; i < progress_st.id_count; i++)
               if (progress_st.ids[i] == response.achievement_ids[k])
               {
                  unlocked++;
                  break;
               }
      }
      rc_api_destroy_fetch_user_unlocks_response(&response);
   }

   if (ok)
      progress_finish(progress_st.busy_id, unlocked, progress_st.id_count);
   else
      progress_fail(progress_st.busy_id);
}

static void progress_game_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   unsigned i;
   char ua[256];
   const char *user, *token;
   rc_api_request_t request;
   rc_api_server_response_t server_response;
   rc_api_fetch_game_data_response_t response;
   rc_api_fetch_user_unlocks_request_t params;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   unsigned gen               = (unsigned)(uintptr_t)user_data;
   unsigned game_id           = progress_st.busy_id;

   if (gen != progress_st.generation)
      return;

   if (!data || !data->data || data->status != 200)
   {
      progress_fail(game_id);
      return;
   }

   cheevos_server_response(&server_response, data);
   if (     rc_api_process_fetch_game_data_server_response(&response,
               &server_response) != RC_OK
         || !response.response.succeeded)
   {
      rc_api_destroy_fetch_game_data_response(&response);
      progress_fail(game_id);
      return;
   }

   /* Core achievements only, as on the achievements screen. */
   progress_st.id_count = 0;
   for (i = 0; i < response.num_achievements
         && progress_st.id_count < CHEEVOS_MAX; i++)
      if (response.achievements[i].category == RC_ACHIEVEMENT_CATEGORY_CORE)
         progress_st.ids[progress_st.id_count++] = response.achievements[i].id;
   rc_api_destroy_fetch_game_data_response(&response);

   if (!progress_st.id_count)
   {
      progress_finish(game_id, 0, 0); /* no achievements */
      return;
   }
   if (!cheevos_login(&user, &token))
   {
      progress_fail(game_id);
      return;
   }

   memset(&params, 0, sizeof(params));
   params.username  = user;
   params.api_token = token;
   params.game_id   = game_id;
   params.hardcore  = 0; /* softcore unlocks include hardcore ones */

   if (rc_api_init_fetch_user_unlocks_request(&request, &params) != RC_OK)
   {
      progress_fail(game_id);
      return;
   }

   rcheevos_get_user_agent(get_rcheevos_locals(), ua, sizeof(ua));
   if (!task_push_http_post_transfer_with_user_agent(request.url,
            request.post_data, true, "POST", ua, progress_unlocks_cb,
            (void*)(uintptr_t)gen))
      progress_fail(game_id);
   rc_api_destroy_request(&request);
}

bool grid_cheevos_progress(unsigned game_id, bool fetch,
      unsigned *unlocked, unsigned *total)
{
   char ua[256];
   const char *user, *token;
   rc_api_request_t request;
   rc_api_fetch_game_data_request_t params;
   progress_entry_t *e;

   if (!game_id)
      return false;
   if (!progress_st.loaded)
      progress_load();

   /* Cached: shown as is, and only looked up again once played since. */
   if ((e = progress_find(game_id)))
   {
      *unlocked = e->unlocked;
      *total    = e->total;
      if (!e->stale)
         return true;
   }

   if (     !fetch || progress_st.busy_id || !cheevos_login(&user, &token))
      return e != NULL;
   if (     !progress_st.ids
         && !(progress_st.ids = (unsigned*)malloc(
               CHEEVOS_MAX * sizeof(unsigned))))
      return e != NULL;

   memset(&params, 0, sizeof(params));
   params.username  = user;
   params.api_token = token;
   params.game_id   = game_id;
   if (rc_api_init_fetch_game_data_request(&request, &params) != RC_OK)
      return e != NULL;

   progress_st.busy_id = game_id;
   rcheevos_get_user_agent(get_rcheevos_locals(), ua, sizeof(ua));
   if (!task_push_http_post_transfer_with_user_agent(request.url,
            request.post_data, true, "POST", ua, progress_game_cb,
            (void*)(uintptr_t)progress_st.generation))
      progress_st.busy_id = 0;
   rc_api_destroy_request(&request);
   return e != NULL;
}

void grid_cheevos_progress_reset(unsigned game_id)
{
   progress_entry_t *e = progress_find(game_id);

   if (e)
      e->stale = true;
   /* A lookup started before playing may miss the new unlocks. */
   if (game_id && progress_st.busy_id == game_id)
   {
      progress_st.busy_id = 0;
      progress_st.generation++;
   }
}

#else

bool grid_cheevos_progress(unsigned game_id, bool fetch,
      unsigned *unlocked, unsigned *total) { return false; }
void grid_cheevos_progress_reset(unsigned game_id) { }
void grid_cheevos_open(unsigned game_id) { }
enum grid_cheevos_state grid_cheevos_state(void) { return GRID_CHEEVOS_LOGGED_OUT; }
size_t grid_cheevos_count(void) { return 0; }
const grid_cheevo_t *grid_cheevos_get(size_t idx) { return NULL; }
bool grid_cheevos_poll(void) { return false; }

#endif
