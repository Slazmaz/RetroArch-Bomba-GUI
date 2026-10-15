#ifndef __BOMBA_GRID_CORES_H
#define __BOMBA_GRID_CORES_H

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

enum grid_core_state
{
   GRID_CORE_NONE = 0,   /* nothing pending for this file */
   GRID_CORE_INSTALLING, /* its core or system files are being fetched */
   GRID_CORE_FAILED      /* its core or system files couldn't be installed */
};

/* Called for a game file that no installed core supports. If its
 * extension clearly belongs to one system, the matching core is queued
 * for automatic download (once per session), followed by the core's
 * system files if it needs any. */
void grid_cores_request(const char *rom_path);

/* Called for an installed core a game will run with. If the core needs
 * system files from the buildbot (Dolphin's Sys folder, ...) and they
 * aren't in the system directory, they are queued for download (once
 * per session). */
void grid_cores_request_files(const char *core_path);

/* Install state of what a file needs. core_path is the core it runs
 * with, or NULL/"" if none is installed (then its extension decides). */
enum grid_core_state grid_cores_state(const char *rom_path,
      const char *core_path);

/* Download progress (0-100) of what a file needs, or -1 if not
 * downloading or unknown yet. core_path as for grid_cores_state. */
int grid_cores_progress(const char *rom_path, const char *core_path);

/* Runs queued downloads. Returns true when a core or its system files
 * finished installing or failed, so the caller can rescan its game list. */
bool grid_cores_poll(void);

RETRO_END_DECLS

#endif
