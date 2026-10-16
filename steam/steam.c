#include <ctype.h>
#include <mist.h>
#include <stdio.h>
#include <string.h>

#include <retro_timers.h>
#include <compat/strl.h>
#include <string/stdstring.h>
#include <retro_miscellaneous.h>

#include "../input/input_driver.h"
#include "../menu/menu_driver.h"
#include "../menu/menu_entries.h"
#include "../retroarch.h"
#include "../runloop.h"
#include "paths.h"
#include "verbosity.h"

#include "steam.h"
#include "../bomba/bomba_steamnet.h"

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#endif

static bool mist_initialized                = false;
static bool mist_showing_osk                = false;
static steam_core_dlc_list_t *mist_dlc_list = NULL;
static enum presence last_presence          = PRESENCE_NONE;

#ifdef _WIN32
/* Steam injects its overlay into the process it starts, and that passes it
 * on to the processes it starts; a launch script (cmd) can lose it, and
 * then mist's own SteamAPI_Init loads the overlay into mist.exe, which
 * draws nothing. Load it here if missing, before any window or graphics
 * context exists, the same way steam_api does. */
static void steam_load_overlay(void)
{
   HKEY key;
   char steam_path[MAX_PATH];
   char dll_path[MAX_PATH + 32];
   DWORD size        = sizeof(steam_path);
   DWORD type        = 0;
   const char *appid = getenv("SteamAppId");

   if (GetModuleHandleA("gameoverlayrenderer64.dll"))
      return;
   /* Only when Steam launched us; the overlay needs the game id. */
   if (!appid || !*appid)
      return;
   if (!getenv("SteamGameId"))
      SetEnvironmentVariableA("SteamGameId", appid);
   if (!getenv("SteamOverlayGameId"))
      SetEnvironmentVariableA("SteamOverlayGameId", appid);

   if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam",
         0, KEY_READ, &key) != ERROR_SUCCESS)
      return;
   if (RegQueryValueExA(key, "SteamPath", NULL, &type,
         (LPBYTE)steam_path, &size) != ERROR_SUCCESS || type != REG_SZ)
   {
      RegCloseKey(key);
      return;
   }
   RegCloseKey(key);
   steam_path[sizeof(steam_path) - 1] = '\0';

   snprintf(dll_path, sizeof(dll_path), "%s\\gameoverlayrenderer64.dll",
         steam_path);

   if (LoadLibraryA(dll_path))
      RARCH_LOG("[Steam] Loaded overlay from \"%s\".\n", dll_path);
   else
      RARCH_ERR("[Steam] Failed to load overlay \"%s\" (%lu).\n",
            dll_path, GetLastError());
}

/* n bytes at rva of a loaded module, as its dll file has them. */
static bool steam_module_file_bytes(HMODULE mod, DWORD rva,
      unsigned char *out, DWORD n)
{
   char path[MAX_PATH];
   IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)mod;
   IMAGE_NT_HEADERS *nt  = (IMAGE_NT_HEADERS*)((BYTE*)mod + dos->e_lfanew);
   IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
   LARGE_INTEGER pos;
   DWORD got  = 0;
   bool ok    = false;
   HANDLE f;
   WORD i;

   for (i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
      if (     rva >= s->VirtualAddress
            && rva + n <= s->VirtualAddress + s->SizeOfRawData)
         break;
   if (     i == nt->FileHeader.NumberOfSections
         || !GetModuleFileNameA(mod, path, sizeof(path)))
      return false;

   f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
         0, NULL);
   if (f == INVALID_HANDLE_VALUE)
      return false;
   pos.QuadPart = s->PointerToRawData + (rva - s->VirtualAddress);
   if (     SetFilePointerEx(f, pos, NULL, FILE_BEGIN)
         && ReadFile(f, out, n, &got, NULL) && got == n)
      ok = true;
   CloseHandle(f);
   return ok;
}

