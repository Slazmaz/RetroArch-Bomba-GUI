#ifndef __BOMBA_GRID_CONTROLS_H
#define __BOMBA_GRID_CONTROLS_H

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Per-console settings kept by the grid: which emulator (core) plays the
 * console's games, and its bindings: for each of the console's controls
 * (RetroArch's RetroPad ids 0-15), a gamepad button (or stick direction)
 * and a keyboard key of its own instead of the RetroPad setup's. */
#define GRID_CONTROLS_BUTTONS 16

/* Control ids in the order they're listed (face buttons first). */
extern const unsigned grid_controls_order[GRID_CONTROLS_BUTTONS];

/* Generic names ("Bottom button", "D-pad up", ...), for when the
 * emulator's own aren't known. */
const char *grid_controls_button_name(unsigned id);

/* A console's own binding for a control: *joykey / *joyaxis (NO_BTN /
 * AXIS_NONE: the RetroPad setup's) and *key (0: the RetroPad setup's). */
void grid_controls_get_bind(const char *console, unsigned id,
      uint16_t *joykey, uint32_t *joyaxis, unsigned *key);
/* Sets the gamepad side (a button, or a stick direction) or the key. */
void grid_controls_set_pad(const char *console, unsigned id,
      uint16_t joykey, uint32_t joyaxis);
void grid_controls_set_key(const char *console, unsigned id, unsigned key);
/* Back to the RetroPad setup: one control, or all of them. */
void grid_controls_clear(const char *console, unsigned id);
void grid_controls_reset(const char *console);
bool grid_controls_changed(const char *console);

/* The emulator chosen for a console: its core file's name (e.g.
 * "snes9x_libretro.dll"), or "" for the first one that fits. */
const char *grid_controls_core(const char *console);
void grid_controls_set_core(const char *console, const char *core_file);

/* Before a game starts: writes its console's bindings as RetroArch's
 * override for the game (or removes ours when there are none). An
 * override the player made in RetroArch is left alone. */
void grid_controls_apply(const char *console, const char *core_path,
      const char *content_path);

/* A console's bindings changed while its game runs: use them now, if
 * RetroArch will put the usual ones back when the game closes (an
 * override is in use); otherwise from its next start. */
void grid_controls_apply_running(const char *console);

RETRO_END_DECLS

#endif
