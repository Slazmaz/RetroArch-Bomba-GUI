#ifndef __BOMBA_SETTINGS_H
#define __BOMBA_SETTINGS_H

#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* The install's own settings file, for people to edit in Notepad:
 *    <install>/bomba-settings.ini   (retroarch.exe is in <install>/app)
 * "key = value" lines, '#' comments. Ships with the build (its template is
 * bomba/bomba-settings.ini); install.sh never replaces one already there.
 * Holds:
 *    steamgriddb_api_key        covers and banners (grid_cover.c)
 *    retroachievements_username, retroachievements_password
 *       a password logs in once: the token goes to RetroArch's settings
 *       (cheevos_username / cheevos_token) and the password is erased */

/* Reads the file and logs in if asked. Once. */
void bomba_settings_init(void);

/* "" when not set. */
const char *bomba_settings_steamgriddb_key(void);

RETRO_END_DECLS

#endif