/* The overlay detours XInputGetStateEx (ordinal 100), which the xinput
 * joypad driver reads the guide button through, and drops the guide
 * button, so it can't open the menu. Whether the overlay came in with
 * Steam's injection or steam_load_overlay, put the function's start back
 * as the dll file has it (its first bytes hold no addresses to relocate). */
static void steam_keep_guide_button(void)
{
   static const char *xinput_dlls[] = { "xinput1_4.dll", "xinput1_3.dll" };
   size_t i;

   if (!GetModuleHandleA("gameoverlayrenderer64.dll"))
      return;

   for (i = 0; i < 2; i++)
   {
      unsigned char *fn, orig[16];
      HMODULE x;
      DWORD old;

      if (     !(x = LoadLibraryA(xinput_dlls[i]))
            || !(fn = (unsigned char*)GetProcAddress(x, (LPCSTR)100))
            || !steam_module_file_bytes(x, (DWORD)(fn - (unsigned char*)x),
                  orig, sizeof(orig)))
         continue;

      if (     memcmp(fn, orig, sizeof(orig))
            && VirtualProtect(fn, sizeof(orig), PAGE_EXECUTE_READWRITE, &old))
      {
         memcpy(fn, orig, sizeof(orig));
         VirtualProtect(fn, sizeof(orig), old, &old);
         FlushInstructionCache(GetCurrentProcess(), fn, sizeof(orig));
         RARCH_LOG("[Steam] Kept the guide button in %s.\n", xinput_dlls[i]);
      }
   }
}
#endif

void steam_init(void)
{
   MistResult result;

#ifdef _WIN32
   steam_load_overlay();
   steam_keep_guide_button();
#endif

   result = mist_subprocess_init();

   if (MIST_IS_SUCCESS(result))
      mist_initialized = true;
   else
      RARCH_ERR("[Steam] Failed to initialize mist subprocess (%d-%d).\n", MIST_UNPACK_RESULT(result));

   bomba_steamnet_init();
}

void steam_poll(void)
{
   MistCallbackMsg callback;
   steam_core_dlc_list_t *core_dlc_list;
   bool has_callback                     = false;
   settings_t* settings                  = config_get_ptr();
   static bool has_poll_errored          = false;
   static bool has_rich_presence_enabled = false;
   MistResult result                     = mist_poll();

   bomba_steamnet_poll();

   if (MIST_IS_ERROR(result))
   {
      if (has_poll_errored)
         return;

      RARCH_ERR("[Steam] Error polling (%d-%d).\n", MIST_UNPACK_RESULT(result));

      has_poll_errored = true;
   }

   result = mist_next_callback(&has_callback, &callback);
   if (MIST_IS_ERROR(result))
      return;

   while (has_callback && MIST_IS_SUCCESS(result))
   {
      switch (callback.callback)
      {
         /* Reload core info and Steam Core DLC mappings */
         case MistCallback_DlcInstalled:
            command_event(CMD_EVENT_CORE_INFO_INIT, NULL);
            steam_get_core_dlcs(&core_dlc_list, false);
            break;
         /* The Steam OSK is dismissed */
         case MistCallback_FloatingGamepadTextInputDismissed:
            /* If we do not poll for input the callback might race condition and
               will dismiss the input even when enter is pressed */
            retro_sleep(50);
            runloop_iterate();
            menu_input_dialog_end();
            break;
      }

      result = mist_next_callback(&has_callback, &callback);
   }

   /* Ensure rich presence state is correct */
   if (settings->bools.steam_rich_presence_enable != has_rich_presence_enabled)
   {
      steam_update_presence(last_presence, true);
      has_rich_presence_enabled = settings->bools.steam_rich_presence_enable;
   }
}

steam_core_dlc_list_t *steam_core_dlc_list_new(size_t count)
{
   steam_core_dlc_list_t *core_dlc_list = (steam_core_dlc_list_t*)
      malloc(sizeof(*core_dlc_list));

   core_dlc_list->list  = (steam_core_dlc_t*)
      malloc(count * sizeof(*core_dlc_list->list));
   core_dlc_list->count = 0; /* This is incremented inside the setup function */

   return core_dlc_list;
}

