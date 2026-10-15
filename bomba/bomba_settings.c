/* Bomba: the install's settings file (see bomba_settings.h). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "bomba_settings.h"
#include "../command.h"
#include "../configuration.h"
#include "../runloop.h"
#include "../verbosity.h"

#if defined(HAVE_CHEEVOS) && defined(HAVE_NETWORKING)
#include <rc_api_user.h>
#include "../tasks/tasks_internal.h"
#include "../cheevos/cheevos_locals.h"
#include "../cheevos/cheevos_client.h"
#endif

#define SETTINGS_FILE "bomba-settings.ini"

static struct
{
   char griddb_key[128];
   char ra_user[128];
   char ra_password[256];
   bool done;
} bset;

/* In the install's top folder, one up from retroarch.exe's app\. */
static void settings_path(char *s, size_t len)
{
   char app_dir[PATH_MAX_LENGTH], top[PATH_MAX_LENGTH];
   fill_pathname_application_dir(app_dir, sizeof(app_dir));
   fill_pathname_parent_dir(top, app_dir, sizeof(top));
   fill_pathname_join_special(s, top, SETTINGS_FILE, len);
}

static void settings_message(const char *msg)
{
   runloop_msg_queue_push(msg, strlen(msg), 1, 300, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
}

/* "  value  " or "\"value\"" -> value, in place. */
static char *settings_trim(char *s)
{
   char *e;
   while (*s == ' ' || *s == '\t')
      s++;
   e = s + strlen(s);
   while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'
         || e[-1] == '\n'))
      *--e = '\0';
   if (e - s >= 2 && *s == '"' && e[-1] == '"')
   {
      e[-1] = '\0';
      s++;
   }
   return s;
}

static void settings_read(const char *path)
{
   char *buf = NULL, *line, *next;
   int64_t len = 0;

   if (!filestream_read_file(path, (void**)&buf, &len) || !buf)
      return;

   for (line = buf; line && *line; line = next)
   {
      char *eq, *key, *val;
      if ((next = strchr(line, '\n')))
         *next++ = '\0';
      line = settings_trim(line);
      if (*line == '#' || !(eq = strchr(line, '=')))
         continue;
      *eq = '\0';
      key = settings_trim(line);
      val = settings_trim(eq + 1);

      if (string_is_equal(key, "steamgriddb_api_key"))
         strlcpy(bset.griddb_key, val, sizeof(bset.griddb_key));
      else if (string_is_equal(key, "retroachievements_username"))
         strlcpy(bset.ra_user, val, sizeof(bset.ra_user));
      else if (string_is_equal(key, "retroachievements_password"))
         strlcpy(bset.ra_password, val, sizeof(bset.ra_password));
   }
   free(buf);
}

/* Empties the password line, keeping the rest of the file as written. */
static void settings_erase_password(void)
{
   char path[PATH_MAX_LENGTH];
   char *buf = NULL, *out, *o, *line, *next;
   int64_t len = 0;
   RFILE *f;

   settings_path(path, sizeof(path));
   if (!filestream_read_file(path, (void**)&buf, &len) || !buf)
      return;
   if (!(out = (char*)malloc((size_t)len + 64)))
   {
      free(buf);
      return;
   }

   for (o = out, line = buf; line && *line; line = next)
   {
      const char *s = line;
      if ((next = strchr(line, '\n')))
         *next++ = '\0';
      while (*s == ' ' || *s == '\t')
         s++;
      if (!strncmp(s, "retroachievements_password", 26)
            && strchr(s, '='))
         o += sprintf(o, "retroachievements_password = \n");
      else
         o += sprintf(o, "%s\n", line);
   }
   free(buf);

   if ((f = filestream_open(path, RETRO_VFS_FILE_ACCESS_WRITE,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
   {
      filestream_write(f, out, o - out);
      filestream_close(f);
   }
   free(out);
   bset.ra_password[0] = '\0';
}

#if defined(HAVE_CHEEVOS) && defined(HAVE_NETWORKING)
static void settings_login_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   rc_api_server_response_t server_response;
   rc_api_login_response_t response;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   settings_t *settings       = config_get_ptr();
   char msg[256];

   if (!data || !data->data || data->status < 0)
   {
      settings_message("RetroAchievements: couldn't reach the server to log in");
      return;
   }

   memset(&server_response, 0, sizeof(server_response));
   server_response.body             = data->data;
   server_response.body_length      = data->len;
   server_response.http_status_code = data->status;

   if (     rc_api_process_login_server_response(&response, &server_response)
               != RC_OK
         || !response.response.succeeded
         || string_is_empty(response.api_token))
   {
      snprintf(msg, sizeof(msg), "RetroAchievements: login failed (%s). "
            "Check bomba-settings.ini",
            response.response.error_message ? response.response.error_message
            : "wrong username or password");
      rc_api_destroy_login_response(&response);
      settings_message(msg);
      return;
   }

   /* As RetroArch keeps a login: the user and a token, no password. */
   strlcpy(settings->arrays.cheevos_username, response.username
         ? response.username : bset.ra_user,
         sizeof(settings->arrays.cheevos_username));
   strlcpy(settings->arrays.cheevos_token, response.api_token,
         sizeof(settings->arrays.cheevos_token));
   settings->arrays.cheevos_password[0] = '\0';
   rc_api_destroy_login_response(&response);

   settings_erase_password();
   command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);

   snprintf(msg, sizeof(msg), "RetroAchievements: logged in as %s",
         settings->arrays.cheevos_username);
   settings_message(msg);
}

static void settings_login(void)
{
   rc_api_request_t request;
   rc_api_login_request_t params;
   char ua[256];

   memset(&params, 0, sizeof(params));
   params.username = bset.ra_user;
   params.password = bset.ra_password;
   if (rc_api_init_login_request(&request, &params) != RC_OK)
      return;

   rcheevos_get_user_agent(get_rcheevos_locals(), ua, sizeof(ua));
   if (!task_push_http_post_transfer_with_user_agent(request.url,
            request.post_data, true, "POST", ua, settings_login_cb, NULL))
      settings_message("RetroAchievements: couldn't start the login");
   rc_api_destroy_request(&request);
}
#endif

void bomba_settings_init(void)
{
   char path[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();

   if (bset.done)
      return;
   bset.done = true;

   settings_path(path, sizeof(path));
   if (path_is_valid(path))
      settings_read(path);

   /* A different account without a password: RetroArch's login no longer
    * belongs to it. */
   if (     !string_is_empty(bset.ra_user)
         && !string_is_equal_case_insensitive(bset.ra_user,
               settings->arrays.cheevos_username)
         && string_is_empty(bset.ra_password))
      settings_message("RetroAchievements: enter the password for "
            "this account in bomba-settings.ini");

#if defined(HAVE_CHEEVOS) && defined(HAVE_NETWORKING)
   if (!string_is_empty(bset.ra_user) && !string_is_empty(bset.ra_password))
      settings_login();
#endif
}

const char *bomba_settings_steamgriddb_key(void)
{
   bomba_settings_init();
   return bset.griddb_key;
}
