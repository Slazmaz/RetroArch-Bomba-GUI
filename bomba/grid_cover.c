/* Bomba: portrait covers from SteamGridDB for the game grid.
 *
 * Needs the user's SteamGridDB API key (asked for on start, see
 * bomba_setup.h). Without it, covers are disabled and the grid keeps its
 * square icons.
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
#include <formats/image.h>
#include <formats/rpng.h>

#include "grid_cover.h"
#include "bomba_setup.h"
#include "../configuration.h"

void grid_title_fix_article(char *s)
{
   static const char *articles[] = { "The", "An", "A" };
   char *comma;

   for (comma = strstr(s, ", "); comma; comma = strstr(comma + 1, ", "))
   {
      unsigned i;
      for (i = 0; i < sizeof(articles) / sizeof(articles[0]); i++)
      {
         size_t n    = strlen(articles[i]);
         const char *after = comma + 2 + n;
         char out[NAME_MAX_LENGTH];

         /* The article ends the name: then the text, a subtitle or the
          * tags follow. */
         if (     strncmp(comma + 2, articles[i], n)
               || (*after && *after != ':' && strncmp(after, " -", 2)
                  && strncmp(after, " (", 2) && strncmp(after, " [", 2)))
            continue;

         snprintf(out, sizeof(out), "%s %.*s%s", articles[i],
               (int)(comma - s), s, after);
         strcpy(s, out); /* same length */
         return;
      }
   }
}

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
   const char *key = bomba_setup_steamgriddb_key();

   if (string_is_empty(key))
      return;

   snprintf(cover_st.header, sizeof(cover_st.header),
         "Authorization: Bearer %s\r\n", key);
   cover_st.have_key = true;
}

