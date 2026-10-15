#ifndef __BOMBA_GRID_PROFILE_H
#define __BOMBA_GRID_PROFILE_H

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define GRID_PROFILE_MAX      8
#define GRID_PROFILE_NAME_LEN 32
#define GRID_PROFILE_DEFAULT  "Player 1"

/* Loads the profile list and points RetroArch's save and savestate
 * directories at the current profile. Safe to call repeatedly. */
void grid_profile_init(void);

unsigned grid_profile_count(void);
const char *grid_profile_name(unsigned idx);
unsigned grid_profile_current(void);

/* Colour (RGBA) for a profile's avatar circle. */
uint32_t grid_profile_color(unsigned idx);

/* Picture for a profile (an image path), or "" for its initial on its
 * colour. */
const char *grid_profile_icon(unsigned idx);
void grid_profile_set_icon(unsigned idx, const char *path);

/* Switches to a profile. Fails (returns false) while a game is running,
 * since its saves are already open in the old profile's folders. */
bool grid_profile_select(unsigned idx);

/* Adds a profile and switches to it. Returns false if the name is empty
 * or taken, the list is full, or a game is running. */
bool grid_profile_add(const char *name);

/* Renames a profile (moving its save folders to the new name). Fails if
 * the name is empty or taken, the folders can't be moved, or it is the
 * current profile while a game is running. */
bool grid_profile_rename(unsigned idx, const char *name);

/* Removes a profile from the list (its save folders are kept on disk).
 * The first profile and the current one can't be removed. */
bool grid_profile_remove(unsigned idx);

RETRO_END_DECLS

#endif
