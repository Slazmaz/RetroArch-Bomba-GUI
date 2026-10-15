#ifndef __BOMBA_SETUP_H
#define __BOMBA_SETUP_H

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* What Bomba needs before the grid is usable, asked for on its Setup
 * screen (grid_home.c) and kept in retroarch.cfg:
 *    bomba_steamgriddb_key          covers and banners (grid_cover.c)
 *    cheevos_username, cheevos_token
 *       RetroAchievements: the password logs in once and isn't kept */

enum bomba_setup_page
{
   BOMBA_SETUP_PAGE_GRIDDB_KEY = 0, /* where to get a SteamGridDB key */
   BOMBA_SETUP_PAGE_RA_SIGNUP       /* making a RetroAchievements account */
};

bool bomba_setup_need_key(void);
bool bomba_setup_need_login(void);

/* Saves the key, if given, and logs in, if a user and password are given
 * (bomba_setup_busy until it's done; bomba_setup_error if it failed). */
void bomba_setup_submit(const char *key, const char *user,
      const char *password);
bool bomba_setup_busy(void);

/* Why the last login failed, "" if it didn't. */
const char *bomba_setup_error(void);

/* Opens the page in the browser. */
void bomba_setup_open_page(enum bomba_setup_page page);

/* "" when not set. */
const char *bomba_setup_steamgriddb_key(void);

RETRO_END_DECLS

#endif
