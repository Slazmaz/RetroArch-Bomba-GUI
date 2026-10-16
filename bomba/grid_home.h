#ifndef __BOMBA_GRID_HOME_H
#define __BOMBA_GRID_HOME_H

#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

#include "../gfx/video_driver.h"
#include "../gfx/gfx_display.h"
#include "../menu/menu_defines.h"
#include "../input/input_types.h"

RETRO_BEGIN_DECLS

/* Draws the grid if it should be shown. Returns true if it drew,
 * in which case the caller skips its own frame. */
bool grid_home_frame(video_frame_info_t *video_info);

/* Per-frame work that must run on the main thread (not in a video frame,
 * which may be on the video thread), e.g. quitting. */
void grid_home_iterate(void);

/* Handles a menu action. Returns true if consumed. */
bool grid_home_entry_action(enum menu_action action, unsigned xmb_depth);

/* Whether OK should act as it goes down: on the grid's own screens, which
 * don't use RetroArch's list (and Exit is held). RetroArch acts on its
 * release for some list entries (Resume, save states...); the list
 * hidden under the grid while a game runs may sit on one. */
bool grid_home_ok_on_press(void);

/* Mouse and keyboard mode: the mouse or keyboard was used last, not a
 * controller. The pointer shows and the on-screen keyboard doesn't. */
bool grid_home_kbm(void);

/* The menu's mouse this frame: position in screen pixels and its
 * MENU_INP_PTR_FLG_* flags. Returns true if the grid took it, in which
 * case RetroArch treats the mouse as idle. */
bool grid_home_mouse(int x, int y, unsigned flags);

/* The controller while typing (buttons held, and newly pressed). Returns
 * true if the grid's on-screen keyboard took it. */
bool grid_home_osk_input(const input_bits_t *held,
      const input_bits_t *pressed);

/* A keyboard event, as input_keyboard_event gets it. Returns true if the
 * grid took it (an editing key in a text field). */
bool grid_home_key_event(bool down, unsigned code, uint32_t character,
      uint16_t mod);

/* The console a game file belongs to, as the grid names it ("SNES",
 * "Wii", "Switch"...). False if the file type doesn't tell (zip, an iso
 * of another system...). Saves are kept in a folder per console. */
bool grid_home_console_for_file(const char *path, char *s, size_t len);

/* Call when the menu opens or closes. */
void grid_home_toggle(bool menu_on);

/* Each frame while the menu is closed (gfx_widgets_frame): the grid
 * fading out over the game for a moment after the menu closed. */
void grid_home_closing_frame(void *video_info);

/* RetroArch's menu message box (help, info) for this frame. Returns true
 * if the grid shows it, in which case the caller doesn't. */
bool grid_home_messagebox(const char *message);

/* Whether RetroArch's Settings list should leave out this page (an
 * msg_hash_enums value): the Settings screen's "More" category skips the
 * pages that have their own category. */
bool grid_home_settings_hide(int enum_idx);

/* Builds the Settings screen's short list of everyday settings into list
 * (a file_list_t) when it shows one: for RetroArch's Settings root list,
 * or its Main Menu (main_menu). Returns how many entries it added, or -1
 * if RetroArch should build the list as usual. */
int grid_home_settings_build(void *list, bool main_menu);

/* Same for RetroArch's hotkey list: the Settings screen's short list of
 * shortcuts (controls only) outside advanced mode, or -1. */
int grid_home_hotkeys_build(void *list);

/* Same for a player's controls page (group: its settings group name,
 * user: 1-based): outside advanced mode, its options plus a "Buttons"
 * entry, or (opened from that entry) just the bindings. */
int grid_home_user_binds_build(void *list, const char *group, unsigned user);

void grid_home_context_reset(bool is_threaded);
void grid_home_context_destroy(void);

/* Menu-styled drawing for in-game overlays. Call from a video frame.
 * Sizes are pixels; cap is a text cap height in the grid's reference
 * pixels (screen height / 423), cy the line's vertical centre. Text is
 * batched: call grid_overlay_flush to draw it. */
void grid_overlay_shape(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h, float r, uint32_t rgba);
void grid_overlay_text(bool bold, const char *text, float x, float cy,
      unsigned width, unsigned height, uint32_t color,
      enum text_alignment align, float cap);
float grid_overlay_text_width(bool bold, const char *text,
      unsigned height, float cap);
void grid_overlay_flush(unsigned width, unsigned height);

RETRO_END_DECLS

#endif
