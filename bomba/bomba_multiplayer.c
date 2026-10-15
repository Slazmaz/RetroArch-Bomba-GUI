/* Bomba: the multiplayer session for launcher cores, handed
 * to them through the environment (see bomba_multiplayer.h). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <string/stdstring.h>

#include "bomba_multiplayer.h"
#include "grid_profile.h"
#include "../configuration.h"

/* The session: only in memory, so it ends with RetroArch. */
static struct
{
   enum bomba_mp_mode mode;
   char address[256];
   unsigned port;
   char password[128];
   unsigned max_players;
} mp;

void bomba_mp_start(enum bomba_mp_mode mode, const char *address,
      unsigned port, const char *password, unsigned max_players)
{
   settings_t *settings = config_get_ptr();

   if (max_players < BOMBA_MP_MAX_PLAYERS_MIN)
      max_players = BOMBA_MP_MAX_PLAYERS_MIN;
   if (max_players > BOMBA_MP_MAX_PLAYERS_MAX)
      max_players = BOMBA_MP_MAX_PLAYERS_MAX;

   mp.mode        = mode;
   mp.port        = port;
   mp.max_players = max_players;
   strlcpy(mp.address,  mode == BOMBA_MP_JOIN && address ? address : "",
         sizeof(mp.address));
   strlcpy(mp.password, password ? password : "", sizeof(mp.password));

   /* The screen starts from these next time. */
   if (mode == BOMBA_MP_JOIN)
      strlcpy(settings->paths.bomba_mp_address, mp.address,
            sizeof(settings->paths.bomba_mp_address));
   settings->uints.bomba_mp_port        = port;
   settings->uints.bomba_mp_max_players = max_players;
}

void bomba_mp_end(void)
{
   memset(&mp, 0, sizeof(mp));
}

enum bomba_mp_mode bomba_mp_mode(void) { return mp.mode; }
const char *bomba_mp_address(void)       { return mp.address; }
unsigned bomba_mp_port(void)             { return mp.port; }
bool bomba_mp_has_password(void)         { return mp.password[0] != '\0'; }
unsigned bomba_mp_max_players(void)      { return mp.max_players; }

static void mp_setenv(const char *name, const char *value)
{
#ifdef _WIN32
   SetEnvironmentVariableA(name, string_is_empty(value) ? NULL : value);
#else
   if (string_is_empty(value))
      unsetenv(name);
   else
      setenv(name, value, 1);
#endif
}

void bomba_mp_export_env(void)
{
   char num[16];
   bool on = mp.mode != BOMBA_MP_NONE;

   mp_setenv("BOMBA_MP_MODE", mp.mode == BOMBA_MP_HOST ? "host"
         : mp.mode == BOMBA_MP_JOIN ? "join" : NULL);
   mp_setenv("BOMBA_MP_ADDRESS",  on ? mp.address  : NULL);
   mp_setenv("BOMBA_MP_PASSWORD", on ? mp.password : NULL);
   snprintf(num, sizeof(num), "%u", mp.port);
   mp_setenv("BOMBA_MP_PORT", on && mp.port ? num : NULL);
   snprintf(num, sizeof(num), "%u", mp.max_players);
   mp_setenv("BOMBA_MP_MAX", on ? num : NULL);
   mp_setenv("BOMBA_MP_NAME", on
         ? grid_profile_name(grid_profile_current()) : NULL);
}