void steam_core_dlc_list_free(steam_core_dlc_list_t *list)
{
   size_t i;
   if (!list)
      return;

   for (i = 0; list->count > i; i++)
   {
      if (list->list[i].name)
         free(list->list[i].name);
      if (list->list[i].name_lower)
         free(list->list[i].name_lower);
   }

   free(list->list);
   free(list);
}

steam_core_dlc_t *steam_core_dlc_list_get(steam_core_dlc_list_t *list, size_t i)
{
   if (!list || (i >= list->count))
      return NULL;

   return &list->list[i];
}

/* Sort the dlc cores alphabetically based on their name */
static int dlc_core_qsort_cmp(const void *a_, const void *b_)
{
   const steam_core_dlc_t *a = (const steam_core_dlc_t*)a_;
   const steam_core_dlc_t *b = (const steam_core_dlc_t*)b_;

   return strcasecmp(a->name, b->name);
}

/* Find core info for dlcs
 * TODO: This currently only uses core info for cores that are installed */
core_info_t* steam_find_core_info_for_dlc(const char* name)
{
   int i;
   core_info_list_t *core_info_list = NULL;
   core_info_get_list(&core_info_list);

   if (!core_info_list)
      return NULL;

   for (i = 0; core_info_list->count > i; i++)
   {
      char core_info_name[NAME_MAX_LENGTH];
      core_info_t *core_info   = core_info_get(core_info_list, i);
      /* Find the opening parenthesis for the core name */
      char *start              = strchr(core_info->display_name, '(');
      if (!start)
         continue;

      /* Skip the first parenthesis and copy it to the stack */
      strlcpy(core_info_name, start + 1, sizeof(core_info_name));

      /* Null terminate at the closing parenthesis. */
      char *end = strchr((const char*)&core_info_name, ')');
      if (!end)
            continue;

      *end = '\0';

      /* Make it lowercase */
      string_to_lower((char*)&core_info_name);

      /* Check if it matches */
      if (strcasecmp(core_info_name, name) == 0)
         return core_info;
   }

   return NULL;
}

/* Generate a list with core dlcs
 * Needs to be called after initialization because it uses core info */
MistResult steam_generate_core_dlcs_list(steam_core_dlc_list_t **list)
{
   int count, i;
   steam_core_dlc_list_t *dlc_list = NULL;
   char dlc_name[PATH_MAX_LENGTH]  = { 0 };
   bool available                  = false;
   MistResult result               = mist_steam_apps_get_dlc_count(&count);
   if (MIST_IS_ERROR(result))
      goto error;

   dlc_list = steam_core_dlc_list_new(count);
   for (i = 0; count > i; i++)
   {
      steam_core_dlc_t core_dlc;

      result = mist_steam_apps_get_dlc_data_by_index(i, &core_dlc.app_id, &available, (char*)&dlc_name, PATH_MAX_LENGTH);
      if (MIST_IS_ERROR(result))
         goto error;

      /* Strip away the "RetroArch - " prefix if present */
      if (strncmp(dlc_name, "RetroArch - ", sizeof("RetroArch - ") - 1) == 0)
         core_dlc.name = strdup(dlc_name + sizeof("RetroArch - ") - 1);
      else
         core_dlc.name = strdup(dlc_name);

      /* Make a lower case version */
      core_dlc.name_lower = strdup(core_dlc.name);
      string_to_lower(core_dlc.name_lower);

      core_dlc.core_info = steam_find_core_info_for_dlc(core_dlc.name_lower);

      dlc_list->list[i] = core_dlc;
      dlc_list->count++;
   }

   /* Sort the list */
   qsort(dlc_list->list, dlc_list->count,
         sizeof(steam_core_dlc_t), dlc_core_qsort_cmp);

   *list = dlc_list;
   return MistResult_Success;

error:
   if (dlc_list)
      steam_core_dlc_list_free(dlc_list);

   return result;
}

