#ifndef __BOMBA_GRID_META_H
#define __BOMBA_GRID_META_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

enum grid_meta_state
{
   GRID_META_PENDING = 0, /* lookup queued or in flight */
   GRID_META_FOUND,       /* title/icon available */
   GRID_META_UNKNOWN      /* not on RetroAchievements, or lookups disabled */
};

/* Looks up RetroAchievements metadata for a ROM, queueing a lookup if
 * it isn't cached yet. On GRID_META_FOUND, title and icon_path are
 * filled (icon_path may be empty while the icon downloads). */
enum grid_meta_state grid_meta_get(const char *rom_path,
      char *title, size_t title_len,
      char *icon_path, size_t icon_path_len);

/* Runs queued lookups. Returns true when any entry changed since the
 * last call, so the caller can refresh what it shows. */
bool grid_meta_poll(void);

/* RetroAchievements game id of a ROM already looked up, or 0. */
unsigned grid_meta_game_id(const char *rom_path);

RETRO_END_DECLS

#endif
