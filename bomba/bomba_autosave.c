/* Bomba: settings save when they change (see bomba_autosave.h).
 *
 * RetroArch only writes its config on a clean quit, so a crash, Steam's
 * Stop button or a shutdown with RetroArch open loses every change. Here
 * the settings and key binds are compared about once a second with what
 * was last saved, and the config is written (quietly) when they differ.
 *
 * Not while an override or remap is active: those change settings that
 * must not end up in the main config (RetroArch drops them before its own
 * save on quit); their changes are saved once the game closes. */

#include <stdlib.h>
#include <string.h>

#include <features/features_cpu.h>
#include <string/stdstring.h>
#include <compat/strl.h>

#include "bomba_autosave.h"
#include "../configuration.h"
#include "../paths.h"
#include "../runloop.h"
#include "../verbosity.h"
#include "../gfx/video_driver.h"
#include "../input/input_driver.h"

#define AUTOSAVE_INTERVAL_US 1000000

static struct
{
   settings_t *settings;
   retro_keybind_set binds[MAX_USERS];
   retro_time_t next_check;
} autosave_st;

static void autosave_keep(settings_t *settings)
{
   memcpy(autosave_st.settings, settings, sizeof(*settings));
   memcpy(autosave_st.binds, input_config_binds, sizeof(autosave_st.binds));
}

static bool autosave_blocked(runloop_state_t *runloop_st)
{
   return (runloop_st->flags & (RUNLOOP_FLAG_SHUTDOWN_INITIATED
            | RUNLOOP_FLAG_OVERRIDES_ACTIVE
            | RUNLOOP_FLAG_REMAPS_CORE_ACTIVE
            | RUNLOOP_FLAG_REMAPS_CONTENT_DIR_ACTIVE
            | RUNLOOP_FLAG_REMAPS_GAME_ACTIVE))
       || !string_is_empty(runloop_st->name.remapfile);
}

void bomba_autosave_poll(void)
{
   settings_t *settings           = config_get_ptr();
   runloop_state_t *runloop_st    = runloop_state_get_ptr();
   video_driver_state_t *video_st = video_state_get_ptr();
   const char *config_path        = path_get(RARCH_PATH_CONFIG);
   retro_time_t now               = cpu_features_get_time_usec();
   char forced_driver[sizeof(settings->arrays.video_driver)];

   if (now < autosave_st.next_check)
      return;
   autosave_st.next_check = now + AUTOSAVE_INTERVAL_US;

   /* Save on exit off: the user saves by hand. */
   if (     !settings || string_is_empty(config_path)
         || !settings->bools.config_save_on_exit)
      return;

   /* First check: what was loaded at start is the saved state. */
   if (!autosave_st.settings)
   {
      if ((autosave_st.settings = (settings_t*)malloc(sizeof(*settings))))
         autosave_keep(settings);
      return;
   }

   if (autosave_blocked(runloop_st))
      return;

   if (     !memcmp(autosave_st.settings, settings, sizeof(*settings))
         && !memcmp(autosave_st.binds, input_config_binds,
               sizeof(autosave_st.binds)))
      return;

   /* A core can force a video driver for its game; the config keeps the
    * user's own one, as RetroArch's save on quit does. */
   forced_driver[0] = '\0';
   if (video_st->cached_driver_id[0])
   {
      strlcpy(forced_driver, settings->arrays.video_driver,
            sizeof(forced_driver));
      strlcpy(settings->arrays.video_driver, video_st->cached_driver_id,
            sizeof(settings->arrays.video_driver));
   }

   if (config_save_file(config_path))
      RARCH_LOG("[Bomba] Settings changed, saved \"%s\".\n", config_path);
   else
      RARCH_ERR("[Bomba] Settings changed, couldn't save \"%s\".\n",
            config_path);

   if (forced_driver[0])
      strlcpy(settings->arrays.video_driver, forced_driver,
            sizeof(settings->arrays.video_driver));

   /* Kept even when saving failed: retried on the next change, not every
    * second. */
   autosave_keep(settings);
}
