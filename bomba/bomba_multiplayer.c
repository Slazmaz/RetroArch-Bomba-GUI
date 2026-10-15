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
#ifdef HAVE_MIST
#include "bomba_steamnet.h"
#endif

enum bomba_mp_mode bomba_mp_mode(void)
{
#ifdef HAVE_MIST
   switch (bomba_steamnet_state())
   {
      case BOMBA_STEAMNET_HOSTING:
         return BOMBA_MP_HOST;
      case BOMBA_STEAMNET_JOINED:
         return BOMBA_MP_JOIN;
      default:
         break;
   }
#endif
   return BOMBA_MP_NONE;
}

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
   enum bomba_mp_mode mode = bomba_mp_mode();
   char num[16];

   mp_setenv("BOMBA_MP_MODE", mode == BOMBA_MP_HOST ? "host"
         : mode == BOMBA_MP_JOIN ? "join" : NULL);
   mp_setenv("BOMBA_MP_PASSWORD", NULL);
#ifdef HAVE_MIST
   /* A guest's room is the tunnel's local end; the host's is the
    * emulator's own port, where the tunnel delivers. */
   mp_setenv("BOMBA_MP_ADDRESS", mode == BOMBA_MP_JOIN ? "127.0.0.1" : NULL);
   snprintf(num, sizeof(num), "%u", bomba_steamnet_local_port());
   mp_setenv("BOMBA_MP_PORT", mode == BOMBA_MP_JOIN ? num : NULL);
   snprintf(num, sizeof(num), "%u", BOMBA_STEAMNET_MAX_PLAYERS);
   mp_setenv("BOMBA_MP_MAX", mode == BOMBA_MP_HOST ? num : NULL);
   mp_setenv("BOMBA_MP_NAME", mode != BOMBA_MP_NONE
         ? bomba_steamnet_my_name() : NULL);
#else
   (void)num;
#endif
}
