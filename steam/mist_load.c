/* mist.dll is loaded at runtime instead of linked, so retroarch.exe starts
 * without Steam's files (a release run outside Steam). Each mist function
 * the fork calls is defined here and forwards to the dll; without the dll
 * it fails the way mist does when its subprocess can't be found. */

#include <windows.h>

#include <mist.h>

#define MIST_LOAD_MISSING (((MistResult)MistError_SubprocessNotFound << 16) | MistResult_Mist)

static FARPROC mist_load_proc(const char *name)
{
   static HMODULE mist = NULL;
   static bool tried   = false;

   if (!tried)
   {
      tried = true;
      mist  = LoadLibraryA("mist.dll");
   }
   return mist ? GetProcAddress(mist, name) : NULL;
}

#define MIST_FORWARD(name, params, args) \
   MistResult name params \
   { \
      static MistResult (*fn) params = NULL; \
      if (!fn) \
         fn = (MistResult (*) params)mist_load_proc(#name); \
      return fn ? fn args : MIST_LOAD_MISSING; \
   }

MIST_FORWARD(mist_subprocess_init, (void), ())
MIST_FORWARD(mist_subprocess_deinit, (void), ())
MIST_FORWARD(mist_poll, (void), ())
MIST_FORWARD(mist_next_callback,
      (bool *has_callback, struct MistCallbackMsg *p_callback),
      (has_callback, p_callback))
MIST_FORWARD(mist_steam_apps_get_dlc_count, (int32_t *dlc_count), (dlc_count))
MIST_FORWARD(mist_steam_apps_get_dlc_data_by_index,
      (int32_t dlc, AppId *app_id, bool *availiable, char *name,
       uint32_t name_size),
      (dlc, app_id, availiable, name, name_size))
MIST_FORWARD(mist_steam_apps_get_dlc_download_progress,
      (AppId app_id, bool *downloading, uint64_t *bytes_downloaded,
       uint64_t *bytes_total),
      (app_id, downloading, bytes_downloaded, bytes_total))
MIST_FORWARD(mist_steam_apps_is_dlc_installed,
      (AppId app_id, bool *installed), (app_id, installed))
MIST_FORWARD(mist_steam_apps_install_dlc, (AppId app_id), (app_id))
MIST_FORWARD(mist_steam_apps_uninstall_dlc, (AppId app_id), (app_id))
MIST_FORWARD(mist_steam_friends_clear_rich_presence, (void), ())
MIST_FORWARD(mist_steam_friends_set_rich_presence,
      (const char *key, const char *value), (key, value))
MIST_FORWARD(mist_steam_utils_is_steam_running_on_steam_deck,
      (bool *on_deck), (on_deck))
MIST_FORWARD(mist_steam_utils_show_floating_gamepad_text_input,
      (enum MistFloatingGamepadTextInputMode keyboard_mode,
       int text_field_x_position, int text_field_y_position,
       int text_field_width, int text_field_height, bool *shown),
      (keyboard_mode, text_field_x_position, text_field_y_position,
       text_field_width, text_field_height, shown))
