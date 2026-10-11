/* RetroArchWrapper: RetroAchievements metadata for the game grid.
 *
 * Identifies ROMs the way RetroAchievements does (rcheevos hash), then asks
 * the server for the official title and game icon using the user's
 * RetroAchievements login from settings. Results are cached in
 *    <thumbnails>/RetroAchievements Grid/index.txt
 * next to the downloaded icons, keyed by file name + size, so each game is
 * looked up once. One lookup runs at a time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "grid_meta.h"
#include "../configuration.h"

#if defined(HAVE_CHEEVOS) && defined(HAVE_NETWORKING)

#include <rc_api_runtime.h>
#include <rc_hash.h>

#include "../tasks/tasks_internal.h"
#include "../cheevos/cheevos_locals.h"
#include "../cheevos/cheevos_client.h"

#define GRID_META_MAX       1024
#define GRID_META_MAX_BYTES (64 * 1024 * 1024) /* skip hashing bigger files */
#define GRID_META_DIR       "RetroAchievements Grid"

typedef struct
{
   char key[NAME_MAX_LENGTH];      /* ROM file name */
   char path[PATH_MAX_LENGTH];     /* full ROM path (not cached) */
   char title[NAME_MAX_LENGTH];
   char icon[NAME_MAX_LENGTH];     /* file name inside the cache dir */
   char icon_url[PATH_MAX_LENGTH]; /* pending icon download (not cached) */
   int64_t size;
   unsigned game_id;
   enum grid_meta_state state;
   bool in_flight;
   bool transient; /* failed for a temporary reason; retry next launch */
} grid_meta_entry_t;

static struct
{
   grid_meta_entry_t *entries;
   size_t count;
   bool loaded;
   bool busy;     /* a request is in flight */
   bool changed;
   char dir[PATH_MAX_LENGTH];
} meta_st;

static bool grid_meta_dir(char *s, size_t len)
{
   settings_t *settings = config_get_ptr();

   if (string_is_empty(settings->paths.directory_thumbnails))
      return false;

   fill_pathname_join_special(s, settings->paths.directory_thumbnails,
         GRID_META_DIR, len);
   return true;
}

