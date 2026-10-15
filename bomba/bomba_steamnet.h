#ifndef __BOMBA_STEAMNET_H
#define __BOMBA_STEAMNET_H

#include <stdint.h>
#include <stddef.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Multiplayer sessions over Steam, so nobody forwards ports.
 *
 * A session is a friends-only Steam lobby owned by the host. Every player
 * hosts their own from the start, so friends can be invited at any time
 * from Steam's overlay; accepting an invite (or Join Game in the friends
 * list) moves into the friend's session instead, and back to one's own when
 * it ends. The emulator's own room traffic (UDP) goes through a tunnel over
 * Steam's relays (SteamNetworkingMessages):
 *   host:  the launcher core runs the room on 127.0.0.1:BOMBA_STEAMNET_ROOM_PORT
 *          as usual; each guest's packets reach it from a socket of their own.
 *   guest: a socket on 127.0.0.1 (bomba_steamnet_local_port) stands in for
 *          the room; the core joins it like any room.
 * The host's lobby also carries the game the host is playing, which guests
 * start too (see grid_home.c).
 *
 * Talks to Steam with its own copy of steam_api (mist's), in this process. */

/* Eden's default room port; the host's room runs there. */
#define BOMBA_STEAMNET_ROOM_PORT 24872
/* Players in a session, the host included. */
#define BOMBA_STEAMNET_MAX_PLAYERS 8

enum bomba_steamnet_state
{
   BOMBA_STEAMNET_OFF = 0,
   BOMBA_STEAMNET_CREATING, /* lobby on its way */
   BOMBA_STEAMNET_HOSTING,
   BOMBA_STEAMNET_JOINING,
   BOMBA_STEAMNET_JOINED
};

void bomba_steamnet_init(void);
void bomba_steamnet_poll(void);
void bomba_steamnet_deinit(void);

enum bomba_steamnet_state bomba_steamnet_state(void);
unsigned bomba_steamnet_local_port(void);
/* This player's Steam name. */
const char *bomba_steamnet_my_name(void);

/* Host: the game being started (from the grid), published to guests
 * once it runs. */
void bomba_steamnet_set_game(const char *system, const char *title);
/* Guest: the host's game; "" while the host plays nothing. */
const char *bomba_steamnet_host_system(void);
const char *bomba_steamnet_host_title(void);

RETRO_END_DECLS

#endif