MistResult steam_get_core_dlcs(steam_core_dlc_list_t **list, bool cached)
{
   MistResult result;
   steam_core_dlc_list_t *new_list = NULL;

   if (cached && mist_dlc_list)
   {
      *list = mist_dlc_list;
      return MistResult_Success;
   }

   result = steam_generate_core_dlcs_list(&new_list);
   if (MIST_IS_ERROR(result))
      return result;

   if (mist_dlc_list)
      steam_core_dlc_list_free(mist_dlc_list);

   mist_dlc_list = new_list;
   *list         = new_list;

   return MistResult_Success;
}

steam_core_dlc_t* steam_get_core_dlc_by_name(
      steam_core_dlc_list_t *list, const char *name)
{
   int i;
   steam_core_dlc_t *core_info;

   for (i = 0; list->count > i; i++)
   {
      core_info = steam_core_dlc_list_get(list, i);
      if (strcasecmp(core_info->name, name) == 0)
         return core_info;
   }

   return NULL;
}

void steam_install_core_dlc(steam_core_dlc_t *core_dlc)
{
   size_t _len;
   char msg[128] = { 0 };
   bool downloading          = false;
   bool installed            = false;
   uint64_t bytes_downloaded = 0;
   uint64_t bytes_total      = 0;
   /* Check if the core is already being downloaded */
   MistResult result         = mist_steam_apps_get_dlc_download_progress(core_dlc->app_id, &downloading, &bytes_downloaded, &bytes_total);
   if (MIST_IS_ERROR(result))
      goto error;

   /* Check if the core is already installed */
   result = mist_steam_apps_is_dlc_installed(core_dlc->app_id, &installed);
   if (MIST_IS_ERROR(result))
      goto error;

   if (downloading || installed)
   {
      const char *_msg = msg_hash_to_str(MSG_CORE_STEAM_CURRENTLY_DOWNLOADING);
      runloop_msg_queue_push(_msg, strlen(_msg), 1, 180, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);
      return;
   }

   result = mist_steam_apps_install_dlc(core_dlc->app_id);
   if (MIST_IS_ERROR(result))
      goto error;

   task_push_steam_core_dlc_install(core_dlc->app_id, core_dlc->name);

   return;
error:
   _len = snprintf(msg, sizeof(msg), "%s: (%d-%d)",
         msg_hash_to_str(MSG_ERROR),
         MIST_UNPACK_RESULT(result));

   runloop_msg_queue_push(msg, _len, 1, 180, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);

   RARCH_ERR("[Steam] Error installing DLC %d (%d-%d).\n", core_dlc->app_id, MIST_UNPACK_RESULT(result));
}

