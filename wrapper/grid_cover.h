#ifndef __WRAPPER_GRID_COVER_H
#define __WRAPPER_GRID_COVER_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

#include "grid_meta.h"

RETRO_BEGIN_DECLS

/* Portrait cover (600x900) from SteamGridDB for a game title, queueing a
 * lookup if it isn't cached. On GRID_META_FOUND, path is the cached image.
 * GRID_META_UNKNOWN when there is no cover or no API key. */
enum grid_meta_state grid_cover_get(const char *title,
      char *path, size_t path_len);

/* Runs queued lookups. Returns true when any entry changed. */
bool grid_cover_poll(void);

RETRO_END_DECLS

#endif
