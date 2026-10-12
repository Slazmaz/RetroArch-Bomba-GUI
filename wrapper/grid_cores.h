#ifndef __WRAPPER_GRID_CORES_H
#define __WRAPPER_GRID_CORES_H

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

enum grid_core_state
{
   GRID_CORE_NONE = 0,   /* nothing pending for this file */
   GRID_CORE_INSTALLING, /* its core is queued or downloading */
   GRID_CORE_FAILED      /* its core couldn't be installed */
};

/* Called for a game file that no installed core supports. If its
 * extension clearly belongs to one system, the matching core is queued
 * for automatic download (once per session). */
void grid_cores_request(const char *rom_path);

/* Install state of the core a file needs. */
enum grid_core_state grid_cores_state(const char *rom_path);

/* Download progress (0-100) of the core a file needs, or -1 if not
 * downloading or unknown yet. */
int grid_cores_progress(const char *rom_path);

/* Runs queued downloads. Returns true when a core finished installing or
 * failed, so the caller can rescan its game list. */
bool grid_cores_poll(void);

RETRO_END_DECLS

#endif
