#ifndef __BOMBA_MULTIPLAYER_H
#define __BOMBA_MULTIPLAYER_H

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Multiplayer for launcher cores (cores that start another emulator, e.g.
 * Eden), one session for all of them. Started from the grid's Multiplayer
 * screen by hosting or joining a room, it lasts until ended there or until
 * RetroArch closes (it is never saved). The last room's address, port and
 * player count are remembered as the screen's starting values; the
 * password isn't.
 *
 * While a session is on, games started then play in it: before a core
 * loads its content the session is put in RetroArch's environment, which
 * the core reads (and passes to nothing it starts):
 *   BOMBA_MP_MODE      host | join (unset: no session)
 *   BOMBA_MP_ADDRESS   room to join
 *   BOMBA_MP_PORT      room's port (unset: the emulator's own)
 *   BOMBA_MP_PASSWORD  room's password (unset: none)
 *   BOMBA_MP_MAX       players in a hosted room
 *   BOMBA_MP_NAME      the player's name: the current profile's
 * RetroArch's own netplay is separate and unaffected. */

enum bomba_mp_mode
{
   BOMBA_MP_NONE = 0,
   BOMBA_MP_HOST,
   BOMBA_MP_JOIN
};

#define BOMBA_MP_MAX_PLAYERS_MIN 2
#define BOMBA_MP_MAX_PLAYERS_MAX 16

/* Starts a session (replacing any). port 0: each emulator's own. */
void bomba_mp_start(enum bomba_mp_mode mode, const char *address,
      unsigned port, const char *password, unsigned max_players);
void bomba_mp_end(void);

enum bomba_mp_mode bomba_mp_mode(void);
const char *bomba_mp_address(void);
unsigned bomba_mp_port(void);
bool bomba_mp_has_password(void);
unsigned bomba_mp_max_players(void);

void bomba_mp_export_env(void);

RETRO_END_DECLS

#endif
