#ifndef __BOMBA_GRID_UPDATES_H
#define __BOMBA_GRID_UPDATES_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Which part of the Downloads screen an item is in. Once updated or
 * installed it moves to Up to date. */
enum grid_update_section
{
   GRID_SECTION_UPDATES = 0, /* installed, a newer version is out */
   GRID_SECTION_INSTALL,     /* not installed */
   GRID_SECTION_INSTALLED,   /* installed and up to date */
   GRID_SECTION_COUNT
};

enum grid_update_state
{
   GRID_UPDATE_CURRENT = 0, /* nothing to do */
   GRID_UPDATE_AVAILABLE,   /* can be updated / installed */
   GRID_UPDATE_WORKING,     /* downloading / installing */
   GRID_UPDATE_DONE,        /* updated / installed this session */
   GRID_UPDATE_FAILED       /* check or install failed; can be retried */
};

typedef struct
{
   char name[128];          /* "Eden", a core's display name */
   char detail[128];        /* "v0.2.2 available", "Up to date", ... */
   char version[32];        /* what is (or would be) installed: Eden's tag,
                               "Steam build", or a build date */
   char console[96];        /* the system it plays, "" if unknown */
   char search[384];        /* name, console, maker: what a search matches */
   bool removable;          /* an installed core that can be removed */
   const char *kind;        /* "App" (Bomba GUI), "Emulator" or "Core" */
   enum grid_update_section section;
   enum grid_update_state state;
   int progress;            /* 0-100 while downloading, -1 if unknown */
   unsigned date;           /* release date as yyyymmdd, 0 if unknown */
} grid_update_t;

/* Looks for updates and things to install (Bomba GUI from its GitHub
 * releases, once per boot; Eden from its releases, cores from the
 * buildbot). Nothing is downloaded until asked for. Without
 * force, does nothing if a check ran recently. Also installs Switch keys
 * and firmware dropped into the system folder (see grid_switch.h). */
void grid_updates_check(bool force, const char *running_core);

/* Whether a check is still running. */
bool grid_updates_checking(void);

/* Whether the buildbot's core list couldn't be fetched. */
bool grid_updates_cores_failed(void);

/* Per-frame work. Returns true when something finished installing or
 * updating (core info and the games list may need reloading). */
bool grid_updates_poll(void);

size_t grid_updates_count(void);
const grid_update_t *grid_updates_get(size_t idx);

/* Whether item idx is set up by picking a file: 1 Switch keys, 2 Switch
 * firmware (see grid_switch.h), 0 not. */
int grid_updates_pick(size_t idx);

/* How many installed items have an update waiting. */
unsigned grid_updates_available(void);

/* Updates or installs item idx (or tries again after a failure); or
 * every waiting update. running_core: the running game's core, or NULL;
 * what it uses can't be replaced while it runs. */
void grid_updates_install(size_t idx, const char *running_core);
void grid_updates_install_all(const char *running_core);

/* Removes an installed core (item idx, if removable). Returns false if
 * it couldn't (the running game uses it, or the file is in use). */
bool grid_updates_remove(size_t idx, const char *running_core);

RETRO_END_DECLS

#endif
