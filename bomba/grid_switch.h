#ifndef __BOMBA_GRID_SWITCH_H
#define __BOMBA_GRID_SWITCH_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* The player's Switch keys and firmware, put into Eden (<system>/Eden/user)
 * either picked on the Downloads screen or dropped into RetroArch's system
 * folder:
 *    <system>/switch/prod.keys    (and title.keys)
 *    <system>/switch/firmware.zip (or a firmware/ folder of .nca files)
 * which are installed whenever they change. */

enum grid_switch_state
{
   GRID_SWITCH_MISSING = 0,
   GRID_SWITCH_INSTALLED,
   GRID_SWITCH_WORKING,
   GRID_SWITCH_FAILED
};

/* Whether Eden is installed (keys and firmware go into it). */
bool grid_switch_available(void);

/* How the keys / firmware stand, with a line for the card. */
enum grid_switch_state grid_switch_keys(char *detail, size_t len);
enum grid_switch_state grid_switch_firmware(char *detail, size_t len);

/* Installs prod.keys (path; title.keys next to it comes too). */
bool grid_switch_install_keys(const char *path);

/* Installs firmware from a zip or a folder of .nca files, replacing what
 * Eden has. running_core: the running game's core, or NULL. */
bool grid_switch_install_firmware(const char *path, const char *running_core);

/* Installs what is in <system>/switch if it changed since last time. */
void grid_switch_import_system(const char *running_core);

/* Per-frame work. Returns true when keys or firmware changed. */
bool grid_switch_poll(void);

/* Deletes a folder and everything in it. Links (junctions) in it are
 * removed as links, never followed. */
bool grid_switch_rmtree(const char *path);

/* Asks for the keys (kind 1) or firmware (2) with the system's Open
 * dialog and installs what was chosen. Returns false if cancelled or it
 * couldn't. */
bool grid_switch_browse(int kind, const char *running_core);

RETRO_END_DECLS

#endif
