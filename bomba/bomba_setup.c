/* Bomba: first-start setup (see bomba_setup.h). */

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <string/stdstring.h>

#include "bomba_setup.h"
#include "../command.h"
#include "../configuration.h"
#include "../verbosity.h"

#if defined(HAVE_CHEEVOS) && defined(HAVE_NETWORKING)
#include <rc_api_user.h>
#include "../tasks/tasks_internal.h"
#include "../cheevos/cheevos_locals.h"
#include "../cheevos/cheevos_client.h"
#endif

static struct
{
   char ra_user[128];
   char error[256];
   bool logging_in;
} bsetup;

/* "  value  " -> value, into s. */
static void setup_copy_trimmed(char *s, size_t len, const char *val)
{
   const char *e;
   while (*val == ' ' || *val == '\t')
      val++;
   e = val + strlen(val);
   while (e > val && (e[-1] == ' ' || e[-1] == '\t'))
      e--;
   if ((size_t)(e - val) + 1 < len)
      len = (size_t)(e - val) + 1;
   strlcpy(s, val, len);
}

#if defined(HAVE_CHEEVOS) && defined(HAVE_NETWORKING)
static void setup_login_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *error)
{
   rc_api_server_response_t server_response;
   rc_api_login_response_t response;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   settings_t *settings       = config_get_ptr();

   bsetup.logging_in = false;

   if (!data || !data->data || data->status < 0)
   {
      strlcpy(bsetup.error, "Couldn't reach RetroAchievements",
            sizeof(bsetup.error));
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
      strlcpy(bsetup.error, response.response.error_message
            ? response.response.error_message : "Wrong username or password",
            sizeof(bsetup.error));
      rc_api_destroy_login_response(&response);
      return;
   }

   /* As RetroArch keeps a login: the user and a token, no password. */
   strlcpy(settings->arrays.cheevos_username, response.username
         ? response.username : bsetup.ra_user,
         sizeof(settings->arrays.cheevos_username));
   strlcpy(settings->arrays.cheevos_token, response.api_token,
         sizeof(settings->arrays.cheevos_token));
   settings->arrays.cheevos_password[0] = '\0';
   rc_api_destroy_login_response(&response);
   /* Logging in turns achievements on, so they unlock in-game too. */
   settings->bools.cheevos_enable = true;

   command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);
}

static void setup_login(const char *password)
{
   rc_api_request_t request;
   rc_api_login_request_t params;
   char ua[256];

   memset(&params, 0, sizeof(params));
   params.username = bsetup.ra_user;
   params.password = password;
   if (rc_api_init_login_request(&request, &params) != RC_OK)
   {
      strlcpy(bsetup.error, "Couldn't start the login", sizeof(bsetup.error));
      return;
   }

   rcheevos_get_user_agent(get_rcheevos_locals(), ua, sizeof(ua));
   if (task_push_http_post_transfer_with_user_agent(request.url,
            request.post_data, true, "POST", ua, setup_login_cb, NULL))
      bsetup.logging_in = true;
   else
      strlcpy(bsetup.error, "Couldn't start the login", sizeof(bsetup.error));
   rc_api_destroy_request(&request);
}
#endif

bool bomba_setup_need_key(void)
{
   return string_is_empty(config_get_ptr()->arrays.bomba_steamgriddb_key);
}

bool bomba_setup_need_login(void)
{
#if defined(HAVE_CHEEVOS) && defined(HAVE_NETWORKING)
   settings_t *settings = config_get_ptr();
   return string_is_empty(settings->arrays.cheevos_username)
       || string_is_empty(settings->arrays.cheevos_token);
#else
   return false;
#endif
}

void bomba_setup_submit(const char *key, const char *user,
      const char *password)
{
   settings_t *settings = config_get_ptr();
   char trimmed[sizeof(settings->arrays.bomba_steamgriddb_key)];

   if (bsetup.logging_in)
      return;
   bsetup.error[0] = '\0';

   if (key)
   {
      setup_copy_trimmed(trimmed, sizeof(trimmed), key);
      if (!string_is_empty(trimmed))
      {
         strlcpy(settings->arrays.bomba_steamgriddb_key, trimmed,
               sizeof(settings->arrays.bomba_steamgriddb_key));
         command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);
      }
   }

#if defined(HAVE_CHEEVOS) && defined(HAVE_NETWORKING)
   if (user && !string_is_empty(password))
   {
      setup_copy_trimmed(bsetup.ra_user, sizeof(bsetup.ra_user), user);
      if (!string_is_empty(bsetup.ra_user))
         setup_login(password);
   }
#endif
}

bool bomba_setup_busy(void)
{
   return bsetup.logging_in;
}

const char *bomba_setup_error(void)
{
   return bsetup.error;
}

void bomba_setup_open_page(enum bomba_setup_page page)
{
   const char *url = page == BOMBA_SETUP_PAGE_GRIDDB_KEY
      ? "https://www.steamgriddb.com/profile/preferences/api"
      : "https://retroachievements.org/";
#ifdef _WIN32
   ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
   RARCH_LOG("[Bomba] Open %s\n", url);
#endif
}

const char *bomba_setup_steamgriddb_key(void)
{
   return config_get_ptr()->arrays.bomba_steamgriddb_key;
}