static void grid_meta_save(void)
{
   size_t i;
   char path[PATH_MAX_LENGTH];
   RFILE *f;

   if (string_is_empty(meta_st.dir))
      return;

   path_mkdir(meta_st.dir);
   fill_pathname_join_special(path, meta_st.dir, "index.txt", sizeof(path));

   if (!(f = filestream_open(path, RETRO_VFS_FILE_ACCESS_WRITE,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      return;

   /* key \t size \t state \t game_id \t title \t icon */
   for (i = 0; i < meta_st.count; i++)
   {
      grid_meta_entry_t *e = &meta_st.entries[i];

      if (e->state == GRID_META_PENDING)
         continue;

      /* Size -1 never matches, so transient failures retry next launch. */
      filestream_printf(f, "%s\t%lld\t%d\t%u\t%s\t%s\n",
            e->key, e->transient ? -1LL : (long long)e->size,
            (int)e->state, e->game_id, e->title, e->icon);
   }

   filestream_close(f);
}

static void grid_meta_load(void)
{
   char path[PATH_MAX_LENGTH];
   char line[PATH_MAX_LENGTH * 2];
   RFILE *f;

   meta_st.loaded = true;

   if (!meta_st.entries)
      meta_st.entries = (grid_meta_entry_t*)calloc(GRID_META_MAX,
            sizeof(grid_meta_entry_t));

   if (!meta_st.entries || !grid_meta_dir(meta_st.dir, sizeof(meta_st.dir)))
      return;

   fill_pathname_join_special(path, meta_st.dir, "index.txt", sizeof(path));

   if (!(f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      return;

   while (meta_st.count < GRID_META_MAX
         && filestream_gets(f, line, sizeof(line)))
   {
      char *field[6];
      char *p   = line;
      int   n   = 0;
      grid_meta_entry_t *e;

      line[strcspn(line, "\r\n")] = '\0';

      while (n < 6)
      {
         field[n++] = p;
         if (!(p = strchr(p, '\t')))
            break;
         *p++ = '\0';
      }

      if (n < 6)
         continue;

      e = &meta_st.entries[meta_st.count++];
      memset(e, 0, sizeof(*e));
      strlcpy(e->key,   field[0], sizeof(e->key));
      e->size    = strtoll(field[1], NULL, 10);
      e->state   = (enum grid_meta_state)atoi(field[2]);
      e->game_id = (unsigned)strtoul(field[3], NULL, 10);
      strlcpy(e->title, field[4], sizeof(e->title));
      strlcpy(e->icon,  field[5], sizeof(e->icon));
   }

   filestream_close(f);
}

/* Tabs and newlines would break the index format. */
static void grid_meta_copy_clean(char *dst, const char *src, size_t len)
{
   char *p;
   strlcpy(dst, src ? src : "", len);
   for (p = dst; *p; p++)
      if (*p == '\t' || *p == '\n' || *p == '\r')
         *p = ' ';
}

static void grid_meta_finish(grid_meta_entry_t *e, enum grid_meta_state state)
{
   e->state     = state;
   e->in_flight = false;
   meta_st.busy    = false;
   meta_st.changed = true;
   grid_meta_save();
}

static void grid_meta_fail_transient(grid_meta_entry_t *e)
{
   e->transient = true;
   grid_meta_finish(e, GRID_META_UNKNOWN);
}

static void grid_meta_icon_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   char path[PATH_MAX_LENGTH];
   grid_meta_entry_t *e          = (grid_meta_entry_t*)user_data;
   http_transfer_data_t *data    = (http_transfer_data_t*)task_data;

   e->icon_url[0] = '\0';

   if (data && data->status == 200 && data->data && data->len)
   {
      path_mkdir(meta_st.dir);
      fill_pathname_join_special(path, meta_st.dir, e->icon, sizeof(path));
      if (!filestream_write_file(path, data->data, data->len))
         e->icon[0] = '\0';
   }
   else
      e->icon[0] = '\0';

   grid_meta_finish(e, GRID_META_FOUND);
}

static void grid_meta_game_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   rc_api_server_response_t server_response;
   rc_api_fetch_game_sets_response_t response;
   grid_meta_entry_t *e       = (grid_meta_entry_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   char ua[256];

   /* Network/server trouble or a bad login: retry next launch. */
   if (     !data || !data->data
         || data->status < 0 || data->status >= 500
         || data->status == 401 || data->status == 403)
   {
      grid_meta_fail_transient(e);
      return;
   }

   memset(&server_response, 0, sizeof(server_response));
   server_response.body             = data->data;
   server_response.body_length      = data->len;
   server_response.http_status_code = data->status;

   if (     rc_api_process_fetch_game_sets_server_response(&response,
               &server_response) != RC_OK
         || !response.response.succeeded
         || response.id == 0)
   {
      rc_api_destroy_fetch_game_sets_response(&response);
      grid_meta_finish(e, GRID_META_UNKNOWN);
      return;
   }

   e->game_id = response.id;
   grid_meta_copy_clean(e->title, response.title, sizeof(e->title));

   if (!string_is_empty(response.image_url))
   {
      snprintf(e->icon, sizeof(e->icon), "%u.png", e->game_id);
      strlcpy(e->icon_url, response.image_url, sizeof(e->icon_url));
   }

   rc_api_destroy_fetch_game_sets_response(&response);

   if (string_is_empty(e->icon_url))
   {
      grid_meta_finish(e, GRID_META_FOUND);
      return;
   }

   rcheevos_get_user_agent(get_rcheevos_locals(), ua, sizeof(ua));
   if (!task_push_http_transfer_with_user_agent(e->icon_url, true, "GET",
            ua, grid_meta_icon_cb, e))
   {
      e->icon[0] = '\0';
      grid_meta_finish(e, GRID_META_FOUND);
   }
}

static void grid_meta_start(grid_meta_entry_t *e)
{
   char hash[33];
   char ua[256];
   rc_hash_iterator_t iterator;
   rc_api_request_t request;
   rc_api_fetch_game_sets_request_t params;
   settings_t *settings = config_get_ptr();
   void *buf            = NULL;
   int64_t len          = 0;
   int ok;

   if (     string_is_empty(settings->arrays.cheevos_username)
         || string_is_empty(settings->arrays.cheevos_token))
   {
      /* Logging in later should still trigger lookups. */
      grid_meta_fail_transient(e);
      return;
   }

   if (     e->size > GRID_META_MAX_BYTES
         || !filestream_read_file(e->path, &buf, &len))
   {
      grid_meta_finish(e, GRID_META_UNKNOWN);
      return;
   }

   /* Hash from memory; the path is only used to pick the console. */
   rc_hash_initialize_iterator(&iterator, e->path, (const uint8_t*)buf, (size_t)len);
   ok = rc_hash_iterate(hash, &iterator);
   rc_hash_destroy_iterator(&iterator);
   free(buf);

   if (!ok)
   {
      grid_meta_finish(e, GRID_META_UNKNOWN);
      return;
   }

   memset(&params, 0, sizeof(params));
   params.username  = settings->arrays.cheevos_username;
   params.api_token = settings->arrays.cheevos_token;
   params.game_hash = hash;

   if (rc_api_init_fetch_game_sets_request(&request, &params) != RC_OK)
   {
      grid_meta_finish(e, GRID_META_UNKNOWN);
      return;
   }

   rcheevos_get_user_agent(get_rcheevos_locals(), ua, sizeof(ua));

   e->in_flight = true;
   meta_st.busy = true;

   if (!task_push_http_post_transfer_with_user_agent(request.url,
            request.post_data, true, "POST", ua, grid_meta_game_cb, e))
      grid_meta_fail_transient(e);

   rc_api_destroy_request(&request);
}

enum grid_meta_state grid_meta_get(const char *rom_path,
      char *title, size_t title_len,
      char *icon_path, size_t icon_path_len)
{
   size_t i;
   int64_t size;
   const char *key       = path_basename(rom_path);
   grid_meta_entry_t *e  = NULL;

   if (!meta_st.loaded)
      grid_meta_load();
   if (!meta_st.entries)
      return GRID_META_UNKNOWN;

   size = path_get_size(rom_path);

   for (i = 0; i < meta_st.count; i++)
   {
      if (string_is_equal(meta_st.entries[i].key, key))
      {
         e = &meta_st.entries[i];
         break;
      }
   }

   /* New file contents (or a cached transient failure): look up again. */
   if (e && e->size != size && !e->in_flight && !e->transient)
   {
      e->state    = GRID_META_PENDING;
      e->size     = size;
      e->game_id  = 0;
      e->title[0] = e->icon[0] = '\0';
   }

   if (!e)
   {
      if (meta_st.count >= GRID_META_MAX)
         return GRID_META_UNKNOWN;
      e = &meta_st.entries[meta_st.count++];
      memset(e, 0, sizeof(*e));
      strlcpy(e->key, key, sizeof(e->key));
      e->size  = size;
      e->state = GRID_META_PENDING;
   }

   strlcpy(e->path, rom_path, sizeof(e->path));

   if (e->state == GRID_META_FOUND)
   {
      strlcpy(title, e->title, title_len);
      icon_path[0] = '\0';
      if (!string_is_empty(e->icon) && !e->in_flight)
      {
         fill_pathname_join_special(icon_path, meta_st.dir, e->icon,
               icon_path_len);
         if (!path_is_valid(icon_path))
            icon_path[0] = '\0';
      }
   }

   return e->state;
}

bool grid_meta_poll(void)
{
   size_t i;
   bool changed;

   if (!meta_st.busy)
   {
      for (i = 0; i < meta_st.count; i++)
      {
         grid_meta_entry_t *e = &meta_st.entries[i];
         if (e->state == GRID_META_PENDING && !string_is_empty(e->path))
         {
            grid_meta_start(e);
            break;
         }
      }
   }

   changed         = meta_st.changed;
   meta_st.changed = false;
   return changed;
}

#else

enum grid_meta_state grid_meta_get(const char *rom_path,
      char *title, size_t title_len,
      char *icon_path, size_t icon_path_len)
{
   return GRID_META_UNKNOWN;
}

bool grid_meta_poll(void) { return false; }

#endif
