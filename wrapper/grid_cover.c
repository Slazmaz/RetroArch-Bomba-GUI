/* RetroArchWrapper: portrait covers from SteamGridDB for the game grid.
 *
 * Needs the user's SteamGridDB API key in
 *    <retroarch.exe dir>/wrapper/steamgriddb.key
 * Without it, covers are disabled and the grid keeps its square icons.
 *
 * For each game title: search SteamGridDB, take the top-voted 600x900
 * static PNG/JPEG grid, download it. Results are cached in
 *    <thumbnails>/SteamGridDB Covers/index.txt
 * next to the images, keyed by title, so each game is fetched once.
 * Network errors are retried next launch. One request runs at a time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <net/net_http.h>

#include "grid_cover.h"
#include "../configuration.h"

#ifdef HAVE_NETWORKING

#include "../tasks/tasks_internal.h"

#define COVER_MAX     1024
#define COVER_DIR     "SteamGridDB Covers"
#define COVER_API     "https://www.steamgriddb.com/api/v2"

enum cover_step
{
   COVER_STEP_SEARCH = 0,
   COVER_STEP_GRIDS,
   COVER_STEP_IMAGE
};

typedef struct
{
   char title[NAME_MAX_LENGTH];
   char file[NAME_MAX_LENGTH];     /* image file name in the cache dir */
   unsigned game_id;               /* SteamGridDB game id */
   enum grid_meta_state state;
   enum cover_step step;
   bool in_flight;
   bool transient;                 /* temporary failure: don't cache */
} cover_entry_t;

static struct
{
   cover_entry_t *entries;
   size_t count;
   bool loaded;
   bool busy;
   bool changed;
   bool have_key;
   char header[128];               /* "Authorization: Bearer <key>\r\n" */
   char dir[PATH_MAX_LENGTH];
} cover_st;

static void cover_load_key(void)
{
   char path[PATH_MAX_LENGTH];
   char app_dir[PATH_MAX_LENGTH];
   void *buf   = NULL;
   int64_t len = 0;
   char key[64];
   char *p;

   fill_pathname_application_dir(app_dir, sizeof(app_dir));
   fill_pathname_join_special(path, app_dir,
         "wrapper/steamgriddb.key", sizeof(path));

   if (!path_is_valid(path) || !filestream_read_file(path, &buf, &len))
      return;

   strlcpy(key, (const char*)buf, sizeof(key));
   free(buf);

   /* Trim whitespace/newlines. */
   for (p = key; *p && *p != '\r' && *p != '\n' && *p != ' '; p++);
   *p = '\0';

   if (string_is_empty(key))
      return;

   snprintf(cover_st.header, sizeof(cover_st.header),
         "Authorization: Bearer %s\r\n", key);
   cover_st.have_key = true;
}

