#ifndef __BOMBA_MULTIPLAYER_H
#define __BOMBA_MULTIPLAYER_H

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Multiplayer for launcher cores (cores that start another emulator, e.g.
 * Eden), one session for all of them, played over Steam (see
 * bomba_steamnet.h): each player hosts their own session from the start,
 * and joins a friend's from Steam (an invite, Join Game).
 *
 * Games started while a session is on play in it: before a core loads its
 * content the session is put in RetroArch's environment, which the core
 * reads (and passes to nothing it starts):
 *   BOMBA_MP_MODE      host | join (unset: no session)
 *   BOMBA_MP_ADDRESS   room to join (the Steam tunnel, on 127.0.0.1)
 *   BOMBA_MP_PORT      room's port (unset: the emulator's own)
 *   BOMBA_MP_PASSWORD  unset (Steam decides who gets in)
 *   BOMBA_MP_MAX       players in a hosted room
 *   BOMBA_MP_NAME      the player's name: their Steam name
 * RetroArch's own netplay is separate and unaffected. */

enum bomba_mp_mode
{
   BOMBA_MP_NONE = 0,
   BOMBA_MP_HOST,
   BOMBA_MP_JOIN
};

/* The session Steam has set up (none while it is being set up). */
enum bomba_mp_mode bomba_mp_mode(void);

void bomba_mp_export_env(void);

RETRO_END_DECLS

#endif
