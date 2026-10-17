#ifndef __BOMBA_GRID_SWITCH_ADDONS_H
#define __BOMBA_GRID_SWITCH_ADDONS_H

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* One Switch game's mods, or its updates and DLC, kept where Eden reads
 * them (<system>/Eden/user):
 *    load/<title ID>/<mod name>/exefs|romfs|cheats   mods
 *    nand/user/Contents/registered/<id>.nca          updates and DLC
 * The game's title ID is read from its file (.nsp / .xci; the NCA header,
 * decrypted with prod.keys' header_key). */

enum grid_addon_kind
{
   GRID_ADDON_MODS = 0,
   GRID_ADDON_UPDATES      /* updates and DLC */
};

enum grid_addons_state
{
   GRID_ADDONS_READY = 0,
   GRID_ADDONS_WORKING,    /* something being added */
   GRID_ADDONS_FAILED,     /* the last add or remove failed (see detail) */
   GRID_ADDONS_NO_GAME     /* the game's title ID couldn't be read */
};

typedef struct
{
   char name[256];
   bool on;                /* mods: not turned off */
   bool dlc;               /* updates: DLC, not an update */
} grid_addon_t;

/* Opens the list of kind for the game at rom. */
void grid_addons_open(const char *rom, enum grid_addon_kind kind);
void grid_addons_close(void);

enum grid_addons_state grid_addons_state(char *detail, size_t len);
size_t grid_addons_count(void);
const grid_addon_t *grid_addons_get(size_t i);

/* Asks for a mod (.zip, or a file in its folder) or an update / DLC
 * (.nsp) with the system's Open dialog and adds it. running_core: the
 * running game's core, or NULL (nothing changes while Eden runs). */
bool grid_addons_browse(const char *running_core);

/* Turns mod i on or off (Eden's disabled add-ons, both its configs). */
bool grid_addons_toggle(size_t i, const char *running_core);

/* Removes entry i. */
bool grid_addons_remove(size_t i, const char *running_core);

/* Per-frame work. Returns true when the list changed. */
bool grid_addons_poll(void);

RETRO_END_DECLS

#endif