void steam_uninstall_core_dlc(steam_core_dlc_t *core_dlc)
{
   size_t _len;
   const char *_msg  = NULL;
   char msg[128]     = { 0 };
   MistResult result = mist_steam_apps_uninstall_dlc(core_dlc->app_id);

   if (MIST_IS_ERROR(result))
      goto error;

   _msg = msg_hash_to_str(MSG_CORE_STEAM_UNINSTALLED);
   runloop_msg_queue_push(_msg, strlen(_msg), 1, 180, true, NULL,
      MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
   return;

error:
   _len = snprintf(msg, sizeof(msg), "%s: (%d-%d)",
         msg_hash_to_str(MSG_ERROR),
         MIST_UNPACK_RESULT(result));

   runloop_msg_queue_push(msg, _len, 1, 180, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);

   RARCH_ERR("[Steam] Error uninstalling DLC %d (%d-%d).\n", core_dlc->app_id, MIST_UNPACK_RESULT(result));
}

bool steam_open_osk(void)
{
   bool                     shown = false;
   bool                   on_deck = false;
   video_driver_state_t *video_st = video_state_get_ptr();

   /* Only open the Steam OSK if running on a Steam Deck,
      as currently the Big Picture OSK seems to be semi-broken */
   mist_steam_utils_is_steam_running_on_steam_deck(&on_deck);
   if (!on_deck)
      return false;

   mist_steam_utils_show_floating_gamepad_text_input(
      MistFloatingGamepadTextInputMode_SingleLine,
      0,
      0,
      video_st->width,
      video_st->height / 2,
      &shown
   );

   mist_showing_osk = shown;

   return shown;
}

bool steam_has_osk_open(void)
{
   return mist_showing_osk;
}

/* Bomba: how friends join this player's multiplayer session ("" when there
 * is none), as the rich presence key Steam reads for "Invite to Game" and
 * "Join Game". Set through mist, which owns the game's rich presence and
 * clears all of it, so it is put back after every update. */
static char steam_connect[64];

/* Bomba: the grid's name for the game it starts (RetroAchievements'
 * title, else the file's), shown instead of the playlist label or file
 * name while that content runs. */
static char steam_game_path[PATH_MAX_LENGTH];
static char steam_game_title[256];

void steam_set_game_title(const char *path, const char *title)
{
   strlcpy(steam_game_path,  path  ? path  : "", sizeof(steam_game_path));
   strlcpy(steam_game_title, title ? title : "", sizeof(steam_game_title));
}

void steam_set_connect(const char *connect)
{
   strlcpy(steam_connect, connect ? connect : "", sizeof(steam_connect));
   if (mist_initialized)
      mist_steam_friends_set_rich_presence("connect", steam_connect);
}

static void steam_update_presence_inner(enum presence presence, bool force);

void steam_update_presence(enum presence presence, bool force)
{
   steam_update_presence_inner(presence, force);
   if (mist_initialized && steam_connect[0])
      mist_steam_friends_set_rich_presence("connect", steam_connect);
}

static void steam_update_presence_inner(enum presence presence, bool force)
{
   settings_t* settings = config_get_ptr();

   if (!mist_initialized)
      return;

   /* Avoid spamming steam with presence updates */
   if (presence == last_presence && !force)
      return;
   last_presence = presence;

   /* Ensure rich presence is enabled */
   if (!settings->bools.steam_rich_presence_enable)
   {
      mist_steam_friends_clear_rich_presence();
      return;
   }

   switch (presence)
   {
      case PRESENCE_MENU:
         mist_steam_friends_set_rich_presence("steam_display", "#Status_InMenu");
         break;
      case PRESENCE_GAME_PAUSED:
         mist_steam_friends_set_rich_presence("steam_display", "#Status_Paused");
         break;
      case PRESENCE_GAME:
         {
            size_t _len;
            char content[PATH_MAX_LENGTH];
            const char *label                  = NULL;
            const struct playlist_entry *entry = NULL;
            core_info_t *core_info             = NULL;
            playlist_t *current_playlist       = playlist_get_cached();

            core_info_get_current_core(&core_info);

            if (     !string_is_empty(steam_game_title)
                  && string_is_equal(steam_game_path, path_get(RARCH_PATH_CONTENT)))
               label = steam_game_title;
            else if (current_playlist)
            {
               playlist_get_index_by_path(
                     current_playlist,
                     path_get(RARCH_PATH_CONTENT),
                     &entry);

               if (entry && !string_is_empty(entry->label))
                  label = entry->label;
            }

            if (!label)
               label = path_basename(path_get(RARCH_PATH_BASENAME));

            switch(settings->uints.steam_rich_presence_format)
            {
               case STEAM_RICH_PRESENCE_FORMAT_CONTENT:
                  strlcpy(content, label, sizeof(content));
                  break;
               case STEAM_RICH_PRESENCE_FORMAT_CORE:
                  if (core_info)
                     strlcpy(content, core_info->core_name, sizeof(content));
                  else
                  {
                     content[0] = 'N';
                     content[1] = '/';
                     content[2] = 'A';
                     content[3] = '\0';
                  }
                  break;
               case STEAM_RICH_PRESENCE_FORMAT_SYSTEM:
                  if (core_info)
                     strlcpy(content, core_info->systemname, sizeof(content));
                  else
                  {
                     content[0] = 'N';
                     content[1] = '/';
                     content[2] = 'A';
                     content[3] = '\0';
                  }
                  break;
               case STEAM_RICH_PRESENCE_FORMAT_CONTENT_SYSTEM:
                  _len               = strlcpy(content, label, sizeof(content));
                  content[_len  ]    = ' ';
                  content[_len+1]    = '(';
                  content[_len+2]    = '\0';
                  if (core_info)
                  {
                     _len           += 2;
                     _len           += strlcpy(content + _len, core_info->systemname,
                           sizeof(content) - _len);
                     content[_len  ] = ')';
                     content[_len+1] = '\0';
                  }
                  else
                  {
                     content[_len+2] = 'N';
                     content[_len+3] = '/';
                     content[_len+4] = 'A';
                     content[_len+5] = ')';
                     content[_len+6] = '\0';
                  }
                  break;
               case STEAM_RICH_PRESENCE_FORMAT_CONTENT_CORE:
                  _len               = strlcpy(content, label, sizeof(content));
                  content[_len  ]    = ' ';
                  content[_len+1]    = '(';
                  content[_len+2]    = '\0';
                  if (core_info)
                  {
                     _len           += 2;
                     _len           += strlcpy(content + _len, core_info->core_name,
                           sizeof(content) - _len);
                     content[_len  ] = ')';
                     content[_len+1] = '\0';
                  }
                  else
                  {
                     content[_len+2] = 'N';
                     content[_len+3] = '/';
                     content[_len+4] = 'A';
                     content[_len+5] = ')';
                     content[_len+6] = '\0';
                  }
                  break;
               case STEAM_RICH_PRESENCE_FORMAT_CONTENT_SYSTEM_CORE:
                  _len               = strlcpy(content, label, sizeof(content));
                  content[_len  ]    = ' ';
                  content[_len+1]    = '(';
                  content[_len+2]    = '\0';
                  if (core_info)
                  {
                     _len           += 2;
                     _len           += strlcpy(content + _len, core_info->systemname,
                           sizeof(content) - _len);
                     content[_len  ] = ' ';
                     content[_len+1] = '-';
                     content[_len+2] = ' ';
                     _len           += 3;
                     _len           += strlcpy(content + _len, core_info->core_name,
                           sizeof(content) - _len);
                     content[_len  ] = ')';
                     content[_len+1] = '\0';
                  }
                  else
                  {
                     content[_len+2]  = 'N';
                     content[_len+3]  = '/';
                     content[_len+4]  = 'A';
                     content[_len+5]  = ' ';
                     content[_len+6]  = '-';
                     content[_len+7]  = ' ';
                     content[_len+8]  = 'N';
                     content[_len+9]  = '/';
                     content[_len+10] = 'A';
                     content[_len+11] = ')';
                     content[_len+12] = '\0';
                  }
                  break;
               case STEAM_RICH_PRESENCE_FORMAT_NONE:
               default:
                  content[0] = '\0';
                  break;
            }


            mist_steam_friends_set_rich_presence("content", content);
            mist_steam_friends_set_rich_presence("steam_display",
                  settings->uints.steam_rich_presence_format != STEAM_RICH_PRESENCE_FORMAT_NONE
                  ? "#Status_RunningContent" : "#Status_Running" );
         }
         break;
      default:
         break;
   }
}

void steam_deinit(void)
{
   MistResult result;

   bomba_steamnet_deinit();
   result = mist_subprocess_deinit();

   /* Free the cached dlc list */
   if (mist_dlc_list)
      steam_core_dlc_list_free(mist_dlc_list);

   if (MIST_IS_SUCCESS(result))
      mist_initialized = false;
   else
      RARCH_ERR("[Steam] Failed to deinitialize mist subprocess (%d-%d).\n", MIST_UNPACK_RESULT(result));
}