static void cover_save(void)
{
   size_t i;
   char path[PATH_MAX_LENGTH];
   RFILE *f;

   if (string_is_empty(cover_st.dir))
      return;

   path_mkdir(cover_st.dir);
   fill_pathname_join_special(path, cover_st.dir, "index.txt", sizeof(path));

   if (!(f = filestream_open(path, RETRO_VFS_FILE_ACCESS_WRITE,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      return;

   /* title \t state \t game_id \t file */
   for (i = 0; i < cover_st.count; i++)
   {
      cover_entry_t *e = &cover_st.entries[i];
      if (e->state == GRID_META_PENDING || e->transient)
         continue;
      filestream_printf(f, "%s\t%d\t%u\t%s\n",
            e->title, (int)e->state, e->game_id, e->file);
   }

   filestream_close(f);
}

static void cover_load(void)
{
   char path[PATH_MAX_LENGTH];
   char line[PATH_MAX_LENGTH * 2];
   settings_t *settings = config_get_ptr();
   RFILE *f;

   cover_st.loaded = true;
   cover_load_key();

   if (!cover_st.entries)
      cover_st.entries = (cover_entry_t*)calloc(COVER_MAX, sizeof(cover_entry_t));

   if (     !cover_st.entries
         || string_is_empty(settings->paths.directory_thumbnails))
      return;

   fill_pathname_join_special(cover_st.dir,
         settings->paths.directory_thumbnails, COVER_DIR, sizeof(cover_st.dir));
   fill_pathname_join_special(path, cover_st.dir, "index.txt", sizeof(path));

   if (!(f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      return;

   while (cover_st.count < COVER_MAX && filestream_gets(f, line, sizeof(line)))
   {
      char *field[4];
      char *p = line;
      int   n = 0;
      cover_entry_t *e;

      line[strcspn(line, "\r\n")] = '\0';

      while (n < 4)
      {
         field[n++] = p;
         if (!(p = strchr(p, '\t')))
            break;
         *p++ = '\0';
      }

      if (n < 4)
         continue;

      e = &cover_st.entries[cover_st.count++];
      memset(e, 0, sizeof(*e));
      strlcpy(e->title, field[0], sizeof(e->title));
      e->state   = (enum grid_meta_state)atoi(field[1]);
      e->game_id = (unsigned)strtoul(field[2], NULL, 10);
      strlcpy(e->file, field[3], sizeof(e->file));
   }

   filestream_close(f);
}

static void cover_finish(cover_entry_t *e, enum grid_meta_state state,
      bool transient)
{
   e->state      = state;
   e->transient  = transient;
   e->in_flight  = false;
   cover_st.busy    = false;
   cover_st.changed = true;
   cover_save();
}

/* First number after "key": inside the "data" array, or 0. */
static unsigned cover_json_first_uint(const char *json, const char *key)
{
   const char *data = strstr(json, "\"data\"");
   const char *p    = data ? strstr(data, key) : NULL;
   return p ? (unsigned)strtoul(p + strlen(key), NULL, 10) : 0;
}

/* First string after "key":" inside the "data" array, with JSON's \/
 * unescaped. Returns false if missing. */
static bool cover_json_first_str(const char *json, const char *key,
      char *out, size_t out_len)
{
   const char *data = strstr(json, "\"data\"");
   const char *p    = data ? strstr(data, key) : NULL;
   size_t n         = 0;

   if (!p)
      return false;

   for (p += strlen(key); *p && *p != '"' && n + 1 < out_len; p++)
   {
      if (*p == '\\' && p[1])
         p++;
      out[n++] = *p;
   }
   out[n] = '\0';
   return n > 0;
}

static void cover_request(cover_entry_t *e, const char *url);

static void cover_http_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   char url[PATH_MAX_LENGTH];
   char *body                 = NULL;
   cover_entry_t *e           = (cover_entry_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;

   if (     !data || !data->data || data->status < 0
         || data->status >= 500 || data->status == 401
         || data->status == 403 || data->status == 429)
   {
      /* Network trouble, bad key or rate limit: retry next launch. */
      cover_finish(e, GRID_META_UNKNOWN, true);
      return;
   }

   if (e->step == COVER_STEP_IMAGE)
   {
      char path[PATH_MAX_LENGTH];

      if (data->status != 200)
      {
         cover_finish(e, GRID_META_UNKNOWN, true);
         return;
      }

      path_mkdir(cover_st.dir);
      fill_pathname_join_special(path, cover_st.dir, e->file, sizeof(path));
      if (!filestream_write_file(path, data->data, data->len))
      {
         cover_finish(e, GRID_META_UNKNOWN, true);
         return;
      }

      cover_finish(e, GRID_META_FOUND, false);
      return;
   }

   /* JSON responses: copy to a terminated string. */
   if (!(body = (char*)malloc(data->len + 1)))
   {
      cover_finish(e, GRID_META_UNKNOWN, true);
      return;
   }
   memcpy(body, data->data, data->len);
   body[data->len] = '\0';

   if (e->step == COVER_STEP_SEARCH)
   {
      e->game_id = cover_json_first_uint(body, "\"id\":");
      free(body);

      if (!e->game_id)
      {
         cover_finish(e, GRID_META_UNKNOWN, false); /* not on SteamGridDB */
         return;
      }

      snprintf(url, sizeof(url), COVER_API "/grids/game/%u"
            "?dimensions=600x900&types=static&mimes=image/png,image/jpeg"
            "&nsfw=false&humor=false", e->game_id);
      e->step = COVER_STEP_GRIDS;
      cover_request(e, url);
      return;
   }

   /* COVER_STEP_GRIDS */
   if (!cover_json_first_str(body, "\"url\":\"", url, sizeof(url)))
   {
      free(body);
      cover_finish(e, GRID_META_UNKNOWN, false); /* game has no covers */
      return;
   }
   free(body);

   snprintf(e->file, sizeof(e->file), "%u%s", e->game_id,
         string_ends_with(url, ".jpg") || string_ends_with(url, ".jpeg")
         ? ".jpg" : ".png");
   e->step = COVER_STEP_IMAGE;
   /* The image CDN needs no key. */
   if (!task_push_http_transfer(url, true, "GET", cover_http_cb, e))
      cover_finish(e, GRID_META_UNKNOWN, true);
}

static void cover_request(cover_entry_t *e, const char *url)
{
   if (!task_push_http_transfer_with_headers(url, true, "GET",
            cover_st.header, cover_http_cb, e))
      cover_finish(e, GRID_META_UNKNOWN, true);
}

static void cover_start(cover_entry_t *e)
{
   char url[PATH_MAX_LENGTH];
   char term[NAME_MAX_LENGTH * 3];

   net_http_urlencode_full(term, e->title, sizeof(term));
   snprintf(url, sizeof(url), COVER_API "/search/autocomplete/%s", term);

   e->in_flight  = true;
   e->step       = COVER_STEP_SEARCH;
   cover_st.busy = true;
   cover_request(e, url);
}

enum grid_meta_state grid_cover_get(const char *title,
      char *path, size_t path_len)
{
   size_t i;
   cover_entry_t *e = NULL;

   path[0] = '\0';

   if (!cover_st.loaded)
      cover_load();
   if (!cover_st.entries || string_is_empty(cover_st.dir)
         || string_is_empty(title))
      return GRID_META_UNKNOWN;

   for (i = 0; i < cover_st.count; i++)
      if (string_is_equal(cover_st.entries[i].title, title))
      {
         e = &cover_st.entries[i];
         break;
      }

   if (!e)
   {
      if (!cover_st.have_key || cover_st.count >= COVER_MAX)
         return GRID_META_UNKNOWN;
      e = &cover_st.entries[cover_st.count++];
      memset(e, 0, sizeof(*e));
      strlcpy(e->title, title, sizeof(e->title));
      e->state = GRID_META_PENDING;
   }

   if (e->state == GRID_META_FOUND)
   {
      fill_pathname_join_special(path, cover_st.dir, e->file, path_len);
      if (!path_is_valid(path))
      {
         /* Image deleted: fetch again. */
         path[0]  = '\0';
         e->state = cover_st.have_key ? GRID_META_PENDING : GRID_META_UNKNOWN;
      }
   }

   return e->state;
}

bool grid_cover_poll(void)
{
   size_t i;
   bool changed;

   if (!cover_st.busy && cover_st.have_key)
   {
      for (i = 0; i < cover_st.count; i++)
      {
         cover_entry_t *e = &cover_st.entries[i];
         if (e->state == GRID_META_PENDING && !e->in_flight)
         {
            cover_start(e);
            break;
         }
      }
   }

   changed          = cover_st.changed;
   cover_st.changed = false;
   return changed;
}

#else

enum grid_meta_state grid_cover_get(const char *title,
      char *path, size_t path_len)
{
   path[0] = '\0';
   return GRID_META_UNKNOWN;
}

bool grid_cover_poll(void) { return false; }

#endif
