/* Bomba: who the player follows on RetroAchievements.
 *
 * Uses the RetroAchievements login from settings (no extra key). The list
 * is fetched when asked for (at most once a minute); avatars are cached in
 *    <thumbnails>/RetroAchievements Grid/avatars/<name>.png
 * and downloaded one at a time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <features/features_cpu.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "grid_social.h"
#include "../configuration.h"

#define SOCIAL_MAX     64
#define SOCIAL_REFRESH 60000000 /* usec between list fetches */

#if defined(HAVE_CHEEVOS) && defined(HAVE_NETWORKING)

#include <rc_api_user.h>

#include "../tasks/tasks_internal.h"
#include "../cheevos/cheevos_locals.h"
#include "../cheevos/cheevos_client.h"

typedef struct
{
   grid_friend_t info;
   char avatar_url[1024];
   bool avatar_tried;          /* download attempted this session */
} social_entry_t;

static struct
{
   social_entry_t entries[SOCIAL_MAX];
   size_t count;
   enum grid_social_state state;
   bool fetching;
   bool avatar_busy;
   bool changed;
   retro_time_t last_fetch;
   char dir[PATH_MAX_LENGTH];
} social_st;

static bool social_avatar_dir(char *s, size_t len)
{
   char ra_dir[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();

   if (string_is_empty(settings->paths.directory_thumbnails))
      return false;
   fill_pathname_join_special(ra_dir, settings->paths.directory_thumbnails,
         "RetroAchievements Grid", sizeof(ra_dir));
   fill_pathname_join_special(s, ra_dir, "avatars", len);
   return true;
}

/* Cached avatar file for a user name (whether or not it exists yet). */
static void social_avatar_path(const char *name, char *s, size_t len)
{
   char file[128];
   snprintf(file, sizeof(file), "%s.png", name);
   fill_pathname_join_special(s, social_st.dir, file, len);
}

static void social_list_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   size_t i;
   rc_api_server_response_t server_response;
   rc_api_fetch_followed_users_response_t response;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;

   social_st.fetching = false;
   social_st.changed  = true;

   if (!data || !data->data || data->status != 200)
   {
      social_st.state = GRID_SOCIAL_ERROR;
      return;
   }

   memset(&server_response, 0, sizeof(server_response));
   server_response.body             = data->data;
   server_response.body_length      = data->len;
   server_response.http_status_code = data->status;

   if (     rc_api_process_fetch_followed_users_server_response(&response,
               &server_response) != RC_OK
         || !response.response.succeeded)
   {
      rc_api_destroy_fetch_followed_users_response(&response);
      social_st.state = GRID_SOCIAL_ERROR;
      return;
   }

   social_st.count = 0;
   for (i = 0; i < response.num_users && social_st.count < SOCIAL_MAX; i++)
   {
      const rc_api_followed_user_t *u = &response.users[i];
      social_entry_t *e               = &social_st.entries[social_st.count];

      if (string_is_empty(u->display_name))
         continue;

      memset(e, 0, sizeof(*e));
      strlcpy(e->info.name, u->display_name, sizeof(e->info.name));
      if (u->recent_activity.context)
         strlcpy(e->info.game, u->recent_activity.context,
               sizeof(e->info.game));
      if (u->recent_activity.description)
         strlcpy(e->info.activity, u->recent_activity.description,
               sizeof(e->info.activity));
      e->info.when = u->recent_activity.when;
      if (u->avatar_url)
         strlcpy(e->avatar_url, u->avatar_url, sizeof(e->avatar_url));

      social_avatar_path(e->info.name, e->info.avatar, sizeof(e->info.avatar));
      if (!path_is_valid(e->info.avatar))
         e->info.avatar[0] = '\0';

      social_st.count++;
   }

   rc_api_destroy_fetch_followed_users_response(&response);
   social_st.state = GRID_SOCIAL_READY;
}

void grid_social_refresh(void)
{
   char ua[256];
   rc_api_request_t request;
   rc_api_fetch_followed_users_request_t params;
   settings_t *settings = config_get_ptr();
   retro_time_t now     = cpu_features_get_time_usec();

   if (     string_is_empty(settings->arrays.cheevos_username)
         || string_is_empty(settings->arrays.cheevos_token))
   {
      social_st.state = GRID_SOCIAL_LOGGED_OUT;
      social_st.count = 0;
      return;
   }

   /* An avatar download holds a pointer into the list: don't replace it
    * under it. */
   if (social_st.fetching || social_st.avatar_busy)
      return;
   if (     social_st.state == GRID_SOCIAL_READY
         && now - social_st.last_fetch < SOCIAL_REFRESH)
      return;

   if (string_is_empty(social_st.dir))
      social_avatar_dir(social_st.dir, sizeof(social_st.dir));

   memset(&params, 0, sizeof(params));
   params.username  = settings->arrays.cheevos_username;
   params.api_token = settings->arrays.cheevos_token;

   if (rc_api_init_fetch_followed_users_request(&request, &params) != RC_OK)
   {
      social_st.state = GRID_SOCIAL_ERROR;
      return;
   }

   if (social_st.state == GRID_SOCIAL_LOGGED_OUT)
      social_st.state = GRID_SOCIAL_LOADING;

   rcheevos_get_user_agent(get_rcheevos_locals(), ua, sizeof(ua));
   social_st.fetching   = true;
   social_st.last_fetch = now;

   if (!task_push_http_post_transfer_with_user_agent(request.url,
            request.post_data, true, "POST", ua, social_list_cb, NULL))
   {
      social_st.fetching = false;
      social_st.state    = GRID_SOCIAL_ERROR;
   }

   rc_api_destroy_request(&request);
}

static void social_avatar_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   social_entry_t *e          = (social_entry_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   char path[PATH_MAX_LENGTH];

   social_st.avatar_busy = false;

   if (!data || data->status != 200 || !data->data || !data->len)
      return;

   path_mkdir(social_st.dir);
   social_avatar_path(e->info.name, path, sizeof(path));
   if (filestream_write_file(path, data->data, data->len))
   {
      strlcpy(e->info.avatar, path, sizeof(e->info.avatar));
      social_st.changed = true;
   }
}

bool grid_social_poll(void)
{
   size_t i;
   bool changed;

   if (!social_st.avatar_busy && !social_st.fetching
         && !string_is_empty(social_st.dir))
   {
      for (i = 0; i < social_st.count; i++)
      {
         social_entry_t *e = &social_st.entries[i];
         if (     string_is_empty(e->info.avatar) && !e->avatar_tried
               && !string_is_empty(e->avatar_url))
         {
            e->avatar_tried       = true;
            social_st.avatar_busy = true;
            if (!task_push_http_transfer(e->avatar_url, true, "GET",
                     social_avatar_cb, e))
               social_st.avatar_busy = false;
            break;
         }
      }
   }

   changed           = social_st.changed;
   social_st.changed = false;
   return changed;
}

enum grid_social_state grid_social_state(void) { return social_st.state; }
size_t grid_social_count(void)                 { return social_st.count; }

const grid_friend_t *grid_social_get(size_t idx)
{
   return idx < social_st.count ? &social_st.entries[idx].info : NULL;
}

#else

void grid_social_refresh(void) { }
enum grid_social_state grid_social_state(void) { return GRID_SOCIAL_LOGGED_OUT; }
size_t grid_social_count(void) { return 0; }
const grid_friend_t *grid_social_get(size_t idx) { return NULL; }
bool grid_social_poll(void) { return false; }

#endif