void grid_cover_reload_key(void)
{
   if (cover_st.loaded)
      cover_load_key();
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
      grid_title_fix_article(e->title); /* cached before titles had it */
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
   unsigned image_id;
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
   /* The image's own id goes in the file name, so the game menu can tell
    * which SteamGridDB image this is. */
   image_id = cover_json_first_uint(body, "\"id\":");
   free(body);

   snprintf(e->file, sizeof(e->file), "%u_%u%s", e->game_id, image_id,
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
   char *term = NULL;

   /* Encodes one value (net_http_urlencode_full expects a whole URL). */
   net_http_urlencode(&term, e->title);
   if (!term)
   {
      cover_finish(e, GRID_META_UNKNOWN, true);
      return;
   }
   snprintf(url, sizeof(url), COVER_API "/search/autocomplete/%s", term);
   free(term);

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

/* ---------------------------------------------------------------------- */
/* Heroes (wide banners, for the background)                              */
/* ---------------------------------------------------------------------- */

/* Same flow as covers, for every game, after its cover lookup (reusing
 * the SteamGridDB game id it found): top-voted 1920x620 (or 1600x650)
 * static hero -> download. The game asked for most recently (the
 * selected one) goes first. Cached in
 *    <thumbnails>/SteamGridDB Heroes/index.txt
 * keyed by title. */

#define HERO_MAX 1024
#define HERO_DIR "SteamGridDB Heroes"

enum hero_step
{
   HERO_STEP_SEARCH = 0,
   HERO_STEP_LIST,
   HERO_STEP_IMAGE
};

typedef struct
{
   char title[NAME_MAX_LENGTH];
   char file[NAME_MAX_LENGTH];
   unsigned game_id;
   unsigned asked;                 /* ask_seq when last asked for; 0: never */
   enum grid_meta_state state;
   enum hero_step step;
   bool in_flight;
   bool transient;
} hero_entry_t;

static struct
{
   hero_entry_t *entries;
   size_t count;
   unsigned ask_seq;
   bool loaded;
   bool busy;
   void (*on_download)(const char *path);
   char dir[PATH_MAX_LENGTH];
} hero_st;

static void hero_save(void)
{
   size_t i;
   char path[PATH_MAX_LENGTH];
   RFILE *f;

   if (string_is_empty(hero_st.dir))
      return;

   path_mkdir(hero_st.dir);
   fill_pathname_join_special(path, hero_st.dir, "index.txt", sizeof(path));

   if (!(f = filestream_open(path, RETRO_VFS_FILE_ACCESS_WRITE,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      return;

   /* title \t state \t game_id \t file */
   for (i = 0; i < hero_st.count; i++)
   {
      hero_entry_t *e = &hero_st.entries[i];
      if (e->state == GRID_META_PENDING || e->transient)
         continue;
      filestream_printf(f, "%s\t%d\t%u\t%s\n",
            e->title, (int)e->state, e->game_id, e->file);
   }

   filestream_close(f);
}

static void hero_load(void)
{
   char path[PATH_MAX_LENGTH];
   char line[PATH_MAX_LENGTH * 2];
   settings_t *settings = config_get_ptr();
   RFILE *f;

   hero_st.loaded = true;

   if (!cover_st.loaded)
      cover_load(); /* API key and known game ids */

   if (!hero_st.entries)
      hero_st.entries = (hero_entry_t*)calloc(HERO_MAX, sizeof(hero_entry_t));

   if (     !hero_st.entries
         || string_is_empty(settings->paths.directory_thumbnails))
      return;

   fill_pathname_join_special(hero_st.dir,
         settings->paths.directory_thumbnails, HERO_DIR, sizeof(hero_st.dir));
   fill_pathname_join_special(path, hero_st.dir, "index.txt", sizeof(path));

   if (!(f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      return;

   while (hero_st.count < HERO_MAX && filestream_gets(f, line, sizeof(line)))
   {
      char *field[4];
      char *p = line;
      int   n = 0;
      hero_entry_t *e;

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

      e = &hero_st.entries[hero_st.count++];
      memset(e, 0, sizeof(*e));
      strlcpy(e->title, field[0], sizeof(e->title));
      grid_title_fix_article(e->title); /* cached before titles had it */
      e->state   = (enum grid_meta_state)atoi(field[1]);
      e->game_id = (unsigned)strtoul(field[2], NULL, 10);
      strlcpy(e->file, field[3], sizeof(e->file));
   }

   filestream_close(f);
}

static void hero_finish(hero_entry_t *e, enum grid_meta_state state,
      bool transient)
{
   e->state     = state;
   e->transient = transient;
   e->in_flight = false;
   hero_st.busy = false;
   hero_save();
}

static void hero_request(hero_entry_t *e, const char *url);

static void hero_list(hero_entry_t *e)
{
   char url[PATH_MAX_LENGTH];
   snprintf(url, sizeof(url), COVER_API "/heroes/game/%u"
         "?dimensions=1920x620,1600x650&types=static"
         "&mimes=image/png,image/jpeg&nsfw=false&humor=false", e->game_id);
   e->step = HERO_STEP_LIST;
   hero_request(e, url);
}

static void hero_http_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   char url[PATH_MAX_LENGTH];
   unsigned image_id;
   char *body                 = NULL;
   hero_entry_t *e            = (hero_entry_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;

   if (     !data || !data->data || data->status < 0
         || data->status >= 500 || data->status == 401
         || data->status == 403 || data->status == 429)
   {
      /* Network trouble, bad key or rate limit: retry next launch. */
      hero_finish(e, GRID_META_UNKNOWN, true);
      return;
   }

   if (e->step == HERO_STEP_IMAGE)
   {
      char path[PATH_MAX_LENGTH];

      if (data->status != 200)
      {
         hero_finish(e, GRID_META_UNKNOWN, true);
         return;
      }

      path_mkdir(hero_st.dir);
      fill_pathname_join_special(path, hero_st.dir, e->file, sizeof(path));
      if (!filestream_write_file(path, data->data, data->len))
      {
         hero_finish(e, GRID_META_UNKNOWN, true);
         return;
      }

      hero_finish(e, GRID_META_FOUND, false);
      if (hero_st.on_download)
         hero_st.on_download(path);
      return;
   }

   /* JSON responses: copy to a terminated string. */
   if (!(body = (char*)malloc(data->len + 1)))
   {
      hero_finish(e, GRID_META_UNKNOWN, true);
      return;
   }
   memcpy(body, data->data, data->len);
   body[data->len] = '\0';

   if (e->step == HERO_STEP_SEARCH)
   {
      e->game_id = cover_json_first_uint(body, "\"id\":");
      free(body);

      if (!e->game_id)
         hero_finish(e, GRID_META_UNKNOWN, false); /* not on SteamGridDB */
      else
         hero_list(e);
      return;
   }

   /* HERO_STEP_LIST */
   if (!cover_json_first_str(body, "\"url\":\"", url, sizeof(url)))
   {
      free(body);
      hero_finish(e, GRID_META_UNKNOWN, false); /* game has no heroes */
      return;
   }
   /* The image's own id goes in the file name, so the game menu can tell
    * which SteamGridDB image this is. */
   image_id = cover_json_first_uint(body, "\"id\":");
   free(body);

   snprintf(e->file, sizeof(e->file), "%u_%u%s", e->game_id, image_id,
         string_ends_with(url, ".jpg") || string_ends_with(url, ".jpeg")
         ? ".jpg" : ".png");
   e->step = HERO_STEP_IMAGE;
   /* The image CDN needs no key. */
   if (!task_push_http_transfer(url, true, "GET", hero_http_cb, e))
      hero_finish(e, GRID_META_UNKNOWN, true);
}

static void hero_request(hero_entry_t *e, const char *url)
{
   if (!task_push_http_transfer_with_headers(url, true, "GET",
            cover_st.header, hero_http_cb, e))
      hero_finish(e, GRID_META_UNKNOWN, true);
}

/* The cover entry for a title, if its lookup has been queued. */
static cover_entry_t *hero_cover_of(const char *title)
{
   size_t i;
   for (i = 0; i < cover_st.count; i++)
      if (string_is_equal(cover_st.entries[i].title, title))
         return &cover_st.entries[i];
   return NULL;
}

static void hero_start(hero_entry_t *e)
{
   cover_entry_t *cover = hero_cover_of(e->title);

   e->in_flight = true;
   hero_st.busy = true;

   /* The cover lookup usually knows the game id already. */
   if (!e->game_id && cover)
      e->game_id = cover->game_id;

   if (e->game_id)
      hero_list(e);
   else
   {
      char url[PATH_MAX_LENGTH];
      char *term = NULL;

      net_http_urlencode(&term, e->title);
      if (!term)
      {
         hero_finish(e, GRID_META_UNKNOWN, true);
         return;
      }
      snprintf(url, sizeof(url), COVER_API "/search/autocomplete/%s", term);
      free(term);

      e->step = HERO_STEP_SEARCH;
      hero_request(e, url);
   }
}

/* Entry for a title, added (pending) if new. NULL without a key. */
static hero_entry_t *hero_entry(const char *title)
{
   size_t i;
   hero_entry_t *e;

   if (!hero_st.loaded)
      hero_load();
   if (!hero_st.entries || string_is_empty(hero_st.dir)
         || string_is_empty(title))
      return NULL;

   for (i = 0; i < hero_st.count; i++)
      if (string_is_equal(hero_st.entries[i].title, title))
         return &hero_st.entries[i];

   if (!cover_st.have_key || hero_st.count >= HERO_MAX)
      return NULL;

   e = &hero_st.entries[hero_st.count++];
   memset(e, 0, sizeof(*e));
   strlcpy(e->title, title, sizeof(e->title));
   e->state = GRID_META_PENDING;
   return e;
}

void grid_cover_hero_queue(const char *title)
{
   hero_entry(title);
}

enum grid_meta_state grid_cover_hero_get(const char *title,
      char *path, size_t path_len)
{
   hero_entry_t *e = hero_entry(title);

   path[0] = '\0';

   if (!e)
      return GRID_META_UNKNOWN;

   e->asked = ++hero_st.ask_seq;

   if (e->state == GRID_META_FOUND)
   {
      fill_pathname_join_special(path, hero_st.dir, e->file, path_len);
      if (!path_is_valid(path))
      {
         /* Image deleted: fetch again. */
         path[0]  = '\0';
         e->state = cover_st.have_key ? GRID_META_PENDING : GRID_META_UNKNOWN;
      }
   }

   return e->state;
}

void grid_cover_hero_set_download_cb(void (*cb)(const char *path))
{
   hero_st.on_download = cb;
}

bool grid_cover_hero_peek(const char *title, char *path, size_t path_len)
{
   size_t i;

   path[0] = '\0';
   if (!hero_st.loaded || string_is_empty(title))
      return false;

   for (i = 0; i < hero_st.count; i++)
   {
      hero_entry_t *e = &hero_st.entries[i];
      if (e->state == GRID_META_FOUND && string_is_equal(e->title, title))
      {
         fill_pathname_join_special(path, hero_st.dir, e->file, path_len);
         return true;
      }
   }
   return false;
}

void grid_cover_hero_poll(void)
{
   size_t i;
   hero_entry_t *next = NULL;

   if (hero_st.busy || !cover_st.have_key)
      return;

   /* The game asked for most recently first, else in list order. Each
    * waits for its cover lookup, which finds the game id; a game the
    * cover lookup didn't find isn't on SteamGridDB at all. */
   for (i = 0; i < hero_st.count; i++)
   {
      hero_entry_t *e = &hero_st.entries[i];
      cover_entry_t *cover;

      if (e->state != GRID_META_PENDING || e->in_flight)
         continue;

      cover = hero_cover_of(e->title);
      if (cover && cover->state == GRID_META_PENDING)
         continue;
      if (     cover && cover->state == GRID_META_UNKNOWN
            && !cover->game_id && !cover->transient)
      {
         hero_finish(e, GRID_META_UNKNOWN, false);
         continue;
      }

      if (!next || e->asked > next->asked)
         next = e;
   }

   if (next)
      hero_start(next);
}

/* ---------------------------------------------------------------------- */
/* Icons (profile pictures)                                               */
/* ---------------------------------------------------------------------- */

/* For every game, after its cover lookup: list SteamGridDB's static PNG
 * icons (best rated first) and, among the top ICON_CANDIDATES only, skip
 * any smaller than 256x256 or with any transparency, keeping the first
 * ICON_PER_GAME that pass, cropped square and scaled to 256x256. Saved
 * as <game id>_<n>.png and indexed in
 *    <thumbnails>/SteamGridDB Icons/index.txt
 * keyed by title. One game is worked on at a time. */

#define ICON_MAX        1024
#define ICON_DIR        "SteamGridDB Icons"
#define ICON_PER_GAME   3
#define ICON_SIZE       256
#define ICON_CANDIDATES 10 /* only the 10 best rated are considered */

typedef struct
{
   char title[NAME_MAX_LENGTH];
   unsigned game_id;
   unsigned found;                 /* icons saved: <id>_0.png .. */
   enum grid_meta_state state;
   bool in_flight;
   bool transient;
} icon_entry_t;

static struct
{
   icon_entry_t *entries;
   size_t count;
   bool loaded;
   bool busy;
   char dir[PATH_MAX_LENGTH];
   /* The game being worked on: its usable icons, best first. */
   char cand[ICON_CANDIDATES][PATH_MAX_LENGTH];
   unsigned cand_count;
   unsigned cand_next;
} icon_st;

static void icon_save(void)
{
   size_t i;
   char path[PATH_MAX_LENGTH];
   RFILE *f;

   if (string_is_empty(icon_st.dir))
      return;

   path_mkdir(icon_st.dir);
   fill_pathname_join_special(path, icon_st.dir, "index.txt", sizeof(path));

   if (!(f = filestream_open(path, RETRO_VFS_FILE_ACCESS_WRITE,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      return;

   /* title \t state \t game_id \t found */
   for (i = 0; i < icon_st.count; i++)
   {
      icon_entry_t *e = &icon_st.entries[i];
      if (e->state == GRID_META_PENDING || e->transient)
         continue;
      filestream_printf(f, "%s\t%d\t%u\t%u\n",
            e->title, (int)e->state, e->game_id, e->found);
   }

   filestream_close(f);
}

static void icon_load(void)
{
   char path[PATH_MAX_LENGTH];
   char line[PATH_MAX_LENGTH * 2];
   settings_t *settings = config_get_ptr();
   RFILE *f;

   icon_st.loaded = true;

   if (!cover_st.loaded)
      cover_load(); /* API key and known game ids */

   if (!icon_st.entries)
      icon_st.entries = (icon_entry_t*)calloc(ICON_MAX, sizeof(icon_entry_t));

   if (     !icon_st.entries
         || string_is_empty(settings->paths.directory_thumbnails))
      return;

   fill_pathname_join_special(icon_st.dir,
         settings->paths.directory_thumbnails, ICON_DIR, sizeof(icon_st.dir));
   fill_pathname_join_special(path, icon_st.dir, "index.txt", sizeof(path));

   if (!(f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      return;

   while (icon_st.count < ICON_MAX && filestream_gets(f, line, sizeof(line)))
   {
      char *field[4];
      char *p = line;
      int   n = 0;
      icon_entry_t *e;

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

      e = &icon_st.entries[icon_st.count++];
      memset(e, 0, sizeof(*e));
      strlcpy(e->title, field[0], sizeof(e->title));
      grid_title_fix_article(e->title); /* cached before titles had it */
      e->state   = (enum grid_meta_state)atoi(field[1]);
      e->game_id = (unsigned)strtoul(field[2], NULL, 10);
      e->found   = (unsigned)strtoul(field[3], NULL, 10);
      if (e->found > ICON_PER_GAME)
         e->found = ICON_PER_GAME;
   }

   filestream_close(f);
}

static void icon_finish(icon_entry_t *e, bool transient)
{
   e->state     = e->found ? GRID_META_FOUND : GRID_META_UNKNOWN;
   e->transient = transient && !e->found;
   e->in_flight = false;
   icon_st.busy = false;
   icon_save();
}

static void icon_file(const icon_entry_t *e, unsigned k, char *s, size_t len)
{
   char name[64];
   snprintf(name, sizeof(name), "%u_%u.png", e->game_id, k);
   fill_pathname_join_special(s, icon_st.dir, name, len);
}

/* Value after "key": within [p, end), as an unsigned (0 if missing). */
static unsigned icon_json_uint(const char *p, const char *end,
      const char *key)
{
   const char *q = strstr(p, key);
   return (q && q < end) ? (unsigned)strtoul(q + strlen(key), NULL, 10) : 0;
}

/* Collects usable icon URLs from a /icons/game response: each object
 * starts with {"id":, in the order SteamGridDB rates them. Only the first
 * ICON_CANDIDATES entries are looked at; of those, only PNGs at least
 * ICON_SIZE in both directions count. */
static void icon_parse_list(const char *json)
{
   const char *p   = strstr(json, "\"data\"");
   unsigned listed = 0;

   icon_st.cand_count = 0;
   icon_st.cand_next  = 0;

   while (p && (p = strstr(p, "{\"id\":")) && listed++ < ICON_CANDIDATES)
   {
      const char *end = strstr(p + 1, "{\"id\":");
      const char *u;
      char url[PATH_MAX_LENGTH];
      size_t n = 0;

      if (!end)
         end = p + strlen(p);

      if (     icon_json_uint(p, end, "\"width\":")  >= ICON_SIZE
            && icon_json_uint(p, end, "\"height\":") >= ICON_SIZE
            && (u = strstr(p, "\"url\":\"")) && u < end)
      {
         for (u += STRLEN_CONST("\"url\":\""); *u && *u != '"'
               && n + 1 < sizeof(url); u++)
         {
            if (*u == '\\' && u[1])
               u++;
            url[n++] = *u;
         }
         url[n] = '\0';

         if (string_ends_with(url, ".png"))
            strlcpy(icon_st.cand[icon_st.cand_count++], url,
                  sizeof(icon_st.cand[0]));
      }

      p = end;
   }
}

/* Whether any pixel of an ARGB image is less than fully opaque. */
static bool icon_has_transparency(const uint32_t *px, size_t n)
{
   size_t i;
   for (i = 0; i < n; i++)
      if ((px[i] >> 24) != 0xff)
         return true;
   return false;
}

/* Scales an opaque ARGB image to ICON_SIZE x ICON_SIZE: the centre square
 * is kept (no transparent padding), then shrunk by area averaging.
 * Returns a new buffer (free() it), or NULL. */
static uint32_t *icon_scale(const uint32_t *src, unsigned sw, unsigned sh)
{
   const unsigned size = ICON_SIZE;
   unsigned side       = sw < sh ? sw : sh;
   unsigned cx         = (sw - side) / 2;
   unsigned cy         = (sh - side) / 2;
   unsigned x, y;
   uint32_t *out       = (uint32_t*)malloc(size * size * sizeof(uint32_t));

   if (!out || !side)
   {
      free(out);
      return NULL;
   }

   for (y = 0; y < size; y++)
   {
      unsigned y0 = y * side / size, y1 = (y + 1) * side / size;
      if (y1 <= y0) y1 = y0 + 1;

      for (x = 0; x < size; x++)
      {
         unsigned x0 = x * side / size, x1 = (x + 1) * side / size;
         unsigned sx, sy, n = 0;
         uint32_t r = 0, g = 0, b = 0;

         if (x1 <= x0) x1 = x0 + 1;

         for (sy = y0; sy < y1; sy++)
            for (sx = x0; sx < x1; sx++)
            {
               uint32_t p = src[(sy + cy) * sw + sx + cx];
               r += (p >> 16) & 0xff;
               g += (p >>  8) & 0xff;
               b +=  p        & 0xff;
               n++;
            }

         out[y * size + x] = 0xff000000 | ((r / n) << 16) | ((g / n) << 8)
            | (b / n);
      }
   }

   return out;
}

static void icon_next(icon_entry_t *e);

static void icon_image_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   struct texture_image ti;
   icon_entry_t *e            = (icon_entry_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;

   if (!data || data->status < 0 || data->status >= 500 || data->status == 429)
   {
      /* Network trouble: keep what we have, retry the rest next launch. */
      icon_finish(e, true);
      return;
   }

   ti.width         = 0;
   ti.height        = 0;
   ti.pixels        = NULL;
   ti.supports_rgba = false; /* ARGB */

   /* The listed size can be wrong: check the real one. Too small, with
    * any transparency, or not decodable: try the next best icon instead. */
   if (     data->status == 200 && data->data && data->len
         && image_texture_load_buffer(&ti, IMAGE_TYPE_PNG,
               data->data, data->len))
   {
      if (     ti.width >= ICON_SIZE && ti.height >= ICON_SIZE
            && !icon_has_transparency(ti.pixels,
                  (size_t)ti.width * ti.height))
      {
         uint32_t *icon = icon_scale(ti.pixels, ti.width, ti.height);
         char path[PATH_MAX_LENGTH];

         if (icon)
         {
            path_mkdir(icon_st.dir);
            icon_file(e, e->found, path, sizeof(path));
            if (rpng_save_image_argb(path, icon, ICON_SIZE, ICON_SIZE,
                     ICON_SIZE * sizeof(uint32_t)))
               e->found++;
            free(icon);
         }
      }
      image_texture_free(&ti);
   }

   icon_next(e);
}

/* Downloads the next candidate, or finishes when there are enough icons
 * or no candidates left. */
static void icon_next(icon_entry_t *e)
{
   if (e->found >= ICON_PER_GAME || icon_st.cand_next >= icon_st.cand_count)
   {
      icon_finish(e, false);
      return;
   }

   /* The image CDN needs no key. */
   if (!task_push_http_transfer(icon_st.cand[icon_st.cand_next++], true,
            "GET", icon_image_cb, e))
      icon_finish(e, true);
}

static void icon_list_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   char *body                 = NULL;
   icon_entry_t *e            = (icon_entry_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;

   if (     !data || !data->data || data->status != 200
         || !(body = (char*)malloc(data->len + 1)))
   {
      /* Network trouble, bad key or rate limit: retry next launch. */
      icon_finish(e, true);
      return;
   }

   memcpy(body, data->data, data->len);
   body[data->len] = '\0';
   icon_parse_list(body);
   free(body);

   icon_next(e);
}

static void icon_start(icon_entry_t *e)
{
   char url[PATH_MAX_LENGTH];

   e->in_flight = true;
   e->found     = 0;
   icon_st.busy = true;

   snprintf(url, sizeof(url), COVER_API "/icons/game/%u"
         "?types=static&mimes=image/png&nsfw=false&humor=false", e->game_id);

   if (!task_push_http_transfer_with_headers(url, true, "GET",
            cover_st.header, icon_list_cb, e))
      icon_finish(e, true);
}

void grid_cover_icons_queue(const char *title)
{
   size_t i;
   icon_entry_t *e;

   if (!icon_st.loaded)
      icon_load();
   if (     !icon_st.entries || string_is_empty(icon_st.dir)
         || string_is_empty(title) || !cover_st.have_key
         || icon_st.count >= ICON_MAX)
      return;

   for (i = 0; i < icon_st.count; i++)
      if (string_is_equal(icon_st.entries[i].title, title))
         return;

   e = &icon_st.entries[icon_st.count++];
   memset(e, 0, sizeof(*e));
   strlcpy(e->title, title, sizeof(e->title));
   e->state = GRID_META_PENDING;
}

void grid_cover_icons_poll(void)
{
   size_t i;

   if (icon_st.busy || !cover_st.have_key)
      return;

   /* Each game waits for its cover lookup, which finds the game id. */
   for (i = 0; i < icon_st.count; i++)
   {
      icon_entry_t *e = &icon_st.entries[i];
      cover_entry_t *cover;

      if (e->state != GRID_META_PENDING || e->in_flight)
         continue;

      cover = hero_cover_of(e->title);
      if (cover && cover->state == GRID_META_PENDING)
         continue;
      if (!cover || !cover->game_id)
      {
         /* Not on SteamGridDB (or no cover lookup to say so). */
         if (cover && !cover->transient)
            icon_finish(e, false);
         continue;
      }

      e->game_id = cover->game_id;
      icon_start(e);
      break;
   }
}

size_t grid_cover_icon_count(void)
{
   size_t i, n = 0;

   if (!icon_st.loaded)
      icon_load();
   for (i = 0; i < icon_st.count; i++)
      if (icon_st.entries[i].state == GRID_META_FOUND)
         n += icon_st.entries[i].found;
   return n;
}

bool grid_cover_icon_get(size_t idx, char *path, size_t len)
{
   size_t i;

   path[0] = '\0';
   for (i = 0; i < icon_st.count; i++)
   {
      icon_entry_t *e = &icon_st.entries[i];
      if (e->state != GRID_META_FOUND)
         continue;
      if (idx < e->found)
      {
         icon_file(e, (unsigned)idx, path, len);
         return true;
      }
      idx -= e->found;
   }
   return false;
}

/* ---------------------------------------------------------------------- */
/* Choosing art (game menu: change cover art / change wallpaper)          */
/* ---------------------------------------------------------------------- */

/* Lists every static cover (600x900) or hero of a game on SteamGridDB,
 * best rated first, with their small preview images (cached in
 * <thumbnails>/SteamGridDB Choices/, one download at a time). Choosing
 * one downloads it and makes it the game's cover or hero. */

#define CHOICE_MAX 60
#define CHOICE_DIR "SteamGridDB Choices"

typedef struct
{
   char url[PATH_MAX_LENGTH];
   char thumb_url[PATH_MAX_LENGTH];
   char thumb[PATH_MAX_LENGTH];    /* cached preview; empty until there */
   unsigned id;
   unsigned w, h;                  /* image size */
   bool thumb_tried;
} choice_t;

static struct
{
   choice_t *items;
   size_t count;
   enum grid_choice_kind kind;
   char title[NAME_MAX_LENGTH];
   char dir[PATH_MAX_LENGTH];
   unsigned game_id;
   unsigned generation;            /* bumps per open, drops stale replies */
   enum grid_meta_state state;     /* PENDING loading, FOUND, UNKNOWN */
   enum grid_choice_apply apply;
   unsigned thumb_gen;             /* generation of the preview in flight */
   bool thumb_busy;
   bool changed;
} choice_st;

/* String value after key ("name":") within [p, end), with JSON's
 * escapes dropped; empty if missing. */
static void choice_json_str(const char *p, const char *end, const char *key,
      char *out, size_t len)
{
   const char *u = strstr(p, key);
   size_t n      = 0;

   if (u && u < end)
      for (u += strlen(key); *u && *u != '"' && n + 1 < len; u++)
      {
         if (*u == 0x5c && u[1]) /* backslash: skip the escape */
            u++;
         out[n++] = *u;
      }
   out[n] = 0;
}

static void choice_list(void);

static void choice_list_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   char *body                 = NULL;
   const char *p;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;

   if ((uintptr_t)user_data != choice_st.generation)
      return;

   choice_st.changed = true;

   if (     !data || !data->data || data->status != 200
         || !(body = (char*)malloc(data->len + 1)))
   {
      choice_st.state = GRID_META_UNKNOWN;
      return;
   }
   memcpy(body, data->data, data->len);
   body[data->len] = '\0';

   /* Search step: first result's id, then list. */
   if (!choice_st.game_id)
   {
      choice_st.game_id = cover_json_first_uint(body, "\"id\":");
      free(body);
      if (!choice_st.game_id)
         choice_st.state = GRID_META_UNKNOWN; /* not on SteamGridDB */
      else
         choice_list();
      return;
   }

   /* Each object starts with {"id":, best rated first. */
   choice_st.count = 0;
   p = strstr(body, "\"data\"");
   while (p && (p = strstr(p, "{\"id\":")) && choice_st.count < CHOICE_MAX)
   {
      const char *end = strstr(p + 1, "{\"id\":");
      choice_t *c     = &choice_st.items[choice_st.count];

      if (!end)
         end = p + strlen(p);

      memset(c, 0, sizeof(*c));
      c->id = (unsigned)strtoul(p + STRLEN_CONST("{\"id\":"), NULL, 10);

      choice_json_str(p, end, "\"url\":\"", c->url, sizeof(c->url));
      c->w = icon_json_uint(p, end, "\"width\":");
      c->h = icon_json_uint(p, end, "\"height\":");
      choice_json_str(p, end, "\"thumb\":\"", c->thumb_url,
            sizeof(c->thumb_url));

      if (c->id && !string_is_empty(c->url))
         choice_st.count++;
      p = end;
   }
   free(body);

   choice_st.state = choice_st.count ? GRID_META_FOUND : GRID_META_UNKNOWN;
}

static void choice_list(void)
{
   char url[PATH_MAX_LENGTH];

   if (choice_st.kind == GRID_CHOICE_COVER)
      snprintf(url, sizeof(url), COVER_API "/grids/game/%u"
            "?dimensions=600x900&types=static&mimes=image/png,image/jpeg"
            "&nsfw=false&humor=false", choice_st.game_id);
   else
      snprintf(url, sizeof(url), COVER_API "/heroes/game/%u"
            "?types=static&mimes=image/png,image/jpeg&nsfw=false&humor=false",
            choice_st.game_id);

   if (!task_push_http_transfer_with_headers(url, true, "GET",
            cover_st.header, choice_list_cb,
            (void*)(uintptr_t)choice_st.generation))
      choice_st.state = GRID_META_UNKNOWN;
}

void grid_cover_choices_open(const char *title, enum grid_choice_kind kind)
{
   size_t i;
   settings_t *settings = config_get_ptr();

   if (!cover_st.loaded)
      cover_load();

   choice_st.generation++;
   choice_st.count      = 0;
   choice_st.kind       = kind;
   choice_st.game_id    = 0;
   choice_st.apply      = GRID_CHOICE_APPLY_IDLE;
   choice_st.thumb_busy = false;
   choice_st.changed    = true;
   strlcpy(choice_st.title, title ? title : "", sizeof(choice_st.title));

   if (!choice_st.items)
      choice_st.items = (choice_t*)calloc(CHOICE_MAX, sizeof(choice_t));

   if (     !cover_st.have_key || !choice_st.items
         || string_is_empty(title)
         || string_is_empty(settings->paths.directory_thumbnails))
   {
      choice_st.state = GRID_META_UNKNOWN;
      return;
   }

   fill_pathname_join_special(choice_st.dir,
         settings->paths.directory_thumbnails, CHOICE_DIR,
         sizeof(choice_st.dir));

   /* The cover lookup usually knows the game id already. */
   for (i = 0; i < cover_st.count; i++)
      if (     cover_st.entries[i].game_id
            && string_is_equal(cover_st.entries[i].title, title))
      {
         choice_st.game_id = cover_st.entries[i].game_id;
         break;
      }

   choice_st.state = GRID_META_PENDING;

   if (choice_st.game_id)
      choice_list();
   else
   {
      char url[PATH_MAX_LENGTH];
      char *term = NULL;

      net_http_urlencode(&term, title);
      if (!term)
      {
         choice_st.state = GRID_META_UNKNOWN;
         return;
      }
      snprintf(url, sizeof(url), COVER_API "/search/autocomplete/%s", term);
      free(term);

      if (!task_push_http_transfer_with_headers(url, true, "GET",
               cover_st.header, choice_list_cb,
               (void*)(uintptr_t)choice_st.generation))
         choice_st.state = GRID_META_UNKNOWN;
   }
}

enum grid_meta_state grid_cover_choices_state(void) { return choice_st.state; }
size_t grid_cover_choices_count(void) { return choice_st.count; }

const char *grid_cover_choice_thumb(size_t idx)
{
   return idx < choice_st.count ? choice_st.items[idx].thumb : "";
}

static void choice_thumb_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   size_t idx                 = (size_t)(uintptr_t)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   choice_t *c;
   char name[64], path[PATH_MAX_LENGTH];

   if (choice_st.thumb_gen != choice_st.generation)
      return; /* another game's list was opened meanwhile */
   choice_st.thumb_busy = false;

   if (     idx >= choice_st.count || !data || data->status != 200
         || !data->data || !data->len)
      return;

   c = &choice_st.items[idx];
   snprintf(name, sizeof(name), "%u_t%s", c->id,
         string_ends_with(c->thumb_url, ".png") ? ".png" : ".jpg");
   path_mkdir(choice_st.dir);
   fill_pathname_join_special(path, choice_st.dir, name, sizeof(path));
   if (filestream_write_file(path, data->data, data->len))
   {
      strlcpy(c->thumb, path, sizeof(c->thumb));
      choice_st.changed = true;
   }
}

static void choice_apply_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   size_t idx                 = (size_t)(uintptr_t)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   choice_t *c;
   char file[64], path[PATH_MAX_LENGTH];
   size_t i;

   choice_st.changed = true;

   if (     idx >= choice_st.count || !data || data->status != 200
         || !data->data || !data->len)
   {
      choice_st.apply = GRID_CHOICE_APPLY_FAILED;
      return;
   }

   c = &choice_st.items[idx];
   snprintf(file, sizeof(file), "%u_%u%s", choice_st.game_id, c->id,
         string_ends_with(c->url, ".jpg") || string_ends_with(c->url, ".jpeg")
         ? ".jpg" : ".png");

   if (choice_st.kind == GRID_CHOICE_COVER)
   {
      cover_entry_t *e = NULL;

      path_mkdir(cover_st.dir);
      fill_pathname_join_special(path, cover_st.dir, file, sizeof(path));
      if (!filestream_write_file(path, data->data, data->len))
      {
         choice_st.apply = GRID_CHOICE_APPLY_FAILED;
         return;
      }

      for (i = 0; i < cover_st.count; i++)
         if (string_is_equal(cover_st.entries[i].title, choice_st.title))
            e = &cover_st.entries[i];
      if (!e && cover_st.count < COVER_MAX)
      {
         e = &cover_st.entries[cover_st.count++];
         memset(e, 0, sizeof(*e));
         strlcpy(e->title, choice_st.title, sizeof(e->title));
      }
      if (e)
      {
         e->game_id   = choice_st.game_id;
         e->state     = GRID_META_FOUND;
         e->transient = false;
         strlcpy(e->file, file, sizeof(e->file));
         cover_st.changed = true;
         cover_save();
      }
   }
   else
   {
      hero_entry_t *e;

      if (!hero_st.loaded)
         hero_load();
      path_mkdir(hero_st.dir);
      fill_pathname_join_special(path, hero_st.dir, file, sizeof(path));
      if (     !filestream_write_file(path, data->data, data->len)
            || !(e = hero_entry(choice_st.title)))
      {
         choice_st.apply = GRID_CHOICE_APPLY_FAILED;
         return;
      }
      e->game_id   = choice_st.game_id;
      e->state     = GRID_META_FOUND;
      e->transient = false;
      strlcpy(e->file, file, sizeof(e->file));
      hero_save();
   }

   choice_st.apply = GRID_CHOICE_APPLY_DONE;
}

void grid_cover_choose(size_t idx)
{
   if (idx >= choice_st.count || choice_st.apply == GRID_CHOICE_APPLY_WORKING)
      return;

   choice_st.apply   = GRID_CHOICE_APPLY_WORKING;
   choice_st.changed = true;
   /* The image CDN needs no key. */
   if (!task_push_http_transfer(choice_st.items[idx].url, true, "GET",
            choice_apply_cb, (void*)(uintptr_t)idx))
      choice_st.apply = GRID_CHOICE_APPLY_FAILED;
}

enum grid_choice_apply grid_cover_choose_state(void) { return choice_st.apply; }

int grid_cover_choice_current(void)
{
   size_t i;
   const char *file = NULL;
   const char *us;

   if (choice_st.state != GRID_META_FOUND || !choice_st.count)
      return -1;

   if (choice_st.kind == GRID_CHOICE_COVER)
   {
      for (i = 0; i < cover_st.count; i++)
         if (     cover_st.entries[i].state == GRID_META_FOUND
               && string_is_equal(cover_st.entries[i].title, choice_st.title))
            file = cover_st.entries[i].file;
   }
   else
   {
      for (i = 0; i < hero_st.count; i++)
         if (     hero_st.entries[i].state == GRID_META_FOUND
               && string_is_equal(hero_st.entries[i].title, choice_st.title))
            file = hero_st.entries[i].file;
   }

   if (string_is_empty(file))
      return -1;

   /* "<game id>_<image id>.png" names the image. Older automatic
    * downloads are "<game id>.png": the best rated of the sizes the
    * automatic download asks for (covers: all listed ones; heroes:
    * 1920x620 or 1600x650), i.e. the first such in the list. */
   if (!(us = strchr(file, '_')))
   {
      if (choice_st.kind == GRID_CHOICE_COVER)
         return 0;
      for (i = 0; i < choice_st.count; i++)
         if (     (choice_st.items[i].w == 1920 && choice_st.items[i].h == 620)
               || (choice_st.items[i].w == 1600 && choice_st.items[i].h == 650))
            return (int)i;
      return -1;
   }
   {
      unsigned id = (unsigned)strtoul(us + 1, NULL, 10);
      for (i = 0; i < choice_st.count; i++)
         if (choice_st.items[i].id == id)
            return (int)i;
   }
   return -1;
}

bool grid_cover_choices_poll(void)
{
   size_t i;
   bool changed;

   /* Previews in list order, one at a time. */
   if (     choice_st.state == GRID_META_FOUND && !choice_st.thumb_busy
         && choice_st.apply != GRID_CHOICE_APPLY_WORKING)
   {
      for (i = 0; i < choice_st.count; i++)
      {
         choice_t *c = &choice_st.items[i];
         if (     string_is_empty(c->thumb) && !c->thumb_tried
               && !string_is_empty(c->thumb_url))
         {
            char name[64], path[PATH_MAX_LENGTH];

            c->thumb_tried = true;

            /* Already cached from an earlier visit? */
            snprintf(name, sizeof(name), "%u_t%s", c->id,
                  string_ends_with(c->thumb_url, ".png") ? ".png" : ".jpg");
            fill_pathname_join_special(path, choice_st.dir, name,
                  sizeof(path));
            if (path_is_valid(path))
            {
               strlcpy(c->thumb, path, sizeof(c->thumb));
               choice_st.changed = true;
               continue;
            }

            choice_st.thumb_busy = true;
            choice_st.thumb_gen  = choice_st.generation;
            if (!task_push_http_transfer(c->thumb_url, true, "GET",
                     choice_thumb_cb, (void*)(uintptr_t)i))
               choice_st.thumb_busy = false;
            break;
         }
      }
   }

   changed           = choice_st.changed;
   choice_st.changed = false;
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

enum grid_meta_state grid_cover_hero_get(const char *title,
      char *path, size_t path_len)
{
   path[0] = '\0';
   return GRID_META_UNKNOWN;
}

void grid_cover_hero_poll(void) { }
void grid_cover_hero_queue(const char *title) { }
void grid_cover_hero_set_download_cb(void (*cb)(const char *path)) { }
bool grid_cover_hero_peek(const char *title, char *path, size_t path_len)
{
   path[0] = '\0';
   return false;
}
void grid_cover_icons_queue(const char *title) { }
void grid_cover_icons_poll(void) { }
size_t grid_cover_icon_count(void) { return 0; }
void grid_cover_choices_open(const char *title, enum grid_choice_kind kind) { }
enum grid_meta_state grid_cover_choices_state(void) { return GRID_META_UNKNOWN; }
size_t grid_cover_choices_count(void) { return 0; }
const char *grid_cover_choice_thumb(size_t idx) { return ""; }
void grid_cover_choose(size_t idx) { }
enum grid_choice_apply grid_cover_choose_state(void) { return GRID_CHOICE_APPLY_FAILED; }
int grid_cover_choice_current(void) { return -1; }
bool grid_cover_choices_poll(void) { return false; }
bool grid_cover_icon_get(size_t idx, char *path, size_t len)
{
   path[0] = '\0';
   return false;
}

#endif
