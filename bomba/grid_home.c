/* Bomba: console-style home screen (dark theme).
 *
 * Replaces XMB's main menu and Quick Menu with one screen:
 *
 *   16:24 | (P) | (F) (S) (search)              [L] All 42 | [R] Recent
 *   +-----+ +-----+ +-----+ +-----+ +-----+ +-----+ +--
 *   |cover| |cover| |cover| |cover| |cover| |cover| |    1-4 rows (Zoom) of
 *   +-----+ +-----+ +-----+ +-----+ +-----+ +-----+ +--  covers, scrolling
 *   +-----+ +-----+ +-----+ +-----+ +-----+ +-----+ +--  sideways; filled
 *   |cover| |cover| |cover| |cover| |cover| |cover| |    column by column
 *   +-----+ +-----+ +-----+ +-----+ +-----+ +-----+ +--
 *   Game title | 2h 15m              (Y) Zoom (X) Options (A) Play (B) Back
 *
 * Games are the files in the File Browser start directory (settings:
 * rgui_browser_directory) that an installed core supports. Titles and
 * icons come from RetroAchievements (grid_meta.c), covers from SteamGridDB
 * (grid_cover.c), falling back to libretro boxart and the file name.
 *
 * Controls: D-pad moves (up from the top row reaches the icon bar), L
 * cycles the console shown (newest console first), R cycles the sort
 * order, A plays/continues, B resumes the running game (or clears the
 * search), Y zooms (1-4 rows), X opens the selected game's menu (quick
 * settings and quit while it runs, achievements, cover art, wallpaper).
 * Buttons are named as on the player's pad: the menu's "search" is Y,
 * its "scan" X. Select and Start do nothing in the grid's screens.
 * Settings is the cog on the icon bar: RetroArch's own menu lists (every
 * setting) drawn in this screen's style, in categories (Graphics, Audio,
 * Controls, ..., System) along the top that L / R switch. Each shows a
 * short list of everyday settings; Y (advanced mode) shows RetroArch's
 * full pages instead, plus Miscellaneous. Back at the top returns here.
 * XMB itself is never drawn.
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <limits.h>
#include <time.h>

#include <features/features_cpu.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <formats/image.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "grid_home.h"
#include "grid_meta.h"
#include "grid_cover.h"
#include "grid_cores.h"
#include "grid_profile.h"
#include "bomba_steamnet.h"
#include "bomba_setup.h"
#include "bomba_clipboard.h"
#include "bomba_keymap.h"
#include "grid_updates.h"
#include "bomba_version.h"
#include "grid_switch.h"
#include "grid_cheevos.h"
#include "grid_controls.h"
#include "grid_coreopts.h"

#include <formats/rpng.h>
#include <rthreads/rthreads.h>

#include "../configuration.h"
#include "../core_info.h"
#include "../defaults.h"
#include "../playlist.h"
#include "../runtime_file.h"
#include "../file_path_special.h"
#include "../runloop.h"
#include "../content.h"
#include "../tasks/task_content.h"
#include "../command.h"
#include "../paths.h"
#include "../input/input_driver.h"
#include "../input/input_keymaps.h"
#include "../audio/audio_driver.h"
#include "../menu/menu_driver.h"
#include "../menu/menu_cbs.h"
#include "../menu/menu_displaylist.h"
#include "../menu/menu_setting.h"
#include "../menu/menu_input.h"
#include "../gfx/gfx_display.h"
#include "../gfx/video_driver.h"
#include "../gfx/font_driver.h"

#define GRID_MAX_GAMES 1024
#define GRID_MAX_TABS  24
#define GRID_ROWS_DEFAULT 2 /* rows per column; Select (Zoom) cycles 1-4 */
#define GRID_ZOOM_USEC 250000 /* zoom animation length */
#define GRID_OPEN_USEC  260000 /* the menu fading in over a game */
#define GRID_CLOSE_USEC 200000 /* ...and out when it closes */
#define GRID_LOADING_USEC 500000 /* the loading screen fading in, and out */
#define GRID_OSK_OPEN_USEC  220000 /* on-screen keyboard sliding in */
#define GRID_OSK_CLOSE_USEC 150000 /* ...and out */
/* The layout is measured from a 752x423 reference screen. REF(px)
 * converts a reference pixel length to this screen; CAP(px) gives the
 * font scale whose capital letters are px reference pixels tall
 * (scale 1.0 = height/32 em). */
#define GRID_FONT_CAP  0.60f /* Baloo 2 cap height, in em */
#define REF(px)        ((float)(px) * (float)height / 423.0f)
#define CAP(px)        ((float)(px) * 32.0f / (423.0f * GRID_FONT_CAP))
#define GRID_FONT_CACHE 24
/* Cap height (reference px) of all text on the main screen: bars, hints,
 * search, L/R labels, game title. */
#define GRID_TEXT_CAP  9
#define GRID_COVER_RADIUS 0.04f /* corner radius / tile width (reference) */
#define GRID_TILE_ASPECT  (2.0f / 3.0f) /* tile width / height (600x900) */

/* Colours (RGBA), dark theme */
#define COL_BG          0x0e0e0eff /* background, bottom */
#define COL_BG_TOP      0x2a2a2dff /* background, top */
#define COL_SEARCH      0x3a3a3dff
#define COL_TILE        0x232325ff /* tile placeholder */
#define COL_TEXT        0xecececff
#define COL_TEXT_DIM    0x8c8c90ff
#define COL_TEXT_SOFT   0xb4b4b9ff /* secondary text on the list screens */
#define COL_TEXT_DARK   0x181818ff /* on light glyphs */
#define COL_GLYPH       0xe6e6e6ff /* L/R, button glyphs, icon buttons: light,
                                      with dark letters/icons */
#define COL_DIVIDER     0xffffff30 /* "|" dividers and top-left button
                                      backgrounds: lightly brighten what is
                                      behind */
#define COL_CURSOR      0x00c3e3ff /* download progress */
#define COL_SELECT      0x0099ffff /* selection outline (vivid blue), pulsing to... */
#define COL_SELECT_HI   0x40b8ffff /* ...this */
#define COL_LIME        0x7ed321ff /* running-game badge */
#define COL_CHEEVO      0x7ce400ff /* achievement progress (lime) */
#define COL_DANGER      0xff5c52ff /* Quit game: its label, white when... */
#define COL_DANGER_BG   0xd93a30ff /* ...its row is selected, on this */

typedef struct
{
   char path[PATH_MAX_LENGTH];
   char core_path[PATH_MAX_LENGTH];
   char title[NAME_MAX_LENGTH];
   char system[40];                   /* console filter label, e.g. "SNES" */
   char boxart_path[PATH_MAX_LENGTH]; /* libretro-thumbnails fallback */
   char art_path[PATH_MAX_LENGTH];    /* what is shown */
   uintptr_t art;
   unsigned art_w;
   unsigned art_h;
   unsigned recent;                   /* position in play history; UINT_MAX: never */
   unsigned played;                   /* total time played, seconds */
   bool art_cover;                    /* art is a wide cover (fills the tile) */
   bool bg_ready;                     /* its hero's blur is cached on disk */
   bool art_tried;
   bool pending;                      /* its core is still downloading */
} grid_game_t;

/* A game's loading screen (see grid_launch). */
enum grid_loading
{
   LOADING_NONE = 0,
   LOADING_IN,     /* fading in over the grid */
   LOADING_START,  /* fully shown: the game starts next */
   LOADING_OUT     /* the game started: fading out over it */
};

enum grid_tab_type
{
   TAB_ALL = 0,
   TAB_SYSTEM
};

/* Console filter (cycled with L). */
typedef struct
{
   enum grid_tab_type type;
   char label[40];
} grid_tab_t;

/* Monochrome icon: its shape (alpha) kept at full size, and a texture
 * rasterised from it at the exact pixel size it is drawn at (scaling a
 * big texture down on the GPU makes it soft). */
typedef struct
{
   uint8_t *alpha;
   unsigned w, h;
   float off_x, off_y; /* visible shape's centre minus the image's centre,
                          as a fraction of the image size */
   uintptr_t tex;
   unsigned px, ph; /* size tex was made for; 0: none */
   /* Round button (circle with the icon in it) as one texture. */
   uintptr_t btn_tex;
   unsigned btn_d, btn_ip; /* circle and icon size it was made for */
   float btn_nx, btn_ny, btn_nr; /* ...and notch (see grid_icon_button_draw_notch) */
} grid_icon_t;

/* Blurred background image. */
typedef struct
{
   uintptr_t tex;
   unsigned w;
   unsigned h;
} grid_bg_t;

#define GRID_BG_GAME   "\x01game" /* background key: the paused game */
#define GRID_BG_DIM      0.5f     /* background brightness factor, at most */
#define GRID_BG_MAX_LUMA 56.0f    /* brightest a background pixel gets (0-255) */
#define GRID_BG_SATURATION 0.75f  /* colour kept (0 grey .. 1 full colour) */
#define GRID_BG_WIDTH    240      /* blur working width: smaller = blurrier */
#define GRID_BG_FADE     100000   /* usec cross-fade between backgrounds */
/* Cached blurred background suffix; bump it when the look changes so old
 * caches are rebuilt. (Never reuse an old number: .bg2 to .bg9 were each
 * written by some earlier build, some at full size.) */
#define GRID_BG_CACHE_EXT ".bg10.png"

/* Sort order (cycled with R). */
enum grid_sort
{
   SORT_RECENT = 0,
   SORT_TITLE,
   SORT_TIME_PLAYED,
   SORT_COUNT
};

static const char *grid_sort_label[SORT_COUNT] = {
   "Recent",
   "Alphabetical",
   "Time Played"
};

/* Top-left icon bar, in order. */
enum grid_bar_item
{
   BAR_PROFILE = 0,
   BAR_SETTINGS,
   BAR_UPDATES,
   BAR_SEARCH,
   BAR_COUNT
};

/* Pop-up panels over the grid. */
enum grid_panel
{
   PANEL_NONE = 0,
   PANEL_PROFILES,
   PANEL_ICONS,    /* picture picker for a profile */
   PANEL_GAME_MENU,/* dropdown for the selected game (+) */
   PANEL_CHEEVOS,  /* the game's achievements */
   PANEL_ART,      /* choosing the game's cover art or wallpaper */
   PANEL_UPDATES,  /* Installs: updates and things to install */
   PANEL_SETUP     /* first start: SteamGridDB key, RetroAchievements login */
};

/* Game menu entries, in order. Settings are its console's (its emulator's
 * settings, bindings, which emulator); Quit game shows only for the
 * running game, last and in red, set apart from the rest. */
enum grid_game_menu_item
{
   GAME_MENU_SETTINGS = 0,
   GAME_MENU_CHEEVOS,
   GAME_MENU_COVER,
   GAME_MENU_WALLPAPER,
   GAME_MENU_QUIT,
   GAME_MENU_COUNT
};

static const char *grid_game_menu_label[GAME_MENU_COUNT] = {
   "Settings",
   "Achievements",
   "Change cover art",
   "Change wallpaper",
   "Quit game"
};

/* Everyday settings shown outside advanced mode: a short flat list per
 * category, in our own words (RetroArch's names are technical). label NULL
 * keeps RetroArch's. MENU_ENUM_LABEL_INPUT_USER_1_BINDS stands for the
 * first players' controls; MSG_UNKNOWN with a label is the grid's own
 * Consoles page for the category (GRID_CC_LABEL). Lists end with
 * MSG_UNKNOWN and no label. */
#define GRID_CC_LABEL "bomba_consoles"
typedef struct
{
   enum msg_hash_enums entry;
   const char *label;
} grid_simple_setting_t;

/* Players' controls listed: just the first, as "Controller" (one place
 * to set the controller up; the others are advanced). */
#define GRID_SIMPLE_PLAYERS 1

static const grid_simple_setting_t grid_simple_graphics[] = {
   { MSG_UNKNOWN,                              "Consoles" },
   { MENU_ENUM_LABEL_VIDEO_DISPLAY_MODE,       "Display mode" },
   { MENU_ENUM_LABEL_VIDEO_ASPECT_RATIO_INDEX, "Aspect ratio" },
   { MENU_ENUM_LABEL_VIDEO_SCALE_INTEGER,      "Pixel-perfect scaling" },
   { MENU_ENUM_LABEL_VIDEO_SMOOTH,             "Smooth pixels" },
   { MENU_ENUM_LABEL_VIDEO_VSYNC,              "Prevent screen tearing (VSync)" },
   { MENU_ENUM_LABEL_FPS_SHOW,                 "Show frame rate" },
   { MSG_UNKNOWN, NULL }
};

static const grid_simple_setting_t grid_simple_audio[] = {
   { MSG_UNKNOWN,                       "Consoles" },
   { MENU_ENUM_LABEL_AUDIO_ENABLE,      "Sound" },
   { MENU_ENUM_LABEL_AUDIO_VOLUME,      "Volume" },
   { MENU_ENUM_LABEL_AUDIO_MUTE,        "Mute" },
   { MENU_ENUM_LABEL_AUDIO_ENABLE_MENU, "Menu sounds" },
   { MSG_UNKNOWN, NULL }
};

static const grid_simple_setting_t grid_simple_controls[] = {
   { MSG_UNKNOWN,                                         "Consoles" },
   { MENU_ENUM_LABEL_INPUT_USER_1_BINDS,                  NULL },
   /* The menu button is set in Shortcuts only (its controller-combo
    * alternative is advanced). */
   { MENU_ENUM_LABEL_INPUT_HOTKEY_BINDS,         "Shortcuts" },
   { MENU_ENUM_LABEL_MENU_INPUT_SWAP_OK_CANCEL,  "Swap confirm and back" },
   { MENU_ENUM_LABEL_ENABLE_DEVICE_VIBRATION,    "Vibration" },
   { MSG_UNKNOWN, NULL }
};

static const grid_simple_setting_t grid_simple_saving[] = {
   { MENU_ENUM_LABEL_SAVESTATE_AUTO_SAVE,        "Save state when quitting a game" },
   { MENU_ENUM_LABEL_SAVESTATE_AUTO_LOAD,        "Continue from it next time" },
   { MENU_ENUM_LABEL_AUTOSAVE_INTERVAL,          "Save game progress every" },
   { MENU_ENUM_LABEL_SAVESTATE_THUMBNAIL_ENABLE, "Screenshots for save states" },
   { MENU_ENUM_LABEL_CONFIG_SAVE_ON_EXIT,        "Remember settings" },
   { MSG_UNKNOWN, NULL }
};

#ifdef HAVE_CHEEVOS
static const grid_simple_setting_t grid_simple_cheevos[] = {
   { MENU_ENUM_LABEL_CHEEVOS_ENABLE,               "Achievements" },
   { MENU_ENUM_LABEL_CHEEVOS_USERNAME,             "Username" },
   { MENU_ENUM_LABEL_CHEEVOS_PASSWORD,             "Password" },
   { MENU_ENUM_LABEL_CHEEVOS_HARDCORE_MODE_ENABLE, "Hardcore mode" },
   { MENU_ENUM_LABEL_CHEEVOS_UNLOCK_SOUND_ENABLE,  "Unlock sound" },
   { MSG_UNKNOWN, NULL }
};
#endif

static const grid_simple_setting_t grid_simple_interface[] = {
   { MENU_ENUM_LABEL_USER_LANGUAGE,     "Language" },
   { MENU_ENUM_LABEL_PAUSE_LIBRETRO,    "Pause the game in the menu" },
   { MENU_ENUM_LABEL_PAUSE_NONACTIVE,   "Pause when in the background" },
   { MENU_ENUM_LABEL_VIDEO_FONT_ENABLE, "On-screen messages" },
   { MSG_UNKNOWN, NULL }
};

/* Shortcuts outside advanced mode: just these controls, no options (by
 * hotkey id, see input_defines.h). Ends with RARCH_BIND_LIST_END. */
static const struct
{
   unsigned id;
   const char *label;
} grid_simple_hotkeys[] = {
   { RARCH_MENU_TOGGLE,           "Open the menu" },
   { RARCH_ENABLE_HOTKEY,         "Hold to use shortcuts" },
   { RARCH_SAVE_STATE_KEY,        "Save state" },
   { RARCH_LOAD_STATE_KEY,        "Load state" },
   { RARCH_STATE_SLOT_PLUS,       "Next save slot" },
   { RARCH_STATE_SLOT_MINUS,      "Previous save slot" },
   { RARCH_FAST_FORWARD_HOLD_KEY, "Fast forward (hold)" },
   { RARCH_PAUSE_TOGGLE,          "Pause" },
   { RARCH_RESET,                 "Restart the game" },
   { RARCH_SCREENSHOT,            "Screenshot" },
   { RARCH_FULLSCREEN_TOGGLE_KEY, "Fullscreen" },
   { RARCH_BIND_LIST_END,         NULL }
};

static const grid_simple_setting_t grid_simple_system[] = {
   { MSG_UNKNOWN,                       "Consoles" },
   { MENU_ENUM_LABEL_ONLINE_UPDATER,    "Updates and downloads" },
   { MENU_ENUM_LABEL_INFORMATION_LIST,  "Information" },
   { MENU_ENUM_LABEL_RESTART_RETROARCH, "Restart RetroArch" },
   { MSG_UNKNOWN, NULL }
};

/* Settings screen categories, along the top and switched with L / R.
 * Outside advanced mode a category is its short list (simple), built in
 * place of its root list. In advanced mode it is one of RetroArch's
 * settings pages, opened from a root list (XMB's Settings or Main Menu
 * tab), or that root list itself (entry MSG_UNKNOWN). Miscellaneous
 * (advanced only) is the Settings root without the pages that have their
 * own category (see grid_home_settings_hide). */
typedef struct
{
   const char *name;
   enum msg_hash_enums root;
   unsigned root_type;
   enum msg_hash_enums entry;
   const grid_simple_setting_t *simple;
   bool advanced;            /* shown only in advanced mode */
} grid_settings_cat_t;

static const grid_settings_cat_t grid_settings_cats[] = {
   { "Graphics",      MENU_ENUM_LABEL_SETTINGS_TAB, MENU_SETTINGS_TAB,
      MENU_ENUM_LABEL_VIDEO_SETTINGS,              grid_simple_graphics,  false },
   { "Audio",         MENU_ENUM_LABEL_SETTINGS_TAB, MENU_SETTINGS_TAB,
      MENU_ENUM_LABEL_AUDIO_SETTINGS,              grid_simple_audio,     false },
   { "Controls",      MENU_ENUM_LABEL_SETTINGS_TAB, MENU_SETTINGS_TAB,
      MENU_ENUM_LABEL_INPUT_SETTINGS,              grid_simple_controls,  false },
   { "Saving",        MENU_ENUM_LABEL_SETTINGS_TAB, MENU_SETTINGS_TAB,
      MENU_ENUM_LABEL_SAVING_SETTINGS,             grid_simple_saving,    false },
#ifdef HAVE_CHEEVOS
   { "Achievements",  MENU_ENUM_LABEL_SETTINGS_TAB, MENU_SETTINGS_TAB,
      MENU_ENUM_LABEL_RETRO_ACHIEVEMENTS_SETTINGS, grid_simple_cheevos,   false },
#endif
   { "Interface",     MENU_ENUM_LABEL_SETTINGS_TAB, MENU_SETTINGS_TAB,
      MENU_ENUM_LABEL_USER_INTERFACE_SETTINGS,     grid_simple_interface, false },
   { "System",       MENU_ENUM_LABEL_MAIN_MENU,    MENU_SETTINGS,
      MSG_UNKNOWN,                                 grid_simple_system,    false },
   { "Miscellaneous", MENU_ENUM_LABEL_SETTINGS_TAB, MENU_SETTINGS_TAB,
      MSG_UNKNOWN,                                 NULL,                  true  }
};

#define GRID_SETTINGS_CAT_COUNT \
   (sizeof(grid_settings_cats) / sizeof(grid_settings_cats[0]))

/* Profiles strip items: Exit, then the profiles (profile p is item
 * p + GRID_STRIP_FIRST), then Add. */
#define GRID_STRIP_EXIT  0
#define GRID_STRIP_FIRST 1

/* Exit quits once OK has been held on it this long (usec), a red ring
 * filling round the button meanwhile. */
#define GRID_EXIT_HOLD   1000000
/* ...and quits this long (usec) after the full ring is first drawn. */
#define GRID_EXIT_SHOW   150000

#define GRID_CHEEVO_COLS 2   /* achievements screen columns */
#define GRID_MAX_CHEEVOS 512

/* Achievements screen: L filters, R sorts. */
enum grid_cheevo_filter
{
   CHEEVO_FILTER_ALL = 0,
   CHEEVO_FILTER_UNLOCKED,
   CHEEVO_FILTER_LOCKED,
   CHEEVO_FILTER_COUNT
};

static const char *grid_cheevo_filter_label[CHEEVO_FILTER_COUNT] = {
   "All", "Unlocked", "Locked"
};

enum grid_cheevo_sort
{
   CHEEVO_SORT_DEFAULT = 0,  /* the set's own order */
   CHEEVO_SORT_POINTS,       /* most points first */
   CHEEVO_SORT_RAREST,       /* fewest players have it first */
   CHEEVO_SORT_COUNT
};

static const char *grid_cheevo_sort_label[CHEEVO_SORT_COUNT] = {
   "Default", "Most points", "Rarest"
};

/* Installs screen: L filters (everything, or one section: filter f
 * shows enum grid_update_section f - 1), R sorts, Y searches. */
enum grid_dl_filter
{
   DL_FILTER_ALL = 0,
   DL_FILTER_UPDATES,        /* GRID_SECTION_UPDATES */
   DL_FILTER_INSTALLABLE,    /* GRID_SECTION_INSTALL */
   DL_FILTER_UP_TO_DATE,     /* GRID_SECTION_INSTALLED */
   DL_FILTER_COUNT
};

static const char *grid_dl_filter_label[DL_FILTER_COUNT] = {
   "All", "Updates", "Installable", "Up to date"
};

enum grid_dl_sort
{
   DL_SORT_STATUS = 0,       /* updates, then up to date, then installable */
   DL_SORT_NAME,
   DL_SORT_NEWEST,           /* most recently built / released first */
   DL_SORT_COUNT
};

static const char *grid_dl_sort_label[DL_SORT_COUNT] = {
   "Status", "Name", "Newest"
};

#define GRID_DL_COLS 2       /* Installs screen columns */
#define GRID_DL_UPDATE_ALL ((size_t)-1) /* its "Update all" card */
/* Removing a core: X held on its card this long (usec), like Exit. */
#define GRID_DL_HOLD     1000000

/* What the on-screen keyboard is typing. */
enum grid_kb
{
   KB_SEARCH = 0,
   KB_PROFILE_NAME,
   KB_MENU,         /* RetroArch's own text entry, in the Settings screen */
   KB_DOWNLOADS,    /* the Installs screen's search */
   KB_SETUP         /* a field on the Setup screen */
};

static struct
{
   grid_game_t *games;
   size_t count;

   /* Current console filter, sort and search, as a list of indices into
    * games */
   grid_tab_t tabs[GRID_MAX_TABS];
   unsigned tab_count;
   char tab_label[40];      /* current console, kept across rebuilds */
   enum grid_sort sort;
   size_t *view;
   size_t view_count;
   size_t selection;        /* index into view */
   size_t first_col;        /* leftmost fully visible column */
   unsigned rows;           /* rows per column (Zoom): 1-4 */
   bool zoomed;             /* rows just changed: snap the scroll */
   unsigned zoom_from;      /* rows before the last zoom, 0 = none */
   float zoom_from_scroll;  /* scroll_x before the last zoom */
   retro_time_t zoom_start; /* when the last zoom began, usec */
   float scroll_x;          /* animated grid scroll, pixels */
   char search[128];        /* applied search text */
   float search_open;       /* search box expansion, 0 (icon) .. 1 (box) */
   float profile_open;      /* profiles strip expansion, 0 .. 1 */

   bool bar_focus;          /* D-pad is on the icon bar, not the grid */
   unsigned bar_sel;        /* selected icon */
   retro_time_t exit_hold_start; /* OK went down on Exit (0: not held) */
   float exit_blend;        /* Exit's cursor, 0 blue .. 1 hold ring */
   float exit_hold_t;       /* hold progress last drawn, 0 .. 1 */
   retro_time_t exit_full_at; /* the full hold ring was first drawn */
   enum grid_panel panel;   /* open pop-up */
   unsigned panel_sel;      /* selected item in it */
   enum grid_kb kb;         /* what the keyboard is for */
   unsigned rename_target;  /* profile being renamed */
   /* Setup screen: what it asks for (fixed while it's open), what's been
    * typed, and the field the keyboard is typing into. */
   bool setup_key, setup_ra;
   char setup_griddb[64];
   char setup_user[128];
   char setup_pass[256];
   unsigned setup_typing;

   /* Game menu: the game it is for (index into games), where its tile is
    * on screen (the dropdown opens beside it), and the screens it opens. */
   size_t menu_game;
   float sel_x, sel_y, sel_w, sel_h;
   /* Its drawer's slide in/out: whether it was open last frame, whether it
    * is closing, and when it began opening or closing (usec). */
   bool gm_open, gm_closing;
   retro_time_t gm_start;
   unsigned gm_sel;         /* its selected row, kept while it closes */
   /* When the menu opened over a game (it fades in) or closed back to it
    * (it fades out), 0 not fading. */
   retro_time_t open_start, close_start;
   /* The loading screen of the game being started (see grid_launch). */
   enum grid_loading loading;
   retro_time_t loading_start;   /* when it began fading in */
   grid_game_t loading_game;
   enum grid_choice_kind art_kind;
   size_t art_count;        /* previews the textures below were made for */
   unsigned art_cols;       /* previews per row, as last laid out */
   unsigned pick_cols;      /* pictures per row, as last laid out */
   bool art_sel_set;        /* selection moved to the current art yet */
   uintptr_t *art_tex;
   bool *art_tried;
   uintptr_t badge_tex[GRID_MAX_CHEEVOS];
   bool badge_tried[GRID_MAX_CHEEVOS];

   /* Achievements / Installs screens: their filter and sort, and the
    * resulting order (indices into their lists; panel_sel indexes it). */
   enum grid_cheevo_filter cheevo_filter;
   enum grid_cheevo_sort cheevo_sort;
   enum grid_dl_filter dl_filter;
   enum grid_dl_sort dl_sort;
   char dl_search[128];
   /* X held on an Installs card to remove it (as Exit's hold): the item,
    * when X went down (0: not held), the red ring's blend in and progress
    * last drawn, and when the full ring was first drawn. */
   size_t dl_hold_item;
   retro_time_t dl_hold_start;
   float dl_hold_blend;
   float dl_hold_t;
   retro_time_t dl_hold_full_at;
   size_t list_view[GRID_MAX_CHEEVOS];
   size_t list_count;

   /* Multiplayer guest: the game started for the host's
    * ("system\ttitle"), so it starts once. */
   char mp_started[320];

   /* Profile pictures (circle-clipped), reloaded when the path changes. */
   uintptr_t profile_tex[GRID_PROFILE_MAX];
   char profile_tex_path[GRID_PROFILE_MAX][PATH_MAX_LENGTH];

   /* Picture picker: which profile it is for, and the icons' textures
    * (item 0 is "no picture", item i the (i-1)th icon). */
   unsigned pick_target;
   size_t pick_count;       /* icons when the picker opened */
   uintptr_t *pick_tex;
   bool *pick_tried;

   /* Settings screen: RetroArch's menu lists drawn in the grid's style.
    * settings_quick: it shows the Quick Menu (opened from the game menu)
    * rather than the Settings / System sections. */
   bool settings;
   bool settings_quick;
   unsigned settings_cat;   /* category shown (grid_settings_cats) */
   bool advanced;           /* advanced mode: off at every start */
   bool forwarding;         /* passing an action on to RetroArch */
   /* A category's Consoles page (the grid's own): 0 closed, 1 the
    * consoles, 2 one console's settings of the category (cc_sub: an enum
    * grid_cc_bucket; cc_console: an index into grid_cc_consoles),
    * 3 its Bindings; cc_sel is the selected row, cc_back the console
    * page's row to come back to. */
   unsigned cc_level;
   int cc_sub;
   unsigned cc_back;
   /* A game's Settings (from its menu): its console's pages, Graphics /
    * Audio / Controls / System as tabs; game_path / game_title are the
    * game's. */
   bool game_settings;
   char game_path[PATH_MAX_LENGTH];
   char game_title[NAME_MAX_LENGTH];
   unsigned cc_console;
   unsigned cc_sel;
   size_t settings_base;    /* menu depth of the screen's top list */
   char box_msg[2048];      /* RetroArch's message box (help, info), this frame */
   bool scanned;
   char scanned_dir[PATH_MAX_LENGTH];

   /* Fonts rasterised at the exact pixel size they are drawn at
    * (scaling one big font down blurs and chops the glyphs). */
   struct
   {
      font_data_t *font;
      unsigned px;
      bool bold;
   } fonts[GRID_FONT_CACHE];
   unsigned font_count;
   unsigned font_screen_h;  /* screen height the cache was built for */
   char font_path[2][PATH_MAX_LENGTH]; /* [0] regular, [1] bold */
   bool font_threaded;
   uintptr_t disc;          /* white anti-aliased circle, for rounded shapes */
   /* Top-left bar icons (from bomba/icons). */
   grid_icon_t icon_exit;
   grid_icon_t icon_settings;
   grid_icon_t icon_search;
   grid_icon_t icon_trophy;  /* achievement progress, in the bottom bar */
   grid_icon_t icon_download;/* Installs, in the icon bar */
   grid_icon_t icon_backspace; /* on-screen keyboard keys */
   grid_icon_t icon_shift;
   /* Background of the selected game. */
   grid_bg_t bg;
   char bg_key[PATH_MAX_LENGTH];   /* what bg shows: "", GRID_BG_GAME or a
                                      hero image path */
   char bg_prev[PATH_MAX_LENGTH];  /* previous one, fading out under it */
   retro_time_t bg_fade_start;

   /* Mouse and keyboard mode (else gamepad mode): whichever was used last.
    * In it the pointer shows, things under it are selected and clicked,
    * the hints name keys, and text is typed without the on-screen
    * keyboard. */
   bool kbm;
   bool cursor_known;       /* cursor_shown is what the window shows */
   bool cursor_shown;
   bool pad_active;         /* controllers polled since the menu opened */
   bool mouse_seen;         /* mouse_x / mouse_y hold a position */
   int mouse_x, mouse_y;    /* screen pixels */
   int mouse_rest_x, mouse_rest_y; /* where it was in gamepad mode */
   bool mouse_left;         /* left button down (holds as OK does) */
   bool mouse_right;
   /* Typing: the selection's other end in the line (GRID_NO_ANCHOR: no
    * selection), whether the mouse is dragging it, and Escape down (it
    * cancels when let go, so the key isn't still down after). */
   size_t edit_anchor;
   bool edit_drag;
   bool edit_esc;

   /* The on-screen keyboard (gamepad mode): its keys, from the system's
    * layout osk_layout (set up when typing starts); the key selected (row,
    * column, and the column up / down keeps to); shift (on until pressed
    * again); the symbols page; and the direction held down repeating
    * (osk_rep_at: its next step). */
   bool osk_ready;
   bomba_key_t osk_keys[BOMBA_KEYMAP_KEYS];
   unsigned osk_layout;
   int osk_row, osk_col, osk_want;
   bool osk_shift;
   bool osk_symbols;
   int osk_rep_dir;
   retro_time_t osk_rep_at;
} grid_st;

#define GRID_NO_ANCHOR ((size_t)-1)

/* What the mouse can point at, recorded while drawing: a box, what it
 * selects when pointed at (kind and index; HIT_NONE: nothing) and the
 * action a click then does. Hits drawn later are on top. */
enum grid_hit_kind
{
   HIT_NONE = 0,
   HIT_TILE,     /* a game in the grid (index into view) */
   HIT_BAR,      /* an icon bar button */
   HIT_PANEL,    /* an item of the open panel or screen (panel_sel) */
   HIT_MENU,     /* a row of RetroArch's list on the Settings screen */
   HIT_CC,       /* a row of the Consoles page (cc_sel) */
   HIT_CAT       /* a Settings tab: a category, or a game's tab (cc_sub) */
};

typedef struct
{
   float x, y, w, h;
   enum grid_hit_kind kind;
   unsigned idx;
   enum menu_action action;
} grid_hit_t;

#define GRID_MAX_HITS 192
#define GRID_EDIT_MAX 255 /* bytes of a typed line the mouse can place in */

typedef struct
{
   grid_hit_t hit[GRID_MAX_HITS];
   unsigned count;
   /* The text field being typed in, when shown: its box, where the text
    * starts (scrolled) and each byte's offset from there. */
   bool edit;
   float edit_x, edit_y, edit_w, edit_h, edit_x0;
   size_t edit_len;
   float edit_xs[GRID_EDIT_MAX + 1];
} grid_hits_t;

/* Being drawn, and the last frame's (what the mouse uses). */
static grid_hits_t grid_hits_next, grid_hits;

static void grid_hit_add(float x, float y, float w, float h,
      enum grid_hit_kind kind, unsigned idx, enum menu_action action)
{
   grid_hit_t *t;
   if (grid_hits_next.count >= GRID_MAX_HITS)
      return;
   t         = &grid_hits_next.hit[grid_hits_next.count++];
   t->x      = x;
   t->y      = y;
   t->w      = w;
   t->h      = h;
   t->kind   = kind;
   t->idx    = idx;
   t->action = action;
}

/* Whether the pointer is over the box (mouse and keyboard mode only). */
static bool grid_hover(float x, float y, float w, float h)
{
   return grid_st.kbm && grid_st.mouse_seen
      && grid_st.mouse_x >= x && grid_st.mouse_x < x + w
      && grid_st.mouse_y >= y && grid_st.mouse_y < y + h;
}

/* From menu_cbs_ok.c; also handles "quit on close" and the menu stack. */
int action_ok_close_content(const char *path, const char *label,
      unsigned type, size_t idx, size_t entry_idx);

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */

/* Opacity of everything drawn this frame (the menu fading in), applied by
 * grid_set_color and to text. 1 outside the grid's frame. */
static float grid_alpha = 1.0f;

/* How long the menu fades out as it closes: a game's loading screen more
 * slowly. */
static retro_time_t grid_close_usec(void);

static void grid_set_color(float *dst, uint32_t rgba)
{
   int i;
   float r = ((rgba >> 24) & 0xff) / 255.0f;
   float g = ((rgba >> 16) & 0xff) / 255.0f;
   float b = ((rgba >>  8) & 0xff) / 255.0f;
   float a = ( rgba        & 0xff) / 255.0f * grid_alpha;

   for (i = 0; i < 4; i++)
   {
      dst[i * 4 + 0] = r;
      dst[i * 4 + 1] = g;
      dst[i * 4 + 2] = b;
      dst[i * 4 + 3] = a;
   }
}

/* Blend two opaque colours; t = 0 gives a, 1 gives b. */
static uint32_t grid_mix(uint32_t a, uint32_t b, float t)
{
   uint32_t out = 0xff;
   int shift;

   for (shift = 8; shift <= 24; shift += 8)
   {
      float ca = (a >> shift) & 0xff;
      float cb = (b >> shift) & 0xff;
      out |= ((uint32_t)(ca + (cb - ca) * t) & 0xff) << shift;
   }
   return out;
}

static bool grid_content_running(void)
{
   return (runloop_state_get_ptr()->current_core.flags
         & RETRO_CORE_FLAG_GAME_LOADED) ? true : false;
}

/* One of RetroArch's menu sounds (assets/sounds), if the player has that
 * sound on. */
static void grid_sound(enum audio_mixer_system_slot slot)
{
#ifdef HAVE_AUDIOMIXER
   settings_t *settings = config_get_ptr();
   bool on              = settings->bools.audio_enable_menu;

   switch (slot)
   {
      case AUDIO_MIXER_SYSTEM_SLOT_OK:
         on = on && settings->bools.audio_enable_menu_ok;
         break;
      case AUDIO_MIXER_SYSTEM_SLOT_CANCEL:
         on = on && settings->bools.audio_enable_menu_cancel;
         break;
      case AUDIO_MIXER_SYSTEM_SLOT_UP:
      case AUDIO_MIXER_SYSTEM_SLOT_DOWN:
         on = on && settings->bools.audio_enable_menu_scroll;
         break;
      default:
         on = on && settings->bools.audio_enable_menu_notice;
         break;
   }
   if (on)
      audio_driver_mixer_play_menu_sound(slot);
#endif
}

/* "Mega Man X (USA) (Rev 1)" -> "Mega Man X" */
static void grid_clean_title(char *s)
{
   char *p;
   size_t len;

   for (p = s; *p; p++)
   {
      if ((*p == '(' || *p == '[') && p > s)
      {
         *p = '\0';
         break;
      }
   }

   len = strlen(s);
   while (len > 0 && isspace((unsigned char)s[len - 1]))
      s[--len] = '\0';
}

/* "~Hack~ Pokemon Unbound" -> "Pokemon Unbound" */
static const char *grid_skip_ra_tags(const char *s)
{
   while (*s == '~')
   {
      const char *end = strchr(s + 1, '~');
      if (!end)
         break;
      s = end + 1;
      while (*s == ' ')
         s++;
   }
   return s;
}

/* libretro-thumbnails replaces these characters with '_' */
static void grid_sanitize_thumb_name(char *s)
{
   for (; *s; s++)
      if (strchr("&*/:`<>?\\|\"", *s))
         *s = '_';
}

/* GameCube and Wii share disc image formats; read the header to tell
 * them apart. Returns "Wii", "GameCube" or NULL (not a disc / unknown).
 *  - RVZ/WIA: header 2 starts at 0x48 with a big-endian disc type
 *    (1 = GameCube, 2 = Wii).
 *  - ISO/GCM: raw disc; Wii magic 0x5D1C9EA3 at 0x18, GameCube magic
 *    0xC2339F3D at 0x1C. */
static const char *grid_disc_system(const char *path, const char *ext)
{
   uint8_t b[0x50];
   int64_t got;
   RFILE *f;

   if (     !string_is_equal_noncase(ext, "rvz")
         && !string_is_equal_noncase(ext, "wia")
         && !string_is_equal_noncase(ext, "iso")
         && !string_is_equal_noncase(ext, "gcm"))
      return NULL;

   if (!(f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      return NULL;
   got = filestream_read(f, b, sizeof(b));
   filestream_close(f);

   if (got < (int64_t)sizeof(b))
      return NULL;

   if (memcmp(b, "RVZ\x01", 4) == 0 || memcmp(b, "WIA\x01", 4) == 0)
   {
      uint32_t type = ((uint32_t)b[0x48] << 24) | ((uint32_t)b[0x49] << 16)
                    | ((uint32_t)b[0x4A] <<  8) |  (uint32_t)b[0x4B];
      return type == 2 ? "Wii" : type == 1 ? "GameCube" : NULL;
   }

   if (b[0x18] == 0x5D && b[0x19] == 0x1C && b[0x1A] == 0x9E && b[0x1B] == 0xA3)
      return "Wii";
   if (b[0x1C] == 0xC2 && b[0x1D] == 0x33 && b[0x1E] == 0x9F && b[0x1F] == 0x3D)
      return "GameCube";

   return NULL; /* e.g. a PSP/PS2 ISO */
}

/* First release (YYYYMM, earliest region) of each system, for ordering
 * tabs newest first. Labels not listed sort last. */
static unsigned grid_system_release(const char *label)
{
   static const struct { const char *label; unsigned date; } map[] = {
      { "Switch",        201703 },
      { "3DS",           201102 }, { "Wii",           200611 },
      { "PSP",           200412 }, { "DS",            200411 },
      { "GameCube",      200109 }, { "GBA",           200103 },
      { "WonderSwan Color", 200012 }, { "NGPC",       199903 },
      { "WonderSwan",    199903 }, { "GBC",           199810 },
      { "NGP",           199810 }, { "N64",           199606 },
      { "Virtual Boy",   199507 }, { "32X",           199411 },
      { "SNES",          199011 }, { "Game Gear",     199010 },
      { "Lynx",          198909 }, { "Game Boy",      198904 },
      { "Genesis",       198810 }, { "PC Engine",     198710 },
      { "Atari 7800",    198605 }, { "FDS",           198602 },
      { "Master System", 198510 }, { "NES",           198307 },
      { "ColecoVision",  198208 }, { "Atari 2600",    197709 }
   };
   size_t i;

   for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
      if (string_is_equal(label, map[i].label))
         return map[i].date;
   return 0;
}

/* Tab label (full system name) for a file extension. False if the
 * extension isn't a known system's (s is then the extension, uppercased). */
static bool grid_system_label(const char *ext, char *s, size_t len)
{
   static const struct { const char *ext, *label; } map[] = {
      { "gba",  "GBA" },           { "gb",   "Game Boy" },
      { "gbc",  "GBC" },           { "sfc",  "SNES" },
      { "smc",  "SNES" },          { "nes",  "NES" },
      { "fds",  "FDS" },
      { "n64",  "N64" },           { "z64",  "N64" },
      { "v64",  "N64" },           { "ndd",  "N64" },
      { "nds",  "DS" },            { "dsi",  "DS" },
      { "3ds",  "3DS" },           { "cci",  "3DS" },
      { "rvz",  "GameCube" },      { "gcz",  "GameCube" },
      { "gcm",  "GameCube" },      { "ciso", "GameCube" },
      { "wbfs", "Wii" },           { "wad",  "Wii" },
      { "nsp",  "Switch" },        { "xci",  "Switch" },
      { "nca",  "Switch" },        { "nro",  "Switch" },
      { "nso",  "Switch" },
      { "vb",   "Virtual Boy" },
      { "md",   "Genesis" },       { "gen",  "Genesis" },
      { "smd",  "Genesis" },       { "sms",  "Master System" },
      { "gg",   "Game Gear" },     { "32x",  "32X" },
      { "cso",  "PSP" },           { "pbp",  "PSP" },
      { "pce",  "PC Engine" },
      { "a26",  "Atari 2600" },    { "a78",  "Atari 7800" },
      { "lnx",  "Lynx" },
      { "ws",   "WonderSwan" },    { "wsc",  "WonderSwan Color" },
      { "ngp",  "NGP" },           { "ngc",  "NGPC" },
      { "col",  "ColecoVision" }
   };
   size_t i;

   for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
      if (string_is_equal_noncase(ext, map[i].ext))
      {
         strlcpy(s, map[i].label, len);
         return true;
      }

   for (i = 0; ext[i] && i + 1 < len; i++)
      s[i] = toupper((unsigned char)ext[i]);
   s[i] = '\0';
   return false;
}

bool grid_home_console_for_file(const char *path, char *s, size_t len)
{
   const char *ext = path_get_extension(path);
   const char *disc;

   if (string_is_empty(ext))
      return false;
   if ((disc = grid_disc_system(path, ext)))
   {
      strlcpy(s, disc, len);
      return true;
   }
   return grid_system_label(ext, s, len);
}

/* Case-insensitive ASCII substring search. */
static bool grid_contains_noncase(const char *hay, const char *needle)
{
   size_t i, n = strlen(needle);

   if (!n)
      return true;

   for (; *hay; hay++)
   {
      for (i = 0; i < n && hay[i]; i++)
         if (tolower((unsigned char)hay[i]) != tolower((unsigned char)needle[i]))
            break;
      if (i == n)
         return true;
   }
   return false;
}

/* Title and RetroAchievements icon for a ROM, falling back to the file
 * name. Returns the lookup state (PENDING while RetroAchievements hasn't
 * answered yet). */
static enum grid_meta_state grid_resolve_title(const char *path,
      char *title, size_t title_len, char *icon, size_t icon_len)
{
   char ra_title[NAME_MAX_LENGTH];
   enum grid_meta_state state;

   ra_title[0] = icon[0] = '\0';
   state       = grid_meta_get(path, ra_title, sizeof(ra_title),
         icon, icon_len);

   if (state == GRID_META_FOUND && !string_is_empty(ra_title))
      strlcpy(title, grid_skip_ra_tags(ra_title), title_len);
   else
   {
      strlcpy(title, path_basename(path), title_len);
      path_remove_extension(title);
      grid_clean_title(title);
   }
   grid_title_fix_article(title);

   return state;
}

/* ---------------------------------------------------------------------- */
/* Game list                                                              */
/* ---------------------------------------------------------------------- */

static void grid_free_textures(void)
{
   size_t i;

   for (i = 0; i < grid_st.count; i++)
   {
      if (grid_st.games[i].art)
         video_driver_texture_unload(&grid_st.games[i].art);
      grid_st.games[i].art       = 0;
      grid_st.games[i].art_tried = false;
   }
}

static void grid_find_art(grid_game_t *game, const core_info_t *core,
      const char *rom_name)
{
   size_t i;
   char name[NAME_MAX_LENGTH];
   char db_dir[PATH_MAX_LENGTH];
   char art_dir[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();
   const char *thumbs  = settings->paths.directory_thumbnails;

   game->boxart_path[0] = '\0';

   if (string_is_empty(thumbs) || !core || !core->databases_list)
      return;

   strlcpy(name, rom_name, sizeof(name));
   path_remove_extension(name);
   grid_sanitize_thumb_name(name);
   strlcat(name, ".png", sizeof(name));

   for (i = 0; i < core->databases_list->size; i++)
   {
      fill_pathname_join_special(db_dir, thumbs,
            core->databases_list->elems[i].data, sizeof(db_dir));
      fill_pathname_join_special(art_dir, db_dir,
            "Named_Boxarts", sizeof(art_dir));
      fill_pathname_join_special(game->boxart_path, art_dir,
            name, sizeof(game->boxart_path));

      if (path_is_valid(game->boxart_path))
         return;
   }

   game->boxart_path[0] = '\0';
}

/* Fills title/art. Art preference: SteamGridDB cover, RetroAchievements
 * icon, libretro boxart. The cover is looked up by the resolved title, so
 * only once RetroAchievements has answered. */
static void grid_apply_meta(grid_game_t *game)
{
   char icon[PATH_MAX_LENGTH];
   char cover[PATH_MAX_LENGTH];
   const char *art;

   cover[0] = '\0';

   if (grid_resolve_title(game->path, game->title, sizeof(game->title),
            icon, sizeof(icon)) != GRID_META_PENDING)
   {
      grid_cover_get(game->title, cover, sizeof(cover));
      /* Background banner downloads along with the cover. */
      grid_cover_hero_queue(game->title);
      /* And its icons, as profile pictures. */
      grid_cover_icons_queue(game->title);
   }

   art = !string_is_empty(cover) ? cover
       : !string_is_empty(icon)  ? icon
       : game->boxart_path;

   if (!string_is_equal(art, game->art_path))
   {
      if (game->art)
         video_driver_texture_unload(&game->art);
      game->art       = 0;
      game->art_tried = false;
      game->art_cover = !string_is_empty(cover);
      strlcpy(game->art_path, art, sizeof(game->art_path));
   }
}

/* Each game's position in RetroArch's play history (newest first) and
 * total time played (from RetroArch's runtime logs), for the Recent and
 * Time Played sorts. */
static void grid_update_recent(void)
{
   size_t i, j;
   playlist_t *history  = g_defaults.content_history;
   size_t n             = history ? playlist_get_size(history) : 0;
   settings_t *settings = config_get_ptr();
   bool per_core        = settings->bools.content_runtime_log;
   bool use_log         = per_core
      || settings->bools.content_runtime_log_aggregate;

   for (i = 0; i < grid_st.count; i++)
   {
      grid_game_t *game = &grid_st.games[i];
      runtime_log_t *log;

      game->recent = UINT_MAX;
      game->played = 0;

      if (!use_log || string_is_empty(game->core_path))
         continue;

      if ((log = runtime_log_init(game->path, game->core_path,
                  settings->paths.directory_runtime_log,
                  settings->paths.directory_playlist, per_core)))
      {
         game->played = log->runtime.hours * 3600
                      + log->runtime.minutes * 60
                      + log->runtime.seconds;
         free(log);
      }
   }

   /* History keeps resolved paths ("<top>\roms\..."), the grid the content
    * folder as configured ("<app>\..\roms\..."): compare both resolved. */
   if (n)
   {
      char (*hist)[PATH_MAX_LENGTH] = (char(*)[PATH_MAX_LENGTH])
         calloc(n, PATH_MAX_LENGTH);
      char game_path[PATH_MAX_LENGTH];

      if (!hist)
         return;

      for (j = 0; j < n; j++)
      {
         const struct playlist_entry *entry = NULL;

         playlist_get_index(history, j, &entry);
         if (!entry || string_is_empty(entry->path))
            continue;
         strlcpy(hist[j], entry->path, PATH_MAX_LENGTH);
         path_resolve_realpath(hist[j], PATH_MAX_LENGTH, false);
      }

      for (i = 0; i < grid_st.count; i++)
      {
         strlcpy(game_path, grid_st.games[i].path, sizeof(game_path));
         path_resolve_realpath(game_path, sizeof(game_path), false);

         for (j = 0; j < n; j++)
            if (string_is_equal_noncase(hist[j], game_path))
            {
               grid_st.games[i].recent = (unsigned)j;
               break;
            }
      }

      free(hist);
   }
}

static void grid_scan(void)
{
   size_t i;
   struct string_list *files = NULL;
   core_info_list_t *cores   = NULL;
   settings_t *settings      = config_get_ptr();
   const char *dir           = settings->paths.directory_menu_content;

   grid_free_textures();
   grid_st.count   = 0;
   grid_st.scanned = true;
   strlcpy(grid_st.scanned_dir, dir, sizeof(grid_st.scanned_dir));

   if (string_is_empty(dir) || !path_is_directory(dir))
      return;

   core_info_get_list(&cores);
   if (!cores)
      return;

   if (!grid_st.games)
      grid_st.games = (grid_game_t*)calloc(GRID_MAX_GAMES, sizeof(grid_game_t));
   if (!grid_st.view)
      grid_st.view  = (size_t*)calloc(GRID_MAX_GAMES, sizeof(size_t));
   if (!grid_st.games || !grid_st.view)
      return;

   files = dir_list_new(dir, NULL, false, false, false, false);
   if (!files)
      return;

   dir_list_sort(files, false);

   for (i = 0; i < files->size && grid_st.count < GRID_MAX_GAMES; i++)
   {
      const core_info_t *infos = NULL;
      size_t num_infos         = 0;
      const char *path         = files->elems[i].data;
      grid_game_t *game        = &grid_st.games[grid_st.count];

      core_info_list_get_supported_cores(cores, path, &infos, &num_infos);
      if (num_infos == 0)
      {
         /* No core for it yet: fetch one if the system is clear, and show
          * the game as pending (not playable) while it downloads. */
         grid_cores_request(path);
         if (grid_cores_state(path, NULL) != GRID_CORE_INSTALLING)
            continue;
      }

      memset(game, 0, sizeof(*game));
      strlcpy(game->path, path, sizeof(game->path));
      {
         const char *disc = grid_disc_system(path, path_get_extension(path));
         if (disc)
            strlcpy(game->system, disc, sizeof(game->system));
         else
            grid_system_label(path_get_extension(path),
                  game->system, sizeof(game->system));
      }

      if (num_infos == 0)
         game->pending = true;
      else
      {
         /* The emulator chosen for the console (Settings › Consoles), or
          * the first that fits. */
         const char *chosen = grid_controls_core(game->system);
         size_t pick        = 0, k;

         for (k = 0; !string_is_empty(chosen) && k < num_infos; k++)
            if (string_is_equal(path_basename(infos[k].path), chosen))
            {
               pick = k;
               break;
            }

         strlcpy(game->core_path, infos[pick].path, sizeof(game->core_path));
         /* infos points into a list that the next lookup re-sorts,
          * so resolve the boxart now while it is valid. */
         grid_find_art(game, &infos[pick], path_basename(path));

         /* Core installed but its system files (Dolphin's Sys folder,
          * ...) missing: fetch them; not playable until they're in. */
         grid_cores_request_files(game->core_path);
         game->pending = grid_cores_state(path, game->core_path)
               == GRID_CORE_INSTALLING;
      }

      grid_apply_meta(game);

      grid_st.count++;
   }

   string_list_free(files);
   grid_update_recent();
}

static bool grid_is_running(const grid_game_t *game)
{
   const char *content = path_get(RARCH_PATH_CONTENT);
   return grid_content_running()
      && !string_is_empty(content)
      && string_is_equal(content, game->path);
}

/* Starts a game behind its loading screen: the screen fades in over the
 * grid first (grid_draw_loading), and once it has been drawn whole the
 * game starts (grid_launch_now, from grid_home_iterate). RetroArch stops
 * while a game loads, with that screen up; it then fades out over the
 * game as the menu closes. */
static void grid_launch(const grid_game_t *game)
{
   if (grid_st.loading != LOADING_NONE)
      return;
   grid_st.loading_game  = *game;
   grid_st.loading       = LOADING_IN;
   grid_st.loading_start = cpu_features_get_time_usec();
}

static retro_time_t grid_close_usec(void)
{
   return grid_st.loading == LOADING_OUT ? GRID_LOADING_USEC : GRID_CLOSE_USEC;
}

#ifdef HAVE_MIST
/* steam/steam.c (steam.h needs mist's headers). */
void steam_set_game_title(const char *path, const char *title);
#endif

#ifdef _WIN32
/* (kernel32; windows.h isn't included here.) */
__declspec(dllimport) int __stdcall SetEnvironmentVariableA(
      const char *name, const char *value);
#endif

static void grid_launch_now(void)
{
   const grid_game_t *game = &grid_st.loading_game;
   content_ctx_info_t content_info;

   content_info.argc        = 0;
   content_info.argv        = NULL;
   content_info.args        = NULL;
   content_info.environ_get = NULL;

#ifdef _WIN32
   /* For a launcher core's own loading screen (Eden's), so it shows the
    * same name. */
   SetEnvironmentVariableA("BOMBA_GAME_TITLE", game->title);
   /* The player's name inside the game (Eden's Switch user). */
   SetEnvironmentVariableA("BOMBA_PROFILE",
         grid_profile_name(grid_profile_current()));
#endif

   /* Its console's game buttons, as RetroArch's remap for this game. */
   grid_controls_apply(game->system, game->core_path, game->path);
   /* A multiplayer host's guests start it too, once it runs. */
   bomba_steamnet_set_game(game->system, game->title);
#ifdef HAVE_MIST
   /* Steam shows friends this name, not the file's. */
   steam_set_game_title(game->path, game->title);
#endif

   task_push_load_content_with_new_core_from_menu(
         game->core_path, game->path, &content_info,
         CORE_TYPE_PLAIN, NULL, NULL);

   /* (Loading is done by now.) Started: the screen fades out with the
    * menu closing. Failed: back to the grid, RetroArch says why. */
   grid_st.loading = grid_is_running(game) ? LOADING_OUT : LOADING_NONE;
}

/* ---------------------------------------------------------------------- */
/* Tabs and view                                                          */
/* ---------------------------------------------------------------------- */

static void grid_add_tab(enum grid_tab_type type, const char *label)
{
   unsigned i;

   if (grid_st.tab_count >= GRID_MAX_TABS)
      return;
   for (i = 0; i < grid_st.tab_count; i++)
      if (string_is_equal(grid_st.tabs[i].label, label))
         return;

   grid_st.tabs[grid_st.tab_count].type = type;
   strlcpy(grid_st.tabs[grid_st.tab_count].label, label,
         sizeof(grid_st.tabs[0].label));
   grid_st.tab_count++;
}

/* Newest system first; same date or unknown systems by name. */
static int grid_tab_cmp(const void *a, const void *b)
{
   const char *la = ((const grid_tab_t*)a)->label;
   const char *lb = ((const grid_tab_t*)b)->label;
   unsigned da    = grid_system_release(la);
   unsigned db    = grid_system_release(lb);

   if (da != db)
      return da > db ? -1 : 1;
   return strcmp(la, lb);
}

/* Rebuilds the console list: All, then one per system present,
 * newest system first. */
static void grid_build_tabs(void)
{
   size_t i;

   grid_st.tab_count = 0;
   grid_add_tab(TAB_ALL, "All");

   for (i = 0; i < grid_st.count; i++)
      grid_add_tab(TAB_SYSTEM, grid_st.games[i].system);

   if (grid_st.tab_count > 2)
      qsort(&grid_st.tabs[1], grid_st.tab_count - 1, sizeof(grid_tab_t),
            grid_tab_cmp);
}

static const grid_tab_t *grid_current_tab(void)
{
   unsigned i;
   for (i = 0; i < grid_st.tab_count; i++)
      if (string_is_equal(grid_st.tabs[i].label, grid_st.tab_label))
         return &grid_st.tabs[i];
   /* Tab gone (e.g. last game of a system removed): back to ALL. */
   strlcpy(grid_st.tab_label, grid_st.tabs[0].label, sizeof(grid_st.tab_label));
   return &grid_st.tabs[0];
}

static unsigned grid_current_tab_index(void)
{
   return (unsigned)(grid_current_tab() - grid_st.tabs);
}

static const char *grid_search_text(void)
{
   /* While the search keyboard is open, filter live as the user types. */
   if (menu_input_dialog_get_display_kb() && grid_st.kb == KB_SEARCH)
   {
      const char *buf = menu_input_dialog_get_buffer();
      return buf ? buf : "";
   }
   return grid_st.search;
}

static bool grid_view_accepts(const grid_tab_t *tab, const grid_game_t *game,
      const char *search)
{
   if (tab->type == TAB_SYSTEM && !string_is_equal(tab->label, game->system))
      return false;
   return grid_contains_noncase(game->title, search);
}

static int grid_title_cmp(const grid_game_t *a, const grid_game_t *b)
{
   const char *pa = a->title, *pb = b->title;

   for (; *pa && *pb; pa++, pb++)
   {
      int ca = tolower((unsigned char)*pa);
      int cb = tolower((unsigned char)*pb);
      if (ca != cb)
         return ca - cb;
   }
   return (unsigned char)*pa - (unsigned char)*pb;
}

/* Orders view entries (indices into games) by the current sort; ties keep
 * file order. */
static int grid_view_cmp(const void *a, const void *b)
{
   size_t ia             = *(const size_t*)a;
   size_t ib             = *(const size_t*)b;
   const grid_game_t *ga = &grid_st.games[ia];
   const grid_game_t *gb = &grid_st.games[ib];
   int r                 = 0;

   switch (grid_st.sort)
   {
      case SORT_RECENT:
         /* Played games newest first, then the rest by title. */
         if (ga->recent != gb->recent)
            return ga->recent < gb->recent ? -1 : 1;
         r = grid_title_cmp(ga, gb);
         break;
      case SORT_TIME_PLAYED:
         /* Most played first, then the rest by title. */
         if (ga->played != gb->played)
            return ga->played > gb->played ? -1 : 1;
         r = grid_title_cmp(ga, gb);
         break;
      default:
         r = grid_title_cmp(ga, gb);
         break;
   }

   if (r)
      return r;
   return ia < ib ? -1 : ia > ib ? 1 : 0;
}

/* Rebuilds the list of games shown for the current console, search and
 * sort. */
static void grid_build_view(void)
{
   size_t i;
   const grid_tab_t *tab;
   const char *search = grid_search_text();

   grid_build_tabs();
   tab                = grid_current_tab();
   grid_st.view_count = 0;

   if (!grid_st.view)
      return;

   for (i = 0; i < grid_st.count; i++)
      if (grid_view_accepts(tab, &grid_st.games[i], search))
         grid_st.view[grid_st.view_count++] = i;

   qsort(grid_st.view, grid_st.view_count, sizeof(size_t), grid_view_cmp);

   if (grid_st.selection >= grid_st.view_count)
      grid_st.selection = grid_st.view_count ? grid_st.view_count - 1 : 0;
}

/* Back to the first game, scrolled to the start. */
static void grid_reset_selection(void)
{
   grid_st.selection = 0;
   grid_st.first_col = 0;
   grid_st.scroll_x  = 0.0f;
}

/* Next console (wrapping), for L. */
static void grid_next_console(void)
{
   unsigned idx = grid_current_tab_index() + 1;

   if (idx >= grid_st.tab_count)
      idx = 0;

   strlcpy(grid_st.tab_label, grid_st.tabs[idx].label,
         sizeof(grid_st.tab_label));
   grid_reset_selection();
}

/* Zoom (Select): one more row per column, wrapping 4 -> 1. The selected game
 * stays selected. Tiles then glide from the old layout to the new one. */
static void grid_zoom(void)
{
   if (grid_st.rows < 1 || grid_st.rows > 4)
      grid_st.rows = GRID_ROWS_DEFAULT;
   grid_st.zoom_from        = grid_st.rows;
   grid_st.zoom_from_scroll = grid_st.scroll_x;
   grid_st.zoom_start       = cpu_features_get_time_usec();
   grid_st.rows      = grid_st.rows % 4 + 1;
   grid_st.first_col = grid_st.selection / grid_st.rows;
   grid_st.zoomed    = true;
}

/* Next sort order (wrapping), for R. */
static void grid_next_sort(void)
{
   grid_st.sort = (enum grid_sort)((grid_st.sort + 1) % SORT_COUNT);
   grid_reset_selection();
}

/* ---------------------------------------------------------------------- */
/* Input                                                                  */
/* ---------------------------------------------------------------------- */

static void grid_bg_free(grid_bg_t *bg)
{
   if (bg->tex)
      video_driver_texture_unload(&bg->tex);
   bg->tex = 0;
}

/* Drops the background texture (e.g. the video context is going). */
static void grid_bg_free_all(void)
{
   grid_bg_free(&grid_st.bg);
   grid_st.bg_key[0]  = '\0';
   grid_st.bg_prev[0] = '\0';
}

static void grid_settings_sync_controllers(void);
static void grid_unbind_escape_quit(void);

void grid_home_toggle(bool menu_on)
{
   /* Back to the game: every player has the controller set up as now. */
   grid_settings_sync_controllers();

   /* The menu opening over a game: remember its emulator's settings, so
    * Settings › Consoles can show them when it isn't running. */
   if (menu_on && grid_content_running())
      grid_coreopts_capture();

   /* Fresh snapshot of the game each time the menu opens: forget the old
    * one so it is captured again. */
   if (menu_on && string_is_equal(grid_st.bg_key, GRID_BG_GAME))
      grid_st.bg_key[0] = '\0';

   /* The game menu animates afresh, not finishing an old open or close. */
   grid_st.gm_open    = false;
   grid_st.gm_closing = false;

   /* Opening over a game: the menu fades in over it; closing back to it,
    * out. */
   grid_st.open_start  = (menu_on && grid_content_running())
      ? cpu_features_get_time_usec() : 0;
   grid_st.close_start = (!menu_on && grid_content_running())
      ? cpu_features_get_time_usec() : 0;
   if (grid_content_running())
      grid_sound(menu_on ? AUDIO_MIXER_SYSTEM_SLOT_NOTICE
                         : AUDIO_MIXER_SYSTEM_SLOT_NOTICE_BACK);
   /* A started game's loading screen fades out as the menu closes. */
   if (menu_on || !grid_st.close_start)
      grid_st.loading = LOADING_NONE;

   /* Controllers are looked at afresh (what is held as the menu opens
    * isn't a new press); leaving it, the pointer is as RetroArch has it,
    * hidden in fullscreen. */
   grid_unbind_escape_quit();
   grid_st.pad_active  = false;
   grid_st.mouse_left  = false;
   grid_st.mouse_right = false;
   if (!menu_on)
   {
      video_driver_state_t *video_st = video_state_get_ptr();
      if (video_st->poke && video_st->poke->show_mouse)
         video_st->poke->show_mouse(video_st->data,
               !config_get_ptr()->bools.video_fullscreen);
      grid_st.cursor_known = false;
   }

   /* The running game may have unlocked achievements since: look its
    * progress up again. */
   if (menu_on && grid_content_running())
      grid_cheevos_progress_reset(
            grid_meta_game_id(path_get(RARCH_PATH_CONTENT)));

   if (menu_on)
   {
      /* Back on the grid; the menu stack is reset when Settings opens. */
      if (grid_st.settings)
         grid_st.kb = KB_SEARCH;
      grid_st.settings       = false;
      grid_st.settings_quick = false;
      grid_st.settings_base  = 0;
      grid_st.game_settings  = false;
      grid_st.cc_level       = 0;
      /* A game may have been played since: refresh the recent order. */
      grid_update_recent();
   }
}

static void grid_search_done(void *userdata, const char *line)
{
   strlcpy(grid_st.search, line ? line : "", sizeof(grid_st.search));
   grid_reset_selection();
   menu_input_dialog_end();
}

static void grid_start_search(void)
{
   menu_input_ctx_line_t line;

   line.label         = "Search";
   line.label_setting = "";
   line.type          = 0;
   line.idx           = 0;
   line.cb            = grid_search_done;

   grid_st.kb = KB_SEARCH;
   menu_input_dialog_start(&line);
}

static void grid_profile_name_done(void *userdata, const char *line)
{
   if (!string_is_empty(line) && grid_profile_add(line))
      grid_st.panel_sel = grid_profile_current() + GRID_STRIP_FIRST;
   menu_input_dialog_end();
   grid_st.kb = KB_SEARCH;
}

static void grid_start_profile_name(void)
{
   menu_input_ctx_line_t line;

   line.label         = "Profile name";
   line.label_setting = "";
   line.type          = 0;
   line.idx           = 0;
   line.cb            = grid_profile_name_done;

   grid_st.kb = KB_PROFILE_NAME;
   menu_input_dialog_start(&line);
}

static void grid_profile_rename_done(void *userdata, const char *line)
{
   unsigned idx = grid_st.rename_target;

   /* An empty name removes the profile (where allowed); its saves stay
    * on disk. */
   if (string_is_empty(line))
   {
      if (     grid_profile_remove(idx)
            && grid_st.panel_sel >= grid_profile_count() + GRID_STRIP_FIRST)
         grid_st.panel_sel = grid_profile_count() - 1 + GRID_STRIP_FIRST;
   }
   else
      grid_profile_rename(idx, line);

   menu_input_dialog_end();
   grid_st.kb = KB_SEARCH;
}

static void grid_start_profile_rename(unsigned idx)
{
   menu_input_ctx_line_t line;

   line.label         = "Rename profile";
   line.label_setting = "";
   line.type          = 0;
   line.idx           = 0;
   line.cb            = grid_profile_rename_done;

   grid_st.rename_target = idx;
   grid_st.kb            = KB_PROFILE_NAME;
   menu_input_dialog_start(&line);
}

/* Profiles panel: Exit, a row of profiles, then "Add" while there is room.
 * Profile p is item p + GRID_STRIP_FIRST. */
static unsigned grid_profile_items(void)
{
   unsigned n = grid_profile_count();
   return GRID_STRIP_FIRST + (n < GRID_PROFILE_MAX ? n + 1 : n);
}

static const char *grid_running_core(void);

static void grid_open_panel(enum grid_panel panel)
{
   grid_st.panel     = panel;
   grid_st.panel_sel = 0;

   if (panel == PANEL_PROFILES)
      grid_st.panel_sel = grid_profile_current() + GRID_STRIP_FIRST;
   else if (panel == PANEL_UPDATES)
   {
      /* Opens on everything, the search empty. */
      grid_updates_check(false, grid_running_core());
      grid_st.dl_filter    = DL_FILTER_ALL;
      grid_st.dl_search[0] = '\0';
   }
}

/* Installs screen's search: filters live while typing. */
static const char *grid_dl_search_text(void)
{
   if (menu_input_dialog_get_display_kb() && grid_st.kb == KB_DOWNLOADS)
   {
      const char *buf = menu_input_dialog_get_buffer();
      return buf ? buf : "";
   }
   return grid_st.dl_search;
}

static void grid_dl_search_done(void *userdata, const char *line)
{
   strlcpy(grid_st.dl_search, line ? line : "", sizeof(grid_st.dl_search));
   grid_st.panel_sel = 0;
   menu_input_dialog_end();
   grid_st.kb = KB_SEARCH;
}

static void grid_dl_start_search(void)
{
   menu_input_ctx_line_t line;

   line.label         = "Search installs";
   line.label_setting = "";
   line.type          = 0;
   line.idx           = 0;
   line.cb            = grid_dl_search_done;

   grid_st.kb = KB_DOWNLOADS;
   menu_input_dialog_start(&line);
}

/* Setup screen: a SteamGridDB column (key, where to get one) and a
 * RetroAchievements column (username, password, making an account), each
 * only if needed, then Continue below. */
enum grid_setup_item
{
   SETUP_KEY = 0,
   SETUP_KEY_PAGE,
   SETUP_USER,
   SETUP_PASS,
   SETUP_RA_PAGE,
   SETUP_CONTINUE
};

#define GRID_SETUP_MAX 6

/* The items shown, in order (panel_sel indexes it), and where each sits:
 * column (0 or 1; Continue spans both, 0.5) and row. */
static unsigned grid_setup_items(unsigned *items, float *col, float *row)
{
   unsigned n = 0, c = 0, r;

   /* The page buttons sit at the bottom of their columns, level. */
   if (grid_st.setup_key)
   {
      items[n] = SETUP_KEY;      col[n] = 0.0f; row[n++] = 0.0f;
      items[n] = SETUP_KEY_PAGE; col[n] = 0.0f;
      row[n++] = grid_st.setup_ra ? 2.0f : 1.0f;
      c++;
   }
   if (grid_st.setup_ra)
   {
      items[n] = SETUP_USER;     col[n] = c; row[n++] = 0.0f;
      items[n] = SETUP_PASS;     col[n] = c; row[n++] = 1.0f;
      items[n] = SETUP_RA_PAGE;  col[n] = c; row[n++] = 2.0f;
      c++;
   }
   r = grid_st.setup_ra ? 3 : 2;
   items[n] = SETUP_CONTINUE; col[n] = c > 1 ? 0.5f : 0.0f;
   row[n++] = (float)r;
   return n;
}

static bool grid_setup_ready(void)
{
   return !bomba_setup_busy()
       && (!grid_st.setup_key || !string_is_empty(grid_st.setup_griddb))
       && (!grid_st.setup_ra  || (!string_is_empty(grid_st.setup_user)
                               && !string_is_empty(grid_st.setup_pass)));
}

/* Opens the Setup screen while anything is missing, and closes it (and
 * looks everything up again, with the new key and login) once nothing is. */
static void grid_setup_poll(void)
{
   bool need_key = bomba_setup_need_key();
   bool need_ra  = bomba_setup_need_login();

   if (grid_st.panel == PANEL_SETUP)
   {
      if (     (need_key && grid_st.setup_key) || (need_ra && grid_st.setup_ra)
            || menu_input_dialog_get_display_kb())
         return;
      grid_st.panel = PANEL_NONE;
      grid_cover_reload_key();
      grid_st.scanned = false;
      return;
   }

   if ((!need_key && !need_ra) || grid_st.settings
         || menu_input_dialog_get_display_kb())
      return;

   grid_st.setup_key       = need_key;
   grid_st.setup_ra        = need_ra;
   grid_st.setup_griddb[0] = '\0';
   grid_st.setup_pass[0]   = '\0';
   strlcpy(grid_st.setup_user, config_get_ptr()->arrays.cheevos_username,
         sizeof(grid_st.setup_user));
   grid_st.panel     = PANEL_SETUP;
   grid_st.panel_sel = 0;
}

static void grid_setup_typed(void *userdata, const char *line)
{
   /* Nothing typed (B on an empty line) keeps what was there. */
   if (!string_is_empty(line))
      switch (grid_st.setup_typing)
      {
         case SETUP_KEY:
            strlcpy(grid_st.setup_griddb, line, sizeof(grid_st.setup_griddb));
            break;
         case SETUP_USER:
            strlcpy(grid_st.setup_user, line, sizeof(grid_st.setup_user));
            break;
         case SETUP_PASS:
            strlcpy(grid_st.setup_pass, line, sizeof(grid_st.setup_pass));
            break;
      }
   menu_input_dialog_end();
   grid_st.kb = KB_SEARCH;
}

static void grid_setup_type(unsigned item)
{
   menu_input_ctx_line_t line;

   line.label         = "";
   line.label_setting = "";
   line.type          = 0;
   line.idx           = 0;
   line.cb            = grid_setup_typed;

   grid_st.setup_typing = item;
   grid_st.kb           = KB_SETUP;
   menu_input_dialog_start(&line);
}

static void grid_setup_action(enum menu_action action)
{
   unsigned items[GRID_SETUP_MAX];
   float col[GRID_SETUP_MAX], row[GRID_SETUP_MAX];
   unsigned i, n = grid_setup_items(items, col, row);
   unsigned sel  = grid_st.panel_sel < n ? grid_st.panel_sel : 0;
   float best    = 1e9f;
   unsigned to   = sel;

   switch (action)
   {
      case MENU_ACTION_OK:
         switch (items[sel])
         {
            case SETUP_KEY:
            case SETUP_USER:
            case SETUP_PASS:
               if (!bomba_setup_busy())
                  grid_setup_type(items[sel]);
               break;
            case SETUP_KEY_PAGE:
               bomba_setup_open_page(BOMBA_SETUP_PAGE_GRIDDB_KEY);
               break;
            case SETUP_RA_PAGE:
               bomba_setup_open_page(BOMBA_SETUP_PAGE_RA_SIGNUP);
               break;
            case SETUP_CONTINUE:
               if (grid_setup_ready())
               {
                  bomba_setup_submit(
                        grid_st.setup_key ? grid_st.setup_griddb : NULL,
                        grid_st.setup_ra  ? grid_st.setup_user   : NULL,
                        grid_st.setup_ra  ? grid_st.setup_pass   : NULL);
                  /* Typed again if the login fails. */
                  grid_st.setup_pass[0] = '\0';
               }
               break;
         }
         return;
      case MENU_ACTION_UP:
      case MENU_ACTION_DOWN:
      case MENU_ACTION_LEFT:
      case MENU_ACTION_RIGHT:
         break;
      default:
         return;
   }

   /* The nearest item that way (straight on counts more than sideways). */
   for (i = 0; i < n; i++)
   {
      float dc = col[i] - col[sel], dr = row[i] - row[sel], d;
      switch (action)
      {
         case MENU_ACTION_UP:    d = dr < 0.0f ? -dr + 2.0f * fabsf(dc) : -1; break;
         case MENU_ACTION_DOWN:  d = dr > 0.0f ?  dr + 2.0f * fabsf(dc) : -1; break;
         case MENU_ACTION_LEFT:  d = dc < 0.0f ? -dc + 2.0f * fabsf(dr) : -1; break;
         default:                d = dc > 0.0f ?  dc + 2.0f * fabsf(dr) : -1; break;
      }
      if (d >= 0.0f && d < best)
      {
         best = d;
         to   = i;
      }
   }
   grid_st.panel_sel = to;
}

/* A guest starts the host's game once it is known and nothing runs; the
 * same one isn't started again after it is quit. */
static void grid_mp_follow_host(void)
{
   const char *system = bomba_steamnet_host_system();
   const char *title  = bomba_steamnet_host_title();
   char key[sizeof(grid_st.mp_started)];
   size_t i;

   if (bomba_steamnet_state() != BOMBA_STEAMNET_JOINED)
   {
      grid_st.mp_started[0] = '\0';
      return;
   }
   if (string_is_empty(title) || grid_content_running())
      return;
   snprintf(key, sizeof(key), "%s\t%s", system, title);
   if (string_is_equal(key, grid_st.mp_started))
      return;
   strlcpy(grid_st.mp_started, key, sizeof(grid_st.mp_started));

   for (i = 0; i < grid_st.count; i++)
   {
      const grid_game_t *g = &grid_st.games[i];
      if (     string_is_equal(g->system, system)
            && string_is_equal_case_insensitive(g->title, title)
            && !g->pending)
      {
         grid_launch(g);
         return;
      }
   }

   {
      char msg[320];
      snprintf(msg, sizeof(msg), "Multiplayer: the host plays %s, "
            "which isn't in your library", title);
      runloop_msg_queue_push(msg, strlen(msg), 1, 300, true, NULL,
            MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
   }
}

/* The running game's core (what can't be replaced right now), or NULL. */
static const char *grid_running_core(void)
{
   return grid_content_running() ? path_get(RARCH_PATH_CORE) : NULL;
}

static void grid_close_picker(void)
{
   size_t i;
   for (i = 0; i < grid_st.pick_count && grid_st.pick_tex; i++)
      if (grid_st.pick_tex[i])
         video_driver_texture_unload(&grid_st.pick_tex[i]);
   free(grid_st.pick_tex);
   free(grid_st.pick_tried);
   grid_st.pick_tex   = NULL;
   grid_st.pick_tried = NULL;
   grid_st.pick_count = 0;
}

/* ---- Settings screen -------------------------------------------------- */

static file_list_t *grid_menu_stack(void)
{
   return MENU_LIST_GET(menu_state_get_ptr()->entries.list, 0);
}

/* Lists deep in RetroArch's menu (1: a root list). */
static size_t grid_menu_depth(void)
{
   file_list_t *stack = grid_menu_stack();
   return stack ? stack->size : 0;
}

/* Depth of the Settings screen's top list: a root list (1), or a settings
 * page or the Quick Menu opened from one (2). */
static size_t grid_settings_base(void)
{
   return grid_st.settings_base ? grid_st.settings_base : 1;
}

/* Pops RetroArch's menu back to its root and makes the root the given list
 * (as XMB and Ozone do when switching tabs). Main thread only. */
static void grid_menu_set_root(enum msg_hash_enums label, unsigned type)
{
   menu_list_t *menu_list = menu_state_get_ptr()->entries.list;
   file_list_t *stack     = MENU_LIST_GET(menu_list, 0);
   size_t sel             = 0;

   if (!stack || !stack->size)
      return;

   while (stack->size > 1)
      menu_entries_pop_stack(&sel, 0, false);

   if (stack->list[0].label)
      free(stack->list[0].label);
   stack->list[0].label = strdup(msg_hash_to_str(label));
   stack->list[0].type  = type;

   menu_driver_deferred_push_content_list(
         MENU_LIST_GET_SELECTION(menu_list, 0));
}

/* Whether the Settings screen shows. The Quick Menu goes when the game
 * closes (Close Content flushes the menu to its root): back to the grid. */
static bool grid_settings_shown(void)
{
   return grid_st.settings
      && (!grid_st.settings_quick || grid_menu_depth() >= 2);
}

/* Shows a settings category: its root list, then (for a single page) that
 * page opened from it as if chosen there. A page hidden from the root
 * (RetroArch's "show in settings" options) leaves the root showing. */
static void grid_settings_open_cat(unsigned cat)
{
   const grid_settings_cat_t *c = &grid_settings_cats[cat];
   struct menu_state *menu_st   = menu_state_get_ptr();

   /* Set first: building the root list asks what to show (the short list
    * outside advanced mode, or which pages to leave out). */
   grid_st.cc_level      = 0;
   grid_st.settings_cat  = cat;
   grid_st.settings_base = 1;
   grid_menu_set_root(c->root, c->root_type);


   if (c->entry != MSG_UNKNOWN && (grid_st.advanced || !c->simple))
   {
      file_list_t *list = MENU_LIST_GET_SELECTION(menu_st->entries.list, 0);
      size_t i, n       = list ? list->size : 0;

      for (i = 0; i < n; i++)
      {
         menu_entry_t entry;
         MENU_ENTRY_INITIALIZE(entry);
         menu_entry_get(&entry, 0, i, NULL, false);
         if (entry.enum_idx != c->entry)
            continue;

         /* Through the menu driver, which hands it back to RetroArch (the
          * Settings screen leaves OK at its top list alone). */
         menu_st->selection_ptr = i;
         menu_entry_action(&entry, i, MENU_ACTION_OK);
         if (grid_menu_depth() >= 2)
            grid_st.settings_base = 2;
         break;
      }
   }
}

static bool grid_settings_advanced(void)
{
   return grid_st.advanced;
}

/* RetroArch's own "Show Advanced Settings" follows advanced mode, so the
 * pages opened from the short lists (e.g. a player's controls) hide their
 * technical options too. */
static void grid_settings_sync_advanced(void)
{
   config_get_ptr()->bools.menu_show_advanced_settings = grid_st.advanced;
}

static bool grid_settings_cat_visible(unsigned cat)
{
   return cat < GRID_SETTINGS_CAT_COUNT
      && (!grid_settings_cats[cat].advanced || grid_settings_advanced());
}

/* Next visible category after cat, stepping by dir (+1 / -1). */
static unsigned grid_settings_cat_step(unsigned cat, int dir)
{
   unsigned n = GRID_SETTINGS_CAT_COUNT, k;

   for (k = 0; k < n; k++)
   {
      cat = (cat + (dir < 0 ? n - 1 : 1)) % n;
      if (grid_settings_cat_visible(cat))
         break;
   }
   return cat;
}

static void grid_open_settings(void)
{
   grid_st.settings       = true;
   grid_st.settings_quick = false;
   grid_st.kb             = KB_MENU;
   grid_settings_sync_advanced();
   grid_settings_open_cat(grid_settings_cat_visible(grid_st.settings_cat)
         ? grid_st.settings_cat : 0);
}

/* Y on the Settings screen: advanced mode on / off. The category reopens
 * in the other form (short list or RetroArch's full page); leaving
 * advanced mode on Miscellaneous goes to the first category. In Quick
 * settings the open list is rebuilt. */
static void grid_settings_toggle_advanced(void)
{
   struct menu_state *menu_st = menu_state_get_ptr();

   grid_st.advanced = !grid_st.advanced;
   grid_settings_sync_advanced();

   if (!grid_st.settings_quick)
   {
      grid_settings_open_cat(grid_settings_cat_visible(grid_st.settings_cat)
            ? grid_st.settings_cat : 0);
      return;
   }

   /* The entry under the cursor may be gone: start from the top. */
   menu_st->selection_ptr  = 0;
   menu_st->flags         |= MENU_ST_FLAG_ENTRIES_NEED_REFRESH;
}

/* The short list the Settings screen shows now, if any, for RetroArch's
 * Settings root or Main Menu (main_menu). */
static const grid_simple_setting_t *grid_settings_simple(bool main_menu)
{
   const grid_settings_cat_t *c;

   if (     !grid_st.settings || grid_st.settings_quick || grid_st.advanced
         || grid_st.settings_cat >= GRID_SETTINGS_CAT_COUNT)
      return NULL;
   c = &grid_settings_cats[grid_st.settings_cat];
   if (!c->simple || (c->root == MENU_ENUM_LABEL_MAIN_MENU) != main_menu)
      return NULL;
   return c->simple;
}

/* Appends one setting to a menu list as RetroArch's own lists do. */
static bool grid_settings_append(file_list_t *list, enum msg_hash_enums idx)
{
   enum menu_displaylist_parse_type parse;
   rarch_setting_t *setting = menu_setting_find_enum(idx);

   if (!setting)
      return false;

   switch (setting->type)
   {
      case ST_ACTION:         parse = PARSE_ACTION;              break;
      case ST_BOOL:           parse = PARSE_ONLY_BOOL;           break;
      case ST_INT:            parse = PARSE_ONLY_INT;            break;
      case ST_UINT:           parse = PARSE_ONLY_UINT;           break;
      case ST_SIZE:           parse = PARSE_ONLY_SIZE;           break;
      case ST_FLOAT:          parse = PARSE_ONLY_FLOAT;          break;
      case ST_BIND:           parse = PARSE_ONLY_BIND;           break;
      case ST_STRING:         parse = PARSE_ONLY_STRING;         break;
      case ST_PATH:           parse = PARSE_ONLY_PATH;           break;
      case ST_DIR:            parse = PARSE_ONLY_DIR;            break;
      case ST_STRING_OPTIONS: parse = PARSE_ONLY_STRING_OPTIONS; break;
      default:
         return false;
   }

   /* Hand-picked, so shown whatever RetroArch thinks is advanced. */
   return menu_displaylist_parse_settings_enum(list, parse, false, setting,
         idx, true, true) == 0;
}

int grid_home_settings_build(void *list, bool main_menu)
{
   const grid_simple_setting_t *s = grid_settings_simple(main_menu);
   unsigned max_users             = config_get_ptr()->uints.input_max_users;
   int count                      = 0;
   unsigned p;

   if (!s)
      return -1;

   menu_setting_display_mode_sync();
   for (; s->entry != MSG_UNKNOWN || s->label; s++)
   {
      if (s->entry == MSG_UNKNOWN)
      {
         /* The grid's own Consoles page for this category (opened by the
          * Settings screen itself). */
         if (menu_entries_append((file_list_t*)list, s->label, GRID_CC_LABEL,
                  MSG_UNKNOWN, 0, 0, 0, NULL))
            count++;
      }
      else if (s->entry == MENU_ENUM_LABEL_INPUT_USER_1_BINDS)
      {
         for (p = 0; p < GRID_SIMPLE_PLAYERS && p < max_users; p++)
            count += grid_settings_append((file_list_t*)list,
                  (enum msg_hash_enums)(MENU_ENUM_LABEL_INPUT_USER_1_BINDS + p));
      }
      else
         count += grid_settings_append((file_list_t*)list, s->entry);
   }
   return count;
}

/* One controller setup for every player (outside advanced mode, where
 * each player has their own): player 1's controller buttons, sticks and
 * analog-to-d-pad choice are copied to the others. Keyboard keys stay per
 * player (copied, one keyboard would play for everyone); hotkeys aren't
 * per player. Cheap, so run on every menu action. Main thread only. */
static void grid_settings_sync_controllers(void)
{
   settings_t *settings = config_get_ptr();
   unsigned p, id;

   if (grid_st.advanced)
      return;

   for (p = 1; p < MAX_USERS; p++)
   {
      for (id = 0; id < RARCH_FIRST_META_KEY; id++)
      {
         input_config_binds[p][id].joykey  = input_config_binds[0][id].joykey;
         input_config_binds[p][id].joyaxis = input_config_binds[0][id].joyaxis;
      }
      settings->uints.input_analog_dpad_mode[p] =
         settings->uints.input_analog_dpad_mode[0];
   }
}

int grid_home_hotkeys_build(void *list)
{
   unsigned k;
   int count = 0;

   if (!grid_st.settings || grid_st.advanced)
      return -1;

   for (k = 0; grid_simple_hotkeys[k].id != RARCH_BIND_LIST_END; k++)
      count += grid_settings_append((file_list_t*)list,
            (enum msg_hash_enums)(MENU_ENUM_LABEL_INPUT_HOTKEY_BIND_BEGIN
               + grid_simple_hotkeys[k].id));
   return count;
}

#define GRID_BINDS_LABEL "bomba_bindings"

/* How many lists on RetroArch's menu stack are a player's controls page:
 * 1 on the RetroPad page, 2 on its Buttons page (the same RetroArch list
 * opened again from it). */
static unsigned grid_user_binds_depth(void)
{
   file_list_t *stack = grid_menu_stack();
   const char *lbl    = msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_USER_BINDS_LIST);
   unsigned i, n      = 0;

   for (i = 0; stack && i < stack->size; i++)
      if (stack->list[i].label && string_is_equal(stack->list[i].label, lbl))
         n++;
   return n;
}

int grid_home_user_binds_build(void *list, const char *group, unsigned user)
{
   rarch_setting_t *s;
   char bind_all[64], bind_defaults[64];
   bool binds_page;
   int count = 0;

   if (!grid_st.settings || grid_st.advanced || !(s = menu_setting_find(group)))
      return -1;

   binds_page = grid_user_binds_depth() >= 2;
   snprintf(bind_all, sizeof(bind_all),
         msg_hash_to_str(MENU_ENUM_LABEL_INPUT_BIND_ALL_INDEX), user);
   snprintf(bind_defaults, sizeof(bind_defaults),
         msg_hash_to_str(MENU_ENUM_LABEL_INPUT_BIND_DEFAULTS_INDEX), user);

   /* The options page leads to the bindings. */
   if (!binds_page && menu_entries_append((file_list_t*)list, "Bindings",
            GRID_BINDS_LABEL, MSG_UNKNOWN, 0, 0, 0, NULL))
      count++;

   /* The group's settings, as RetroArch lists them, split: bindings (and
    * "set all" / "reset" for them) on the Buttons page, the rest here. */
   for (; s->type != ST_END_GROUP && s->type != ST_NONE; s++)
   {
      bool bind;

      if (     s->type == ST_GROUP || s->type == ST_SUB_GROUP
            || s->type == ST_END_SUB_GROUP)
         continue;

      bind = s->type == ST_BIND
         || string_is_equal(s->name, bind_all)
         || string_is_equal(s->name, bind_defaults);
      if (bind != binds_page)
         continue;

      if (menu_entries_append((file_list_t*)list, s->short_description,
               s->name, MSG_UNKNOWN,
               (unsigned)(MENU_SETTINGS_INPUT_BEGIN + count), 0, 0, s))
         count++;
   }
   return count;
}

/* Opens the RetroPad page's Buttons page: RetroArch's same list again
 * (for the same player), which the build above then fills with the
 * bindings. */
static void grid_open_binds_page(void)
{
   file_list_t *stack = grid_menu_stack();
   const char *player;

   if (!stack || !stack->size)
      return;
   player = stack->list[stack->size - 1].path;
   generic_action_ok_displaylist_push("", NULL, player ? player : "1", 0,
         menu_state_get_ptr()->selection_ptr, 0,
         ACTION_OK_DL_USER_BINDS_LIST);
}

/* Our name for a menu entry shown from a short list, or NULL. */
static const char *grid_settings_label(enum msg_hash_enums idx,
      char *s, size_t len)
{
   const grid_simple_setting_t *e;
   unsigned k;

   if (grid_st.advanced)
      return NULL;

   for (k = 0; grid_simple_hotkeys[k].id != RARCH_BIND_LIST_END; k++)
      if (idx == MENU_ENUM_LABEL_INPUT_HOTKEY_BIND_BEGIN
            + grid_simple_hotkeys[k].id)
         return grid_simple_hotkeys[k].label;

   if (     idx >= MENU_ENUM_LABEL_INPUT_USER_1_BINDS
         && idx <  MENU_ENUM_LABEL_INPUT_USER_1_BINDS + GRID_SIMPLE_PLAYERS)
   {
      if (GRID_SIMPLE_PLAYERS == 1)
         return "RetroPad";
      snprintf(s, len, "Player %u controls",
            (unsigned)(idx - MENU_ENUM_LABEL_INPUT_USER_1_BINDS) + 1);
      return s;
   }

   for (k = 0; k < GRID_SETTINGS_CAT_COUNT; k++)
      for (e = grid_settings_cats[k].simple;
            e && (e->entry != MSG_UNKNOWN || e->label); e++)
         if (e->entry != MSG_UNKNOWN && e->entry == idx && e->label)
            return e->label;
   return NULL;
}

bool grid_home_settings_hide(int enum_idx)
{
   const grid_settings_cat_t *cur;
   unsigned k;

   if (     !grid_st.settings || grid_st.settings_quick
         || grid_st.settings_cat >= GRID_SETTINGS_CAT_COUNT)
      return false;

   /* Only on Miscellaneous: the Settings root itself, not a page opened
    * from it. */
   cur = &grid_settings_cats[grid_st.settings_cat];
   if (     cur->entry != MSG_UNKNOWN
         || cur->root  != MENU_ENUM_LABEL_SETTINGS_TAB)
      return false;

   for (k = 0; k < GRID_SETTINGS_CAT_COUNT; k++)
      if (grid_settings_cats[k].entry == (enum msg_hash_enums)enum_idx)
         return true;
   return false;
}

/* The game menu's entries for the game it is open for, in order; returns
 * how many. */
static unsigned grid_game_menu_items(unsigned *items)
{
   unsigned i, n = 0;
   bool running  = grid_st.menu_game < grid_st.count
      && grid_is_running(&grid_st.games[grid_st.menu_game]);

   for (i = 0; i < GAME_MENU_COUNT; i++)
      if (i != GAME_MENU_QUIT || running)
         items[n++] = i;
   return n;
}

/* Row of an entry in the game menu (to come back to it). */
static unsigned grid_game_menu_row(unsigned item)
{
   unsigned items[GAME_MENU_COUNT];
   unsigned k, n = grid_game_menu_items(items);

   for (k = 0; k < n; k++)
      if (items[k] == item)
         return k;
   return 0;
}

/* Back out of the Settings screen: to the grid, or to the game menu for a
 * game's Settings. RetroArch's menu goes back to its usual root. */
static void grid_close_settings(void)
{
   bool game = grid_st.game_settings;

   /* Closed first, so the root is RetroArch's own Main Menu again. */
   grid_st.cc_level       = 0;
   grid_st.settings       = false;
   grid_st.settings_quick = false;
   grid_st.game_settings  = false;
   grid_st.settings_base  = 0;
   grid_st.kb             = KB_SEARCH;
   grid_menu_set_root(MENU_ENUM_LABEL_MAIN_MENU, MENU_SETTINGS);

   if (game)
   {
      /* Its game may have moved in the list (a new emulator rescans):
       * find it again for its menu. */
      size_t i;
      for (i = 0; i < grid_st.count; i++)
         if (string_is_equal(grid_st.games[i].path, grid_st.game_path))
         {
            grid_st.menu_game = i;
            grid_st.panel     = PANEL_GAME_MENU;
            grid_st.panel_sel = grid_game_menu_row(GAME_MENU_SETTINGS);
            break;
         }
   }
   else
      grid_scan(); /* settings may have changed the games directory */
}

/* ---- Consoles ----------------------------------------------------------
 * The grid's own Consoles page, in Graphics, Audio, Controls and System:
 * the consoles in the library, then one console's settings of that kind:
 * its emulator's own settings (see grid_coreopts.c) that fit the category,
 * plus Bindings in Controls (see grid_controls.c) and the emulator choice
 * in System (with the emulator settings that fit nowhere else). Values are
 * cycled with left / right (or A); X puts one back to its default. */

#define GRID_CC_MAX_ROWS  512
#define GRID_CC_MAX_CORES 8

/* Which category's Consoles page is open. */
enum grid_cc_bucket
{
   CC_GRAPHICS = 0,
   CC_AUDIO,
   CC_CONTROLS,
   CC_SYSTEM
};

static const char *grid_cc_bucket_name[] = {
   "Graphics", "Audio", "Controls", "System"
};

enum grid_cc_row_kind
{
   CC_ROW_EMULATOR = 0, /* which core runs the console (System) */
   CC_ROW_BINDINGS,     /* opens the Bindings page (Controls) */
   CC_ROW_OPT,          /* an emulator setting (arg) */
   CC_ROW_RESET,        /* Bindings: reset all */
   CC_ROW_BUTTON,       /* Bindings: a button (arg: its id) */
   CC_ROW_NOTE          /* text only */
};

typedef struct
{
   enum grid_cc_row_kind kind;
   int arg;
} grid_cc_row_t;

/* The open console's emulators: the cores that can run its games, and the
 * one they use now. Refreshed when its page opens or the choice changes. */
static struct
{
   char paths[GRID_CC_MAX_CORES][PATH_MAX_LENGTH];
   unsigned count;
   char current[PATH_MAX_LENGTH];
} grid_cc_cores;

static enum grid_cc_bucket grid_cc_bucket(void)
{
   return (enum grid_cc_bucket)grid_st.cc_sub;
}

/* The consoles in the library, in the tabs' order. */
static unsigned grid_cc_consoles(const char **out)
{
   unsigned i, n = 0;
   for (i = 0; i < grid_st.tab_count; i++)
      if (grid_st.tabs[i].type == TAB_SYSTEM)
         out[n++] = grid_st.tabs[i].label;
   return n;
}

static const char *grid_cc_console(void)
{
   const char *consoles[GRID_MAX_TABS];
   unsigned n = grid_cc_consoles(consoles);
   return grid_st.cc_console < n ? consoles[grid_st.cc_console] : NULL;
}

/* A category's Consoles entry chosen: its page, for that category. */
static void grid_cc_open(void)
{
   const grid_simple_setting_t *s = grid_settings_cats[
      grid_st.settings_cat < GRID_SETTINGS_CAT_COUNT
      ? grid_st.settings_cat : 0].simple;

   grid_st.cc_sub   = s == grid_simple_graphics ? CC_GRAPHICS
                    : s == grid_simple_audio    ? CC_AUDIO
                    : s == grid_simple_controls ? CC_CONTROLS : CC_SYSTEM;
   grid_st.cc_level = 1;
   grid_st.cc_sel   = 0;
}

static void grid_cc_refresh_cores(const char *console)
{
   size_t i;

   grid_cc_cores.count      = 0;
   grid_cc_cores.current[0] = '\0';
   if (!console)
      return;

   for (i = 0; i < grid_st.count; i++)
   {
      core_info_list_t *list   = NULL;
      const core_info_t *infos = NULL;
      size_t k, num            = 0;
      grid_game_t *game        = &grid_st.games[i];

      if (     !string_is_equal(game->system, console)
            || string_is_empty(game->core_path))
         continue;

      strlcpy(grid_cc_cores.current, game->core_path,
            sizeof(grid_cc_cores.current));
      core_info_get_list(&list);
      if (list)
         core_info_list_get_supported_cores(list, game->path, &infos, &num);
      for (k = 0; k < num && grid_cc_cores.count < GRID_CC_MAX_CORES; k++)
         strlcpy(grid_cc_cores.paths[grid_cc_cores.count++], infos[k].path,
               sizeof(grid_cc_cores.paths[0]));
      break;
   }
}

/* A game's Settings (from its menu): its console's pages, starting on
 * Graphics, with Graphics / Audio / Controls / System as tabs. */
static void grid_open_game_settings(const grid_game_t *game)
{
   const char *consoles[GRID_MAX_TABS];
   unsigned i, n = grid_cc_consoles(consoles);

   grid_st.cc_console = 0;
   for (i = 0; i < n; i++)
      if (string_is_equal(consoles[i], game->system))
         grid_st.cc_console = i;
   strlcpy(grid_st.game_path, game->path, sizeof(grid_st.game_path));
   strlcpy(grid_st.game_title, game->title, sizeof(grid_st.game_title));

   grid_menu_set_root(MENU_ENUM_LABEL_MAIN_MENU, MENU_SETTINGS);
   grid_st.settings       = true;
   grid_st.settings_quick = false;
   grid_st.game_settings  = true;
   grid_st.settings_base  = 1;
   grid_st.kb             = KB_MENU;
   grid_settings_sync_advanced();

   grid_st.cc_sub   = CC_GRAPHICS;
   grid_st.cc_level = 2;
   grid_st.cc_sel   = 0;
   grid_cc_refresh_cores(game->system);
}

/* An emulator's name ("Snes9x"), from its core file. */
static const char *grid_cc_core_name(const char *core_path)
{
   core_info_t *info = NULL;
   if (     !string_is_empty(core_path) && core_info_find(core_path, &info)
         && info && !string_is_empty(info->core_name))
      return info->core_name;
   return string_is_empty(core_path) ? "None" : path_basename(core_path);
}

/* Whether lowercase s has any of the words. */
static bool grid_cc_has_word(const char *s, const char **words)
{
   char low[512];
   size_t i;

   for (i = 0; s && s[i] && i + 1 < sizeof(low); i++)
      low[i] = (char)tolower((unsigned char)s[i]);
   low[i] = '\0';
   for (; *words; words++)
      if (strstr(low, *words))
         return true;
   return false;
}

/* Which category an emulator setting belongs in: by its emulator's own
 * category for it ("Video", "Audio", "Input"...), else by its name and
 * key; what fits nowhere goes in System. */
static enum grid_cc_bucket grid_cc_bucket_of(unsigned opt)
{
   static const char *graphics[] = { "video", "graphic", "display",
      "screen", "render", "shader", "colo", "palette", "resolution",
      "aspect", "scal", "crop", "overscan", "filter", "blend", "frame",
      "gpu", "texture", "upscal", "widescreen", "border", "image", NULL };
   static const char *audio[]    = { "audio", "sound", "music", "volume",
      "sample", "interpolation", "reverb", "channel", "mixer", NULL };
   static const char *controls[] = { "input", "control", "joypad",
      "gamepad", "keyboard", "mouse", "gun", "rumble", "analog",
      "turbo", "button", "stick", "multitap", "port ", "paddle", NULL };
   int cat          = grid_coreopts_cat(opt);
   const char *name = grid_coreopts_name(opt);

   if (cat >= 0)
   {
      const char *cname = grid_coreopts_cat_name((unsigned)cat);
      if (grid_cc_has_word(cname, controls))
         return CC_CONTROLS;
      if (grid_cc_has_word(cname, audio))
         return CC_AUDIO;
      if (grid_cc_has_word(cname, graphics))
         return CC_GRAPHICS;
      return CC_SYSTEM;
   }
   if (grid_cc_has_word(name, controls))
      return CC_CONTROLS;
   if (grid_cc_has_word(name, audio))
      return CC_AUDIO;
   if (grid_cc_has_word(name, graphics))
      return CC_GRAPHICS;
   return CC_SYSTEM;
}

/* The rows of the open page (level 2: a console; level 3: Bindings). */
static unsigned grid_cc_rows(grid_cc_row_t *rows, unsigned max)
{
   unsigned n = 0, i;
   enum grid_cc_bucket bucket = grid_cc_bucket();
   bool known = grid_coreopts_select(grid_cc_cores.current);

#define CC_ADD(k, a) do { if (n < max) { rows[n].kind = (k); \
   rows[n].arg = (a); n++; } } while (0)

   /* Bindings: the console's controls (those its emulator uses, once
    * that's known). */
   if (grid_st.cc_level == 3)
   {
      CC_ADD(CC_ROW_RESET, 0);
      for (i = 0; i < GRID_CONTROLS_BUTTONS; i++)
      {
         const char *name = grid_coreopts_button_name(grid_controls_order[i]);
         if (!name || *name)
            CC_ADD(CC_ROW_BUTTON, (int)grid_controls_order[i]);
      }
      return n;
   }

   if (bucket == CC_SYSTEM)
      CC_ADD(CC_ROW_EMULATOR, 0);
   if (bucket == CC_CONTROLS)
      CC_ADD(CC_ROW_BINDINGS, 0);

   for (i = 0; known && i < grid_coreopts_count(); i++)
      if (grid_cc_bucket_of(i) == bucket)
         CC_ADD(CC_ROW_OPT, (int)i);

   /* Settings not known yet: say how to get them. Known and none here:
    * only say so if the page would be empty. */
   if (!known)
      CC_ADD(CC_ROW_NOTE, 0);
   else if (!n)
      CC_ADD(CC_ROW_NOTE, 1);
#undef CC_ADD
   return n;
}

/* A console control's name: the emulator's ("A", "Jump"), else where it
 * sits on the RetroPad. */
static const char *grid_cc_control_name(unsigned id)
{
   const char *name = grid_coreopts_button_name(id);
   return !string_is_empty(name) ? name : grid_controls_button_name(id);
}

/* Binding a console control: RetroArch's own "press a button or key" wait
 * (as for the RetroPad), pointed at a scratch bind instead of the
 * RetroPad's; when it ends, what was pressed becomes the console's. */
static struct
{
   struct retro_keybind bind;
   rarch_setting_t setting;
   unsigned id;
   bool active;
} grid_cc_binding;

/* Whether RetroArch's bind wait is still on (its step tops the menu). */
static bool grid_cc_bind_waiting(void)
{
   file_list_t *stack = grid_menu_stack();
   const char *lbl    = stack && stack->size
      ? stack->list[stack->size - 1].label : NULL;
   return lbl && string_is_equal(lbl, msg_hash_to_str(
            MENU_ENUM_LABEL_CUSTOM_BIND));
}

static void grid_cc_bind_start(unsigned id)
{
   struct retro_keybind *b = &grid_cc_binding.bind;
   rarch_setting_t *s      = &grid_cc_binding.setting;

   memset(b, 0, sizeof(*b));
   b->joykey  = NO_BTN;
   b->joyaxis = AXIS_NONE;
   b->key     = RETROK_UNKNOWN;
   b->mbutton = NO_BTN;
   b->id      = (uint16_t)id;
   b->valid   = true;

   memset(s, 0, sizeof(*s));
   s->type                  = ST_BIND;
   s->value.target.keybind  = b;
   s->bind_type             = MENU_SETTINGS_BIND_BEGIN + id;
   s->index_offset          = 0;

   grid_cc_binding.id     = id;
   grid_cc_binding.active = menu_input_key_bind_set_mode(
         MENU_INPUT_BINDS_CTL_BIND_SINGLE, s);
}

static void grid_cc_bindings_changed(const char *console);

/* The bind wait ended: keep what was pressed (nothing if it timed out). */
static void grid_cc_bind_poll(void)
{
   const char *console = grid_cc_console();
   struct retro_keybind *b = &grid_cc_binding.bind;

   if (!grid_cc_binding.active || grid_cc_bind_waiting())
      return;
   grid_cc_binding.active = false;
   if (!console)
      return;

   if (b->joykey != NO_BTN || b->joyaxis != AXIS_NONE)
      grid_controls_set_pad(console, grid_cc_binding.id, b->joykey,
            b->joyaxis);
   if (b->key != RETROK_UNKNOWN)
      grid_controls_set_key(console, grid_cc_binding.id, b->key);
   grid_cc_bindings_changed(console);
}

/* A console's bindings changed: if its game is running, use them now. */
static void grid_cc_bindings_changed(const char *console)
{
   size_t i;
   for (i = 0; i < grid_st.count; i++)
      if (     grid_is_running(&grid_st.games[i])
            && string_is_equal(grid_st.games[i].system, console))
      {
         grid_controls_apply_running(console);
         break;
      }
}

/* Picks the next / previous emulator for the console; its games switch to
 * it. */
static void grid_cc_step_core(const char *console, int dir)
{
   unsigned k, cur = 0;

   if (grid_cc_cores.count < 2)
      return;
   for (k = 0; k < grid_cc_cores.count; k++)
      if (string_is_equal(grid_cc_cores.paths[k], grid_cc_cores.current))
         cur = k;
   cur = (cur + (dir < 0 ? grid_cc_cores.count - 1 : 1))
      % grid_cc_cores.count;
   grid_controls_set_core(console, path_basename(grid_cc_cores.paths[cur]));
   grid_scan();
   grid_cc_refresh_cores(console);
}

static void grid_cc_action(enum menu_action action)
{
   const char *consoles[GRID_MAX_TABS];
   unsigned n_consoles = grid_cc_consoles(consoles);
   const char *console = grid_cc_console();
   grid_cc_row_t rows[GRID_CC_MAX_ROWS];
   unsigned n;
   grid_cc_row_t *row;

   if (grid_st.cc_level == 1)
   {
      switch (action)
      {
         case MENU_ACTION_UP:
            if (grid_st.cc_sel > 0)
               grid_st.cc_sel--;
            break;
         case MENU_ACTION_DOWN:
            if (grid_st.cc_sel + 1 < n_consoles)
               grid_st.cc_sel++;
            break;
         case MENU_ACTION_OK:
            if (grid_st.cc_sel < n_consoles)
            {
               grid_st.cc_console = grid_st.cc_sel;
               grid_st.cc_level   = 2;
               grid_st.cc_sel     = 0;
               grid_cc_refresh_cores(consoles[grid_st.cc_console]);
            }
            break;
         case MENU_ACTION_CANCEL:   /* back to the category */
            grid_st.cc_level = 0;
            break;
         default:
            break;
      }
      return;
   }

   if (!console)
   {
      grid_st.cc_level = 1;
      grid_st.cc_sel   = 0;
      return;
   }

   n   = grid_cc_rows(rows, GRID_CC_MAX_ROWS);
   if (grid_st.cc_sel >= n)
      grid_st.cc_sel = n ? n - 1 : 0;
   row = n ? &rows[grid_st.cc_sel] : NULL;

   switch (action)
   {
      case MENU_ACTION_UP:
         if (grid_st.cc_sel > 0)
            grid_st.cc_sel--;
         return;
      case MENU_ACTION_DOWN:
         if (grid_st.cc_sel + 1 < n)
            grid_st.cc_sel++;
         return;
      case MENU_ACTION_CANCEL:
         if (grid_st.cc_level == 3)
         {
            grid_st.cc_level = 2;
            grid_st.cc_sel   = grid_st.cc_back;
         }
         else
         {
            grid_st.cc_level = 1;
            grid_st.cc_sel   = grid_st.cc_console;
         }
         return;
      default:
         break;
   }

   if (!row)
      return;

   switch (row->kind)
   {
      case CC_ROW_BINDINGS:
         if (action == MENU_ACTION_OK)
         {
            grid_st.cc_back  = grid_st.cc_sel;
            grid_st.cc_level = 3;
            grid_st.cc_sel   = 0;
         }
         break;
      case CC_ROW_EMULATOR:
         if (     action == MENU_ACTION_OK || action == MENU_ACTION_LEFT
               || action == MENU_ACTION_RIGHT)
            grid_cc_step_core(console, action == MENU_ACTION_LEFT ? -1 : 1);
         else if (action == MENU_ACTION_SCAN)   /* X : the usual one */
         {
            grid_controls_set_core(console, "");
            grid_scan();
            grid_cc_refresh_cores(console);
         }
         break;
      case CC_ROW_OPT:
      {
         unsigned opt = (unsigned)row->arg;
         unsigned cnt = grid_coreopts_value_count(opt);
         unsigned cur = grid_coreopts_value(opt);

         if (     cnt && (action == MENU_ACTION_OK
               || action == MENU_ACTION_LEFT || action == MENU_ACTION_RIGHT))
            grid_coreopts_set(opt, (cur + (action == MENU_ACTION_LEFT
                        ? cnt - 1 : 1)) % cnt);
         else if (action == MENU_ACTION_SCAN)   /* X : default */
            grid_coreopts_set(opt, grid_coreopts_default(opt));
         break;
      }
      case CC_ROW_RESET:
         if (action == MENU_ACTION_OK)
         {
            grid_controls_reset(console);
            grid_cc_bindings_changed(console);
         }
         break;
      case CC_ROW_BUTTON:
      {
         unsigned id = (unsigned)row->arg;
         if (action == MENU_ACTION_OK)          /* A : press what to use */
            grid_cc_bind_start(id);
         else if (action == MENU_ACTION_SCAN)   /* X : the RetroPad's again */
         {
            grid_controls_clear(console, id);
            grid_cc_bindings_changed(console);
         }
         break;
      }
      default:
         break;
   }
}

/* Runs a different menu action on the selected entry, through the menu
 * driver as RetroArch's own input would. */
static void grid_settings_forward(enum menu_action action)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   size_t sel                 = menu_st->selection_ptr;
   menu_entry_t entry;

   MENU_ENTRY_INITIALIZE(entry);
   menu_entry_get(&entry, 0, sel, NULL, false);
   grid_st.forwarding = true;
   menu_entry_action(&entry, sel, action);
   grid_st.forwarding = false;
}

/* Input on the Settings screen. Below its top list everything is
 * RetroArch's (false: not consumed); at the top, Back leaves and L / R
 * switch category. */
static bool grid_settings_action(enum menu_action action)
{
   size_t depth = grid_menu_depth();

   /* The grid's own Consoles page takes all input (the menu toggle still
    * closes the menu; Back at its top returns to the category). A game's
    * Settings: Back leaves them, L / R switch tab. */
   if (grid_st.cc_level)
   {
      if (action == MENU_ACTION_TOGGLE)
         return false;
      if (grid_st.game_settings && grid_st.cc_level == 2)
      {
         if (action == MENU_ACTION_CANCEL)
         {
            grid_close_settings();
            return true;
         }
         if (     action == MENU_ACTION_SCROLL_UP
               || action == MENU_ACTION_SCROLL_DOWN)
         {
            grid_st.cc_sub = (grid_st.cc_sub
                  + (action == MENU_ACTION_SCROLL_UP ? 3 : 1)) % 4;
            grid_st.cc_sel = 0;
            return true;
         }
      }
      grid_cc_action(action);
      return true;
   }

   /* The grid's own entries: a category's Consoles, the RetroPad page's
    * Bindings. */
   if (action == MENU_ACTION_OK)
   {
      struct menu_state *menu_st = menu_state_get_ptr();
      file_list_t *list = MENU_LIST_GET_SELECTION(menu_st->entries.list, 0);
      size_t sel        = menu_st->selection_ptr;

      if (     list && sel < list->size
            && string_is_equal(list->list[sel].label, GRID_CC_LABEL))
      {
         grid_cc_open();
         return true;
      }
      if (     list && sel < list->size
            && string_is_equal(list->list[sel].label, GRID_BINDS_LABEL))
      {
         grid_open_binds_page();
         return true;
      }
   }

   /* Face buttons as labelled on the player's controller (the menu's
    * "search" is Y, its "scan" X); Select and Start do nothing (the
    * selected entry's help already scrolls along the bottom). */
   switch (action)
   {
      case MENU_ACTION_SEARCH:      /* Y : advanced mode on / off */
         grid_settings_toggle_advanced();
         return true;
      case MENU_ACTION_SCAN:        /* X : reset the entry to its default */
         grid_settings_forward(MENU_ACTION_START);
         return true;
      case MENU_ACTION_START:
      case MENU_ACTION_INFO:
         return true;
      default:
         break;
   }

   if (depth > grid_settings_base())
      return false;

   switch (action)
   {
      case MENU_ACTION_CANCEL:
         grid_close_settings();
         return true;
      case MENU_ACTION_LEFT:
      case MENU_ACTION_RIGHT:
         /* At a root list XMB would switch its own tabs; settings pages
          * and the Quick Menu (e.g. the state slot) take left / right. */
         return depth <= 1;
      case MENU_ACTION_SCROLL_UP:   /* L : previous category */
      case MENU_ACTION_SCROLL_DOWN: /* R : next category */
         if (!grid_st.settings_quick)
         {
            grid_settings_open_cat(grid_settings_cat_step(
                  grid_st.settings_cat,
                  action == MENU_ACTION_SCROLL_UP ? -1 : 1));
            return true;
         }
         return false;
      default:
         return false;
   }
}

bool grid_home_messagebox(const char *message)
{
   /* XMB never draws now; the grid shows the message (one frame's worth,
    * set again each frame while it stays up). While binding a control the
    * row itself says to press something: no box. */
   if (menu_state_get_ptr()->flags & MENU_ST_FLAG_IS_BINDING)
      return true;
   strlcpy(grid_st.box_msg, message ? message : "", sizeof(grid_st.box_msg));
   return true;
}

/* ---- Game menu and the screens it opens ------------------------------- */

static void grid_free_art(void)
{
   size_t i;
   for (i = 0; i < grid_st.art_count && grid_st.art_tex; i++)
      if (grid_st.art_tex[i])
         video_driver_texture_unload(&grid_st.art_tex[i]);
   free(grid_st.art_tex);
   free(grid_st.art_tried);
   grid_st.art_tex   = NULL;
   grid_st.art_tried = NULL;
   grid_st.art_count = 0;
}

static void grid_free_badges(void)
{
   unsigned i;
   for (i = 0; i < GRID_MAX_CHEEVOS; i++)
   {
      if (grid_st.badge_tex[i])
         video_driver_texture_unload(&grid_st.badge_tex[i]);
      grid_st.badge_tex[i]   = 0;
      grid_st.badge_tried[i] = false;
   }
}

/* + on a game: its dropdown menu. */
static void grid_open_game_menu(void)
{
   if (grid_st.selection >= grid_st.view_count)
      return;
   grid_st.menu_game = grid_st.view[grid_st.selection];
   grid_st.panel     = PANEL_GAME_MENU;
   grid_st.panel_sel = 0;
}

static void grid_open_game_screen(unsigned item)
{
   grid_game_t *game;

   if (grid_st.menu_game >= grid_st.count)
      return;
   game              = &grid_st.games[grid_st.menu_game];
   grid_st.panel_sel = 0;

   if (item == GAME_MENU_SETTINGS)
   {
      grid_st.panel = PANEL_NONE;
      grid_open_game_settings(game);
   }
   else if (item == GAME_MENU_QUIT)
   {
      grid_st.panel = PANEL_NONE;
      if (grid_content_running())
         action_ok_close_content(NULL, NULL, MENU_SETTING_ACTION_CLOSE, 0, 0);
   }
   else if (item == GAME_MENU_CHEEVOS)
   {
      grid_free_badges();
      grid_cheevos_open(grid_meta_game_id(game->path));
      grid_st.panel = PANEL_CHEEVOS;
   }
   else
   {
      grid_free_art();
      grid_st.art_sel_set = false;
      grid_st.art_kind    = item == GAME_MENU_COVER
         ? GRID_CHOICE_COVER : GRID_CHOICE_HERO;
      grid_cover_choices_open(game->title, grid_st.art_kind);
      grid_st.panel = PANEL_ART;
   }
}

/* Case-insensitive ASCII compare. */
static int grid_strcmp_noncase(const char *a, const char *b)
{
   for (; *a && *b; a++, b++)
   {
      int ca = tolower((unsigned char)*a);
      int cb = tolower((unsigned char)*b);
      if (ca != cb)
         return ca - cb;
   }
   return (unsigned char)*a - (unsigned char)*b;
}

/* Orders list_view entries for the open screen; ties keep list order. */
static int grid_list_cmp(const void *pa, const void *pb)
{
   size_t ia = *(const size_t*)pa;
   size_t ib = *(const size_t*)pb;
   int r     = 0;

   if (grid_st.panel == PANEL_CHEEVOS)
   {
      const grid_cheevo_t *a = grid_cheevos_get(ia);
      const grid_cheevo_t *b = grid_cheevos_get(ib);
      if (grid_st.cheevo_sort == CHEEVO_SORT_POINTS && a->points != b->points)
         return a->points > b->points ? -1 : 1;
      if (grid_st.cheevo_sort == CHEEVO_SORT_RAREST && a->rarity != b->rarity)
         return a->rarity < b->rarity ? -1 : 1;
   }
   else if (grid_st.panel == PANEL_UPDATES)
   {
      const grid_update_t *a = grid_updates_get(ia);
      const grid_update_t *b = grid_updates_get(ib);
      /* Status: updates, then up to date, then installable (by name
       * within each). */
      static const int rank[GRID_SECTION_COUNT] = { 0, 2, 1 };
      if (     grid_st.dl_sort == DL_SORT_STATUS
            && a->section != b->section)
         return rank[a->section] < rank[b->section] ? -1 : 1;
      if (grid_st.dl_sort == DL_SORT_NEWEST && a->date != b->date)
         return a->date > b->date ? -1 : 1;
      r = grid_strcmp_noncase(a->name, b->name);
   }

   if (r)
      return r;
   return ia < ib ? -1 : ia > ib ? 1 : 0;
}

/* Rebuilds list_view for the achievements or Installs screen from its
 * filter and sort. */
static void grid_build_list_view(void)
{
   size_t i, n = 0;

   grid_st.list_count = 0;

   if (grid_st.panel == PANEL_CHEEVOS)
   {
      n = grid_cheevos_count();
      for (i = 0; i < n && grid_st.list_count < GRID_MAX_CHEEVOS; i++)
      {
         const grid_cheevo_t *c = grid_cheevos_get(i);
         if (     (grid_st.cheevo_filter == CHEEVO_FILTER_UNLOCKED && !c->unlocked)
               || (grid_st.cheevo_filter == CHEEVO_FILTER_LOCKED   &&  c->unlocked))
            continue;
         grid_st.list_view[grid_st.list_count++] = i;
      }
   }
   else if (grid_st.panel == PANEL_UPDATES)
   {
      const char *search = grid_dl_search_text();
      n = grid_updates_count();
      for (i = 0; i < n && grid_st.list_count < GRID_MAX_CHEEVOS; i++)
      {
         const grid_update_t *u = grid_updates_get(i);
         if (     (grid_st.dl_filter != DL_FILTER_ALL
                && (int)u->section != (int)grid_st.dl_filter - 1)
               || !grid_contains_noncase(u->search, search))
            continue;
         grid_st.list_view[grid_st.list_count++] = i;
      }
   }
   else
      return;

   qsort(grid_st.list_view, grid_st.list_count, sizeof(size_t),
         grid_list_cmp);

   /* Installs: an "Update all" card first while more than one update
    * waits (in All and Updates, not while searching). */
   if (     grid_st.panel == PANEL_UPDATES
         && (grid_st.dl_filter == DL_FILTER_ALL
          || grid_st.dl_filter == DL_FILTER_UPDATES)
         && grid_updates_available() > 1
         && string_is_empty(grid_dl_search_text())
         && grid_st.list_count < GRID_MAX_CHEEVOS)
   {
      memmove(grid_st.list_view + 1, grid_st.list_view,
            grid_st.list_count * sizeof(size_t));
      grid_st.list_view[0] = GRID_DL_UPDATE_ALL;
      grid_st.list_count++;
   }

   if (grid_st.panel_sel >= grid_st.list_count)
      grid_st.panel_sel = grid_st.list_count
         ? (unsigned)grid_st.list_count - 1 : 0;
}

/* L / R on the achievements or Installs screen: next filter / sort. */
static void grid_list_cycle(bool filter)
{
   if (grid_st.panel == PANEL_CHEEVOS)
   {
      if (filter)
         grid_st.cheevo_filter = (enum grid_cheevo_filter)
            ((grid_st.cheevo_filter + 1) % CHEEVO_FILTER_COUNT);
      else
         grid_st.cheevo_sort = (enum grid_cheevo_sort)
            ((grid_st.cheevo_sort + 1) % CHEEVO_SORT_COUNT);
   }
   else if (grid_st.panel == PANEL_UPDATES)
   {
      if (filter)
         grid_st.dl_filter = (enum grid_dl_filter)
            ((grid_st.dl_filter + 1) % DL_FILTER_COUNT);
      else
         grid_st.dl_sort = (enum grid_dl_sort)
            ((grid_st.dl_sort + 1) % DL_SORT_COUNT);
   }
   else
      return;

   grid_st.panel_sel = 0;
   grid_build_list_view();
}

/* Moves sel around a grid of n items in cols columns (row-major). */
static unsigned grid_move_in_grid(unsigned sel, size_t n, unsigned cols,
      enum menu_action action)
{
   switch (action)
   {
      case MENU_ACTION_LEFT:
         if (sel % cols > 0)
            sel--;
         break;
      case MENU_ACTION_RIGHT:
         if (sel % cols + 1 < cols && sel + 1 < n)
            sel++;
         break;
      case MENU_ACTION_UP:
         if (sel >= cols)
            sel -= cols;
         break;
      case MENU_ACTION_DOWN:
         if (sel + cols < n)
            sel += cols;
         else if (n && sel / cols < (n - 1) / cols)
            sel = (unsigned)n - 1;   /* partial last row */
         break;
      default:
         break;
   }
   return sel;
}

/* Picture picker for a profile, starting on its current picture. */
static void grid_open_picker(unsigned profile)
{
   size_t i;
   char path[PATH_MAX_LENGTH];
   const char *cur = grid_profile_icon(profile);

   grid_close_picker();
   grid_st.pick_target = profile;
   grid_st.pick_count  = grid_cover_icon_count();
   if (grid_st.pick_count)
   {
      grid_st.pick_tex   = (uintptr_t*)calloc(grid_st.pick_count,
            sizeof(uintptr_t));
      grid_st.pick_tried = (bool*)calloc(grid_st.pick_count, sizeof(bool));
      if (!grid_st.pick_tex || !grid_st.pick_tried)
         grid_close_picker();
   }

   grid_st.panel     = PANEL_ICONS;
   grid_st.panel_sel = 0;
   for (i = 0; !string_is_empty(cur) && i < grid_st.pick_count; i++)
      if (     grid_cover_icon_get(i, path, sizeof(path))
            && string_is_equal(path, cur))
      {
         grid_st.panel_sel = (unsigned)(i + 1);
         break;
      }
}

/* Exit held: quits once OK has stayed down on it for GRID_EXIT_HOLD;
 * letting go or moving off it first cancels. Called every frame, on the
 * main thread: quitting with a game open saves its state, which can't
 * be done from the video thread. */
/* X held on an Installs card: removes its core the same way (letting go,
 * or the selection or screen changing, first cancels). */
static void grid_dl_hold_iterate(void)
{
   if (!grid_st.dl_hold_start)
      return;

   if (     grid_st.panel != PANEL_UPDATES
         || grid_st.panel_sel >= grid_st.list_count
         || grid_st.list_view[grid_st.panel_sel] != grid_st.dl_hold_item
         || !(menu_driver_scan_held() || grid_st.mouse_left))
   {
      grid_st.dl_hold_start   = 0;
      grid_st.dl_hold_full_at = 0;
      return;
   }

   if (     grid_st.dl_hold_full_at
         && cpu_features_get_time_usec() - grid_st.dl_hold_full_at
            >= GRID_EXIT_SHOW)
   {
      grid_st.dl_hold_start   = 0;
      grid_st.dl_hold_full_at = 0;
      grid_st.dl_hold_blend   = 0.0f;
      grid_updates_remove(grid_st.dl_hold_item, grid_running_core());
   }
}

static void grid_input_mode_poll(void);

void grid_home_iterate(void)
{
   static bool was_running = false;
   bool running            = grid_content_running();

   /* A game closed from the menu over it: its play time and history
    * were written as it closed, after the menu opened. */
   if (was_running && !running)
      grid_update_recent();
   was_running = running;

   /* The loading screen is up: start its game. */
   if (grid_st.loading == LOADING_START)
   {
      grid_launch_now();
      return;
   }

   grid_input_mode_poll();
   grid_dl_hold_iterate();
   grid_mp_follow_host();

   if (!grid_st.exit_hold_start)
      return;

   /* (A click holds as OK does: the mouse button held down.) */
   if (     grid_st.panel != PANEL_PROFILES
         || grid_st.panel_sel != GRID_STRIP_EXIT
         || !(menu_driver_ok_held() || grid_st.mouse_left))
   {
      grid_st.exit_hold_start = 0;
      grid_st.exit_full_at    = 0;
      return;
   }

   /* Quit once the full ring has been drawn and seen a moment. */
   if (     grid_st.exit_full_at
         && cpu_features_get_time_usec() - grid_st.exit_full_at
            >= GRID_EXIT_SHOW)
   {
      grid_st.exit_hold_start = 0;
      grid_st.exit_full_at    = 0;
      command_event(CMD_EVENT_QUIT, NULL);
   }
}

static void grid_panel_action(enum menu_action action)
{
   unsigned sel;

   if (grid_st.panel == PANEL_SETUP)
   {
      grid_setup_action(action);
      return;
   }

   grid_build_list_view(); /* current order and count for list screens */
   sel = grid_st.panel_sel;

   if (grid_st.panel == PANEL_PROFILES)
   {
      unsigned n = grid_profile_items();
      bool is_profile = sel >= GRID_STRIP_FIRST
            && sel - GRID_STRIP_FIRST < grid_profile_count();
      unsigned p      = sel - GRID_STRIP_FIRST; /* when is_profile */

      switch (action)
      {
         case MENU_ACTION_LEFT:
            if (sel > 0)
               sel--;
            break;
         case MENU_ACTION_RIGHT:
            if (sel + 1 < n)
               sel++;
            break;
         case MENU_ACTION_OK:
            if (sel == GRID_STRIP_EXIT)
            {
               /* Quits only if held (see grid_exit_hold_update). */
               grid_st.exit_hold_start = cpu_features_get_time_usec();
               grid_st.exit_full_at    = 0;
               return;
            }
            else if (!is_profile)
            {
               /* Add: only while no game is running (it switches to it). */
               if (!grid_content_running())
                  grid_start_profile_name();
            }
            else if (grid_content_running() && p == grid_profile_current())
               grid_st.panel = PANEL_NONE; /* already on it: keep playing */
            else
            {
               /* Another profile: close the game first (its saves are
                * open in this profile's folders). */
               if (grid_content_running())
                  action_ok_close_content(NULL, NULL,
                        MENU_SETTING_ACTION_CLOSE, 0, 0);
               if (grid_profile_select(p))
                  grid_st.panel = PANEL_NONE;
            }
            break;
         /* The face buttons are labelled as on the player's controller:
          * "X" is the menu's scan action, "Y" its search action. */
         case MENU_ACTION_SCAN:   /* X : change picture */
            if (is_profile)
            {
               grid_open_picker(p);
               return;
            }
            break;
         case MENU_ACTION_SEARCH: /* Y : rename (empty name: remove) */
            if (is_profile)
               grid_start_profile_rename(p);
            break;
         case MENU_ACTION_DOWN:   /* the strip hangs off the top bar */
         case MENU_ACTION_CANCEL:
            grid_st.panel = PANEL_NONE;
            break;
         default:
            break;
      }
   }
   else if (grid_st.panel == PANEL_ICONS)
   {
      /* Grid of pick_cols columns: "none", then the icons. */
      switch (action)
      {
         case MENU_ACTION_OK:
         {
            char path[PATH_MAX_LENGTH];
            path[0] = '\0';
            if (sel > 0)
               grid_cover_icon_get(sel - 1, path, sizeof(path));
            grid_profile_set_icon(grid_st.pick_target, path);
         }
         /* fall through: back to the profiles */
         case MENU_ACTION_CANCEL:
            grid_close_picker();
            grid_st.panel     = PANEL_PROFILES;
            grid_st.panel_sel = grid_st.pick_target + GRID_STRIP_FIRST;
            return;
         default:
            sel = grid_move_in_grid(sel, grid_st.pick_count + 1,
                  grid_st.pick_cols ? grid_st.pick_cols : 1, action);
            break;
      }
   }
   else if (grid_st.panel == PANEL_GAME_MENU)
   {
      unsigned items[GAME_MENU_COUNT];
      unsigned n = grid_game_menu_items(items);

      switch (action)
      {
         /* Wrapping round: up from the top to the bottom and back. */
         case MENU_ACTION_UP:
            if (n)
               sel = sel > 0 ? sel - 1 : n - 1;
            break;
         case MENU_ACTION_DOWN:
            if (n)
               sel = sel + 1 < n ? sel + 1 : 0;
            break;
         case MENU_ACTION_OK:
            if (sel < n)
               grid_open_game_screen(items[sel]);
            return;
         case MENU_ACTION_CANCEL:
            grid_st.panel = PANEL_NONE;
            break;
         default:
            break;
      }
   }
   else if (grid_st.panel == PANEL_CHEEVOS)
   {
      if (action == MENU_ACTION_CANCEL)
      {
         /* Back to the game menu, on Achievements. */
         grid_free_badges();
         grid_st.panel     = PANEL_GAME_MENU;
         grid_st.panel_sel = grid_game_menu_row(GAME_MENU_CHEEVOS);
         return;
      }
      sel = grid_move_in_grid(sel, grid_st.list_count, GRID_CHEEVO_COLS,
            action);
   }
   else if (grid_st.panel == PANEL_ART)
   {
      /* While the chosen image downloads, wait. */
      if (grid_cover_choose_state() == GRID_CHOICE_APPLY_WORKING)
         return;

      if (action == MENU_ACTION_CANCEL)
      {
         grid_free_art();
         grid_st.panel     = PANEL_GAME_MENU;
         grid_st.panel_sel = grid_game_menu_row(
               grid_st.art_kind == GRID_CHOICE_COVER
               ? GAME_MENU_COVER : GAME_MENU_WALLPAPER);
         return;
      }
      if (action == MENU_ACTION_OK)
      {
         if (sel < grid_cover_choices_count())
            grid_cover_choose(sel);
         return;
      }
      sel = grid_move_in_grid(sel, grid_cover_choices_count(),
            grid_st.art_cols ? grid_st.art_cols : 1, action);
   }
   else if (grid_st.panel == PANEL_UPDATES)
   {
      switch (action)
      {
         case MENU_ACTION_OK:     /* A : update / install (or all) */
            if (sel >= grid_st.list_count)
               return;
            /* Switch keys / firmware: pick the file (the system's Open
             * dialog). */
            if (     grid_st.list_view[sel] != GRID_DL_UPDATE_ALL
                  && grid_updates_pick(grid_st.list_view[sel]))
            {
               const grid_update_t *u = grid_updates_get(
                     grid_st.list_view[sel]);
               if (u && u->state != GRID_UPDATE_WORKING)
                  grid_switch_browse(grid_updates_pick(grid_st.list_view[sel]),
                        grid_running_core());
               return;
            }
            if (grid_st.list_view[sel] == GRID_DL_UPDATE_ALL)
               grid_updates_install_all(grid_running_core());
            else
               grid_updates_install(grid_st.list_view[sel],
                     grid_running_core());
            return;
         case MENU_ACTION_SCAN:   /* X : remove, once held (see
                                   * grid_home_iterate); repeats while held
                                   * are ignored */
         {
            size_t item = sel < grid_st.list_count
               ? grid_st.list_view[sel] : GRID_DL_UPDATE_ALL;
            const grid_update_t *u = item != GRID_DL_UPDATE_ALL
               ? grid_updates_get(item) : NULL;

            if (!u || !u->removable || grid_st.dl_hold_start)
               return;
            grid_st.dl_hold_item    = item;
            grid_st.dl_hold_start   = cpu_features_get_time_usec();
            grid_st.dl_hold_full_at = 0;
            return;
         }
         case MENU_ACTION_SEARCH: /* Y : search */
            grid_dl_start_search();
            return;
         case MENU_ACTION_CANCEL: /* B : clear the search, else back */
            if (!string_is_empty(grid_st.dl_search))
            {
               grid_st.dl_search[0] = '\0';
               sel = 0;
            }
            else
               grid_st.panel = PANEL_NONE;
            break;
         default:
            sel = grid_move_in_grid(sel, grid_st.list_count, GRID_DL_COLS,
                  action);
            break;
      }
   }

   grid_st.panel_sel = sel;
}

static void grid_bar_action(enum menu_action action)
{
   switch (action)
   {
      case MENU_ACTION_LEFT:
         if (grid_st.bar_sel > 0)
            grid_st.bar_sel--;
         break;
      case MENU_ACTION_RIGHT:
         if (grid_st.bar_sel + 1 < BAR_COUNT)
            grid_st.bar_sel++;
         break;
      case MENU_ACTION_DOWN:
      case MENU_ACTION_CANCEL:
         grid_st.bar_focus = false;
         break;
      case MENU_ACTION_OK:
         switch (grid_st.bar_sel)
         {
            case BAR_PROFILE:
               grid_open_panel(PANEL_PROFILES);
               break;
            case BAR_SETTINGS:
               grid_open_settings();
               break;
            case BAR_SEARCH:
               grid_start_search();
               break;
            case BAR_UPDATES:
               grid_open_panel(PANEL_UPDATES);
               break;
         }
         break;
      default:
         break;
   }
}

/* Games fill the grid column by column: index i is at column i / ROWS,
 * row i % ROWS. Up from the top row goes to the icon bar. */
static void grid_grid_action(enum menu_action action)
{
   size_t sel  = grid_st.selection;
   size_t rows = grid_st.rows ? grid_st.rows : GRID_ROWS_DEFAULT;
   size_t n    = grid_st.view_count;

   switch (action)
   {
      case MENU_ACTION_LEFT:
         if (sel >= rows)
            sel -= rows;
         break;
      case MENU_ACTION_RIGHT:
         if (sel + rows < n)
            sel += rows;
         else if (n && sel / rows < (n - 1) / rows)
            sel = n - 1;   /* partial last column */
         break;
      case MENU_ACTION_UP:
         if (sel % rows > 0)
            sel--;
         else
            grid_st.bar_focus = true;
         break;
      case MENU_ACTION_DOWN:
         if (sel % rows + 1 < rows && sel + 1 < n)
            sel++;
         break;
      case MENU_ACTION_OK:
         if (sel < n && !grid_st.games[grid_st.view[sel]].pending)
         {
            grid_game_t *game = &grid_st.games[grid_st.view[sel]];
            if (grid_is_running(game))
               command_event(CMD_EVENT_RESUME, NULL);
            else
               grid_launch(game);
         }
         break;
      default:
         break;
   }

   grid_st.selection = sel;
}

static bool grid_entry_action(enum menu_action action, unsigned xmb_depth);

/* The sound for a button the grid acted on. (What it leaves to RetroArch
 * makes RetroArch's own.) */
static void grid_action_sound(enum menu_action action)
{
   switch (action)
   {
      case MENU_ACTION_UP:
      case MENU_ACTION_LEFT:
      case MENU_ACTION_SCROLL_UP:
         grid_sound(AUDIO_MIXER_SYSTEM_SLOT_UP);
         break;
      case MENU_ACTION_DOWN:
      case MENU_ACTION_RIGHT:
      case MENU_ACTION_SCROLL_DOWN:
         grid_sound(AUDIO_MIXER_SYSTEM_SLOT_DOWN);
         break;
      case MENU_ACTION_OK:
      case MENU_ACTION_SEARCH:
      case MENU_ACTION_SCAN:
         grid_sound(AUDIO_MIXER_SYSTEM_SLOT_OK);
         break;
      case MENU_ACTION_CANCEL:
         grid_sound(AUDIO_MIXER_SYSTEM_SLOT_CANCEL);
         break;
      default:
         break;
   }
}

bool grid_home_ok_on_press(void)
{
   return !grid_settings_shown();
}

/* Where the cursor is, to tell whether a move went anywhere. */
typedef struct
{
   size_t selection;
   unsigned bar_sel;
   unsigned panel_sel;
   unsigned cc_sel;
   int panel;
   int cc_level;
   bool bar_focus;
   bool settings;
} grid_cursor_t;

static void grid_cursor_get(grid_cursor_t *c)
{
   memset(c, 0, sizeof(*c));
   c->selection = grid_st.selection;
   c->bar_sel   = (unsigned)grid_st.bar_sel;
   c->panel_sel = (unsigned)grid_st.panel_sel;
   c->cc_sel    = (unsigned)grid_st.cc_sel;
   c->panel     = (int)grid_st.panel;
   c->cc_level  = (int)grid_st.cc_level;
   c->bar_focus = grid_st.bar_focus;
   c->settings  = grid_st.settings;
}

bool grid_home_entry_action(enum menu_action action, unsigned xmb_depth)
{
   bool quiet   = grid_st.forwarding || grid_st.loading != LOADING_NONE;
   bool move    = action == MENU_ACTION_UP   || action == MENU_ACTION_DOWN
               || action == MENU_ACTION_LEFT || action == MENU_ACTION_RIGHT;
   /* Left / right on a Consoles row change its value, not the cursor. */
   bool value   = (action == MENU_ACTION_LEFT || action == MENU_ACTION_RIGHT)
               && grid_st.settings && grid_st.cc_level >= 2;
   /* A hold (Exit, removing an install) already under way: the button's
    * repeats are no new press. */
   bool holding = (action == MENU_ACTION_OK   && grid_st.exit_hold_start)
               || (action == MENU_ACTION_SCAN && grid_st.dl_hold_start);
   bool updates_scan = action == MENU_ACTION_SCAN
               && grid_st.panel == PANEL_UPDATES && !grid_st.settings;
   grid_cursor_t before, after;
   bool done;

   grid_cursor_get(&before);
   done = grid_entry_action(action, xmb_depth);
   grid_cursor_get(&after);

   /* A move that went nowhere (an edge), or X on an install that can't be
    * removed: silent. */
   if (move && !value && !memcmp(&before, &after, sizeof(before)))
      quiet = true;
   if (holding || (updates_scan && !grid_st.dl_hold_start))
      quiet = true;

   if (done && !quiet)
      grid_action_sound(action);
   return done;
}

static bool grid_entry_action(enum menu_action action, unsigned xmb_depth)
{
   /* An action the grid passes on to RetroArch itself (see
    * grid_settings_forward). */
   if (grid_st.forwarding)
      return false;

   /* A game is starting: nothing to do on its loading screen. */
   if (grid_st.loading != LOADING_NONE)
      return true;

   /* A console control just bound: keep it. */
   grid_cc_bind_poll();

   /* A change to the controller (e.g. a button just bound) applies to
    * every player. */
   grid_settings_sync_controllers();

   if (grid_st.settings)
   {
      if (grid_settings_shown())
         return grid_settings_action(action);

      /* The Quick Menu went with its game: carry on in the grid. */
      grid_st.settings       = false;
      grid_st.settings_quick = false;
      grid_st.settings_base  = 0;
      grid_st.game_settings  = false;
      grid_st.kb             = KB_SEARCH;
   }

   /* An open panel, or the icon bar, takes the D-pad and A/B (and X/Y in
    * the profiles panel). */
   if (grid_st.panel != PANEL_NONE)
   {
      switch (action)
      {
         case MENU_ACTION_LEFT:
         case MENU_ACTION_RIGHT:
         case MENU_ACTION_UP:
         case MENU_ACTION_DOWN:
         case MENU_ACTION_OK:
         case MENU_ACTION_CANCEL:
         case MENU_ACTION_SCAN:
         case MENU_ACTION_SEARCH:
            grid_panel_action(action);
            return true;
         case MENU_ACTION_SCROLL_UP:   /* L : the screen's filter */
            grid_build_list_view();
            grid_list_cycle(true);
            return true;
         case MENU_ACTION_SCROLL_DOWN: /* R : the screen's sort */
            grid_build_list_view();
            grid_list_cycle(false);
            return true;
         case MENU_ACTION_TOGGLE:
            break; /* menu toggle still works */
         default:
            return true;
      }
   }
   else if (grid_st.bar_focus)
   {
      switch (action)
      {
         case MENU_ACTION_LEFT:
         case MENU_ACTION_RIGHT:
         case MENU_ACTION_UP:
         case MENU_ACTION_DOWN:
         case MENU_ACTION_OK:
         case MENU_ACTION_CANCEL:
            grid_bar_action(action);
            return true;
         default:
            break;
      }
   }

   switch (action)
   {
      case MENU_ACTION_LEFT:
      case MENU_ACTION_RIGHT:
      case MENU_ACTION_UP:
      case MENU_ACTION_DOWN:
      case MENU_ACTION_OK:
         grid_grid_action(action);
         break;
      case MENU_ACTION_SCROLL_UP:   /* L : next console */
         grid_next_console();
         break;
      case MENU_ACTION_SCROLL_DOWN: /* R : next sort order */
         grid_next_sort();
         break;
      /* Face buttons as labelled on the player's controller: the menu's
       * "search" is Y (top), its "scan" X (left). Select and Start do
       * nothing here. */
      case MENU_ACTION_SEARCH:      /* Y : zoom */
         grid_zoom();
         break;
      case MENU_ACTION_SCAN:        /* X : the selected game's menu (quit
                                     * the running game is in it) */
         if (     grid_st.selection < grid_st.view_count
               && !grid_st.games[grid_st.view[grid_st.selection]].pending)
         {
            grid_st.bar_focus = false;
            grid_open_game_menu();
         }
         break;
      case MENU_ACTION_START:
         break;
      case MENU_ACTION_CANCEL:
         if (!string_is_empty(grid_st.search))
         {
            grid_st.search[0] = '\0';
            grid_reset_selection();
         }
         else if (grid_content_running())
            command_event(CMD_EVENT_RESUME, NULL);
         break;
      case MENU_ACTION_INFO:        /* Select (the menu calls it "info") */
      case MENU_ACTION_SELECT:
         break;
      default:
         /* Leave menu toggle, pointer etc. to XMB. */
         return false;
   }

   return true;
}

/* ---------------------------------------------------------------------- */
/* Mouse and keyboard                                                     */
/* ---------------------------------------------------------------------- */

static bool grid_binding(void);

bool grid_home_kbm(void)
{
   return grid_st.kbm;
}

/* Escape never quits (it is Back in the menu): RetroArch's default quit
 * key is unbound wherever it is still Escape. */
static void grid_unbind_escape_quit(void)
{
   if (input_config_binds[0][RARCH_QUIT_KEY].key == RETROK_ESCAPE)
      input_config_binds[0][RARCH_QUIT_KEY].key = RETROK_UNKNOWN;
}

static void grid_set_kbm(bool kbm)
{
   grid_st.kbm          = kbm;
   grid_st.mouse_rest_x = grid_st.mouse_x;
   grid_st.mouse_rest_y = grid_st.mouse_y;
}

/* Gamepad mode once a controller button is pressed or a stick pushed
 * (something newly down, so one held or resting off centre doesn't keep
 * switching back). Also shows the pointer in mouse and keyboard mode
 * only. */
static void grid_input_mode_poll(void)
{
   static input_bits_t prev[MAX_USERS];
   static uint8_t prev_axes[MAX_USERS];
   input_driver_state_t *input_st      = input_state_get_ptr();
   const input_device_driver_t *joypad = input_st->primary_joypad;
   video_driver_state_t *video_st      = video_state_get_ptr();
   settings_t *settings                = config_get_ptr();
   unsigned users                      = settings->uints.input_max_users;
   bool pressed                        = false;
   unsigned u, k;

   if (users > MAX_USERS)
      users = MAX_USERS;

   grid_unbind_escape_quit();

   for (u = 0; joypad && u < users; u++)
   {
      unsigned port = settings->uints.input_joypad_index[u];
      input_bits_t bits;
      uint8_t axes  = 0;

      BIT256_CLEAR_ALL(bits);
      /* (xinput's driver reads an unplugged pad out of bounds.) */
      if (joypad->query_pad && !joypad->query_pad(port))
         continue;
      if (joypad->get_buttons)
         joypad->get_buttons(port, &bits);
      else if (joypad->button)
      {
         /* Drivers without get_buttons (xinput): buttons one by one, and
          * the D-pad as hat 0. */
         static const uint16_t hats[4] = {
            HAT_UP_MASK, HAT_DOWN_MASK, HAT_LEFT_MASK, HAT_RIGHT_MASK
         };
         for (k = 0; k < 32; k++)
            if (joypad->button(port, (uint16_t)k))
               BIT256_SET(bits, k);
         for (k = 0; k < 4; k++)
            if (joypad->button(port, HAT_MAP(0, hats[k])))
               BIT256_SET(bits, 32 + k);
      }
      for (k = 0; joypad->axis && k < 4; k++)
      {
         if (abs(joypad->axis(port, AXIS_POS(k))) > 0x4000)
            axes |= 1 << (2 * k);
         if (abs(joypad->axis(port, AXIS_NEG(k))) > 0x4000)
            axes |= 1 << (2 * k + 1);
      }

      if (grid_st.pad_active)
      {
         for (k = 0; k < ARRAY_SIZE(bits.data); k++)
            if (bits.data[k] & ~prev[u].data[k])
               pressed = true;
         if (axes & ~prev_axes[u])
            pressed = true;
      }
      prev[u]      = bits;
      prev_axes[u] = axes;
   }
   /* (The first poll since the menu opened only takes note.) */
   if (!grid_st.pad_active)
   {
      grid_st.pad_active = true;
      pressed            = false;
   }
   if (pressed)
      grid_set_kbm(false);

   if (!menu_input_dialog_get_display_kb())
   {
      grid_st.edit_anchor = GRID_NO_ANCHOR;
      grid_st.edit_drag   = false;
      grid_st.edit_esc    = false;
      grid_st.osk_ready   = false;
   }

   if (     (!grid_st.cursor_known || grid_st.cursor_shown != grid_st.kbm)
         && video_st->poke && video_st->poke->show_mouse)
   {
      video_st->poke->show_mouse(video_st->data, grid_st.kbm);
      grid_st.cursor_shown = grid_st.kbm;
      grid_st.cursor_known = true;
   }
}

/* An action as if from the controller (through the menu driver, so
 * Settings' pages get it as RetroArch's). */
static void grid_mouse_dispatch(enum menu_action action)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   size_t sel                 = menu_st->selection_ptr;
   menu_entry_t entry;

   MENU_ENTRY_INITIALIZE(entry);
   menu_entry_get(&entry, 0, sel, NULL, false);
   menu_entry_action(&entry, sel, action);
}

/* Pointing at something selects it. */
static void grid_hit_select(const grid_hit_t *t)
{
   switch (t->kind)
   {
      case HIT_TILE:
         if (t->idx < grid_st.view_count)
         {
            grid_st.selection = t->idx;
            grid_st.bar_focus = false;
         }
         break;
      case HIT_BAR:
         grid_st.bar_focus = true;
         grid_st.bar_sel   = t->idx;
         break;
      case HIT_PANEL:
         grid_st.panel_sel = t->idx;
         break;
      case HIT_MENU:
      {
         struct menu_state *menu_st = menu_state_get_ptr();
         file_list_t *list = MENU_LIST_GET_SELECTION(menu_st->entries.list, 0);
         if (list && t->idx < list->size)
            menu_st->selection_ptr = t->idx;
         break;
      }
      case HIT_CC:
         grid_st.cc_sel = t->idx;
         break;
      default:
         break;
   }
}

/* Clicking it does its action. */
static void grid_hit_activate(const grid_hit_t *t)
{
   if (t->kind == HIT_CAT)
   {
      grid_sound(AUDIO_MIXER_SYSTEM_SLOT_OK);
      /* A game's Settings switch tab at their top only (as L / R). */
      if (grid_st.game_settings)
      {
         if (grid_st.cc_level == 2)
         {
            grid_st.cc_sub = (int)t->idx;
            grid_st.cc_sel = 0;
         }
      }
      else if (!grid_st.settings_quick)
         grid_settings_open_cat(t->idx);
      return;
   }
   if (t->action != MENU_ACTION_NOOP)
      grid_mouse_dispatch(t->action);
}

/* The wheel moves the selection: sideways along the games grid, the icon
 * bar and the profiles strip, up and down elsewhere. */
static enum menu_action grid_wheel_action(bool down)
{
   if (     !grid_settings_shown()
         && (grid_st.panel == PANEL_NONE || grid_st.panel == PANEL_PROFILES))
      return down ? MENU_ACTION_RIGHT : MENU_ACTION_LEFT;
   return down ? MENU_ACTION_DOWN : MENU_ACTION_UP;
}

static input_keyboard_line_t *grid_edit_line(void)
{
   return &input_state_get_ptr()->keyboard_line;
}

static size_t grid_utf8_prev(const char *s, size_t i)
{
   if (!i)
      return 0;
   for (i--; i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80; i--);
   return i;
}

static size_t grid_utf8_next(const char *s, size_t i, size_t len)
{
   if (i >= len)
      return len;
   for (i++; i < len && ((unsigned char)s[i] & 0xc0) == 0x80; i++);
   return i;
}

/* The selected bytes of the line [a, b), if any. */
static bool grid_edit_sel(size_t *a, size_t *b)
{
   input_keyboard_line_t *line = grid_edit_line();
   size_t anchor               = grid_st.edit_anchor;

   if (     anchor == GRID_NO_ANCHOR || anchor > line->size
         || anchor == line->ptr || !line->buffer)
      return false;
   *a = anchor < line->ptr ? anchor : line->ptr;
   *b = anchor < line->ptr ? line->ptr : anchor;
   return true;
}

static void grid_edit_erase(size_t a, size_t b)
{
   input_keyboard_line_t *line = grid_edit_line();

   if (!line->buffer || b <= a || b > line->size)
      return;
   memmove(line->buffer + a, line->buffer + b, line->size - b + 1);
   line->size -= b - a;
   line->ptr   = a;
}

/* Removes the selection (true if there was one); either way none is
 * left. */
static bool grid_edit_erase_sel(void)
{
   size_t a, b;
   bool had = grid_edit_sel(&a, &b);

   if (had)
      grid_edit_erase(a, b);
   grid_st.edit_anchor = GRID_NO_ANCHOR;
   return had;
}

static void grid_edit_delete(bool forward)
{
   input_keyboard_line_t *line = grid_edit_line();

   if (grid_edit_erase_sel() || !line->buffer)
      return;
   if (forward)
      grid_edit_erase(line->ptr,
            grid_utf8_next(line->buffer, line->ptr, line->size));
   else if (line->ptr)
      grid_edit_erase(grid_utf8_prev(line->buffer, line->ptr), line->ptr);
}

/* Left / right a character (ctrl: a word), Home / End; shift extends the
 * selection. */
static void grid_edit_move(unsigned code, bool shift, bool ctrl)
{
   input_keyboard_line_t *line = grid_edit_line();
   const char *s = line->buffer ? line->buffer : "";
   size_t len    = line->buffer ? line->size : 0;
   size_t p      = line->ptr, a, b;

   /* Without shift, left / right go to that end of the selection. */
   if (     !shift && (code == RETROK_LEFT || code == RETROK_RIGHT)
         && grid_edit_sel(&a, &b))
   {
      line->ptr           = code == RETROK_LEFT ? a : b;
      grid_st.edit_anchor = GRID_NO_ANCHOR;
      return;
   }
   if (shift && (grid_st.edit_anchor == GRID_NO_ANCHOR
            || grid_st.edit_anchor > len))
      grid_st.edit_anchor = p;

   switch (code)
   {
      case RETROK_LEFT:
         if (ctrl)
         {
            while (p && s[p - 1] == ' ')
               p--;
            while (p && s[p - 1] != ' ')
               p--;
         }
         else
            p = grid_utf8_prev(s, p);
         break;
      case RETROK_RIGHT:
         if (ctrl)
         {
            while (p < len && s[p] != ' ')
               p++;
            while (p < len && s[p] == ' ')
               p++;
         }
         else
            p = grid_utf8_next(s, p, len);
         break;
      case RETROK_HOME:
         p = 0;
         break;
      default:
         p = len;
         break;
   }

   line->ptr = p;
   if (!shift)
      grid_st.edit_anchor = GRID_NO_ANCHOR;
}

/* Copies (or cuts) the selection; not from the password field. */
static void grid_edit_copy(bool cut)
{
   input_keyboard_line_t *line = grid_edit_line();
   size_t a, b;

   if (     (grid_st.kb == KB_SETUP && grid_st.setup_typing == SETUP_PASS)
         || !grid_edit_sel(&a, &b))
      return;
   bomba_clipboard_set(line->buffer + a, b - a);
   if (cut)
      grid_edit_erase_sel();
}

/* Pastes over the selection, as one line (control characters such as
 * line breaks are left out). */
static void grid_edit_paste(void)
{
   char *clip = bomba_clipboard_get();
   char *src, *dst;

   if (!clip)
      return;
   for (src = dst = clip; *src; src++)
      if ((unsigned char)*src >= 0x20 && *src != 0x7f)
         *dst++ = *src;
   *dst = '\0';

   grid_edit_erase_sel();
   if (*clip)
      input_keyboard_line_append(grid_edit_line(), clip, strlen(clip));
   free(clip);
}

/* Escape: closes the text entry, keeping what was there before. */
static void grid_edit_cancel(void)
{
   input_driver_state_t *input_st = input_state_get_ptr();

   input_keyboard_line_free(input_st);
   input_st->flags &= ~INP_FLAG_KB_MAPPING_BLOCKED;
   menu_input_dialog_end();
   if (grid_st.kb != KB_MENU)
      grid_st.kb = KB_SEARCH;
}

/* The line's byte nearest the pointer, in the field as last drawn. */
static size_t grid_edit_index_at(float x)
{
   size_t i, best = 0;
   float best_d   = 1e9f;

   for (i = 0; i <= grid_hits.edit_len; i++)
   {
      float d;
      if (grid_hits.edit_xs[i] < 0.0f) /* inside a UTF-8 character */
         continue;
      d = fabsf(grid_hits.edit_x0 + grid_hits.edit_xs[i] - x);
      if (d < best_d)
      {
         best_d = d;
         best   = i;
      }
   }
   return best;
}

bool grid_home_mouse(int x, int y, unsigned flags)
{
   bool left    = (flags & MENU_INP_PTR_FLG_PRESS_SELECT) ? true : false;
   bool right   = (flags & MENU_INP_PTR_FLG_PRESS_CANCEL) ? true : false;
   bool press   = left  && !grid_st.mouse_left;
   bool rpress  = right && !grid_st.mouse_right;
   bool moved   = grid_st.mouse_seen
               && (x != grid_st.mouse_x || y != grid_st.mouse_y);
   int wheel    = (flags & MENU_INP_PTR_FLG_PRESS_UP) ? -1
                : (flags & MENU_INP_PTR_FLG_PRESS_DOWN) ? 1 : 0;
   const grid_hit_t *hit = NULL;
   unsigned i;

   /* A game is starting: nothing to point at on its loading screen. */
   if (grid_st.loading != LOADING_NONE)
      return true;

   if (!grid_st.mouse_seen)
   {
      grid_st.mouse_rest_x = x;
      grid_st.mouse_rest_y = y;
   }
   grid_st.mouse_seen  = true;
   grid_st.mouse_x     = x;
   grid_st.mouse_y     = y;
   grid_st.mouse_left  = left;
   grid_st.mouse_right = right;

   /* Gamepad mode until the mouse moves a little, clicks or scrolls. */
   if (!grid_st.kbm)
   {
      if (     !press && !rpress && !wheel
            && abs(x - grid_st.mouse_rest_x) <= 4
            && abs(y - grid_st.mouse_rest_y) <= 4)
         return true;
      grid_set_kbm(true);
   }

   /* RetroArch waiting for a control to bind: leave the screen as is. */
   if (grid_binding() || grid_cc_bind_waiting())
      return true;

   for (i = grid_hits.count; i-- > 0; )
   {
      const grid_hit_t *t = &grid_hits.hit[i];
      if (     x >= t->x && x < t->x + t->w
            && y >= t->y && y < t->y + t->h)
      {
         hit = t;
         break;
      }
   }

   /* Typing: a click in the field places the caret and dragging selects;
    * a click elsewhere is done typing (as Enter). */
   if (menu_input_dialog_get_display_kb())
   {
      input_keyboard_line_t *line = grid_edit_line();
      bool in_field = grid_hits.edit
            && x >= grid_hits.edit_x && x < grid_hits.edit_x + grid_hits.edit_w
            && y >= grid_hits.edit_y && y < grid_hits.edit_y + grid_hits.edit_h;

      if (!line->enabled)
         return true;
      if (press && in_field)
      {
         size_t p = grid_edit_index_at((float)x);
         line->ptr           = p < line->size ? p : line->size;
         grid_st.edit_anchor = line->ptr;
         grid_st.edit_drag   = true;
      }
      else if (press)
         input_keyboard_event(true, '\n', '\n', 0, RETRO_DEVICE_KEYBOARD);
      else if (left && moved && grid_st.edit_drag)
      {
         size_t p  = grid_edit_index_at((float)x);
         line->ptr = p < line->size ? p : line->size;
      }
      if (!left)
         grid_st.edit_drag = false;
      return true;
   }

   if (moved && hit && !left)
      grid_hit_select(hit);
   if (press && hit)
   {
      grid_hit_select(hit);
      grid_hit_activate(hit);
   }
   /* Right click: a game's menu on its tile, else back. */
   if (rpress)
   {
      if (hit && hit->kind == HIT_TILE)
      {
         grid_hit_select(hit);
         grid_mouse_dispatch(MENU_ACTION_SCAN);
      }
      else
         grid_mouse_dispatch(MENU_ACTION_CANCEL);
   }
   if (wheel)
      grid_mouse_dispatch(grid_wheel_action(wheel > 0));
   return true;
}

bool grid_home_key_event(bool down, unsigned code, uint32_t character,
      uint16_t mod)
{
   input_driver_state_t *input_st = input_state_get_ptr();
   input_keyboard_line_t *line    = &input_st->keyboard_line;
   bool ctrl = (mod & RETROKMOD_CTRL) && !(mod & RETROKMOD_ALT); /* not AltGr */

   /* The on-screen keyboard's own keys (Backspace, Enter) come as code ==
    * character: those are the controller's. */
   if (code && code == character)
      return false;

   if (down)
      grid_set_kbm(true);

   if (     !menu_input_dialog_get_display_kb() || !line->enabled
         || input_st->keyboard_press_cb)
      return false;

   if (!down)
   {
      if (code == RETROK_ESCAPE && grid_st.edit_esc)
      {
         grid_st.edit_esc = false;
         grid_edit_cancel();
         return true;
      }
      return false;
   }

   /* A key (its character, if any, comes as an event of its own). */
   if (code != RETROK_UNKNOWN)
   {
      switch (code)
      {
         case RETROK_LEFT:
         case RETROK_RIGHT:
         case RETROK_HOME:
         case RETROK_END:
            grid_edit_move(code, (mod & RETROKMOD_SHIFT) ? true : false,
                  ctrl);
            return true;
         case RETROK_DELETE:
            grid_edit_delete(true);
            return true;
         case RETROK_ESCAPE:
            grid_st.edit_esc = true;
            return true;
         case RETROK_UP:
         case RETROK_DOWN:
            return true;
         case RETROK_a:
            if (!ctrl)
               break;
            grid_st.edit_anchor = 0;
            line->ptr           = line->size;
            return true;
         case RETROK_c:
         case RETROK_x:
            if (!ctrl)
               break;
            grid_edit_copy(code == RETROK_x);
            return true;
         case RETROK_v:
            if (!ctrl)
               break;
            grid_edit_paste();
            return true;
         default:
            break;
      }
      return false;
   }

   /* Input method composition: RetroArch's. */
   if (character & 0xff000000)
      return false;

   switch (character)
   {
      case '\b':
      case 0x7f:
         grid_edit_delete(false);
         return true;
      case '\r':
      case '\n':
         grid_st.edit_anchor = GRID_NO_ANCHOR;
         return false;
      default:
         break;
   }
   /* Other control characters (Ctrl+letter, Escape, Tab) type nothing. */
   if (character < 0x20)
      return true;

   /* Typing replaces the selection. RetroArch types plain ASCII; anything
    * else goes in as UTF-8 here (it would type "?"). */
   grid_edit_erase_sel();
   if (character < 0x80)
      return false;
   if (character < 0xd800 || (character >= 0xe000 && character < 0x10000))
   {
      char utf8[4];
      size_t n;
      if (character < 0x800)
      {
         utf8[0] = (char)(0xc0 | (character >> 6));
         utf8[1] = (char)(0x80 | (character & 0x3f));
         n       = 2;
      }
      else
      {
         utf8[0] = (char)(0xe0 | (character >> 12));
         utf8[1] = (char)(0x80 | ((character >> 6) & 0x3f));
         utf8[2] = (char)(0x80 | (character & 0x3f));
         n       = 3;
      }
      input_keyboard_line_append(line, utf8, n);
   }
   return true;
}

/* The on-screen keyboard: four rows of eleven character keys over a row
 * of the layout, shift, symbols and space keys, and a column on the right
 * of backspace, cancel and OK:
 *
 *    1 2 3 4 5 6 7 8 9 0 +   [ Bksp ]
 *    q w e r t y u i o p å   [Cancel]
 *    a s d f g h j k l ö ä   [      ]
 *    z x c v b n m , . - '   [  OK  ]
 *    Shift #+= [   Space   ] [      ]
 *
 * Keys are character indices (row * 11 + column) or these. */
enum
{
   OSK_BACKSPACE = 100,
   OSK_CANCEL,
   OSK_OK,
   OSK_SHIFT,
   OSK_SYMBOLS,
   OSK_SPACE
};

#define GRID_OSK_COLS 12 /* the character columns and the right one */
#define GRID_OSK_ROWS 5

/* The symbols page, in place of the character keys. */
static const char *grid_osk_symbols[BOMBA_KEYMAP_KEYS] = {
   "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "_",
   "[", "]", "{", "}", "<", ">", "\\", "|", "~", "`", "=",
   "\xe2\x82\xac", "\xc2\xa3", "\xc2\xa5", "\xc2\xa7", "\xc2\xb0",
   "\xc2\xbf", "\xc2\xa1", "\xc2\xab", "\xc2\xbb", "\xc2\xb4", "\xc2\xa8",
   "+", "-", "*", "/", ":", ";", "\"", "'", ",", ".", "?"
};

static int grid_osk_key_at(int row, int col)
{
   if (col == GRID_OSK_COLS - 1)
      return row == 0 ? OSK_BACKSPACE : row <= 2 ? OSK_CANCEL : OSK_OK;
   if (row < BOMBA_KEYMAP_ROWS)
      return row * BOMBA_KEYMAP_COLS + col;
   return col == 0 ? OSK_SHIFT : col == 1 ? OSK_SYMBOLS : OSK_SPACE;
}

/* A character key's text as it types now (page and shift). */
static const char *grid_osk_label(int id)
{
   if (id < 0 || id >= BOMBA_KEYMAP_KEYS)
      return "";
   if (grid_st.osk_symbols)
      return grid_osk_symbols[id];
   /* Shift leaves the number row as it is. */
   return grid_st.osk_shift && id >= BOMBA_KEYMAP_COLS
      ? grid_st.osk_keys[id].shift : grid_st.osk_keys[id].base;
}

/* Typing starts: the system's current layout, on the q key. */
static void grid_osk_prepare(void)
{
   if (grid_st.osk_ready)
      return;
   grid_st.osk_ready   = true;
   grid_st.osk_layout  = bomba_keymap_current();
   bomba_keymap_get(grid_st.osk_layout, grid_st.osk_keys);
   grid_st.osk_row     = 1;
   grid_st.osk_col     = 0;
   grid_st.osk_want    = 0;
   grid_st.osk_shift   = false;
   grid_st.osk_symbols = false;
   grid_st.osk_rep_dir = -1;
}

/* One step that way, past the rest of a wide key; sideways wraps round. */
static void grid_osk_step(unsigned dir)
{
   int row = grid_st.osk_row, col = grid_st.osk_col;
   int id  = grid_osk_key_at(row, col);

   if (dir == RETRO_DEVICE_ID_JOYPAD_LEFT || dir == RETRO_DEVICE_ID_JOYPAD_RIGHT)
   {
      int dc = dir == RETRO_DEVICE_ID_JOYPAD_LEFT ? -1 : 1;
      do
         col = (col + dc + GRID_OSK_COLS) % GRID_OSK_COLS;
      while (grid_osk_key_at(row, col) == id);
      grid_st.osk_col  = col;
      grid_st.osk_want = col;
      return;
   }

   col = grid_st.osk_want;
   do
   {
      row += dir == RETRO_DEVICE_ID_JOYPAD_UP ? -1 : 1;
      if (row < 0 || row >= GRID_OSK_ROWS)
         return;
   } while (grid_osk_key_at(row, col) == id);
   grid_st.osk_row = row;
   grid_st.osk_col = col;
}

static void grid_osk_type(const char *s)
{
   grid_edit_erase_sel();
   if (*s)
      input_keyboard_line_append(grid_edit_line(), s, strlen(s));
}

static void grid_osk_press(int id)
{
   switch (id)
   {
      case OSK_BACKSPACE:
         grid_edit_delete(false);
         break;
      case OSK_CANCEL:
         grid_edit_cancel();
         break;
      case OSK_OK:
         input_keyboard_event(true, '\n', '\n', 0, RETRO_DEVICE_KEYBOARD);
         break;
      case OSK_SHIFT:
         grid_st.osk_shift = !grid_st.osk_shift;
         break;
      case OSK_SYMBOLS:
         grid_st.osk_symbols = !grid_st.osk_symbols;
         break;
      case OSK_SPACE:
         grid_osk_type(" ");
         break;
      default:
         grid_osk_type(grid_osk_label(id));
         break;
   }
}

/* Controller: the D-pad moves (repeating while held), A types the key, B
 * erases (closes when empty), X cancels, Y types a space, Start is done,
 * L / R move the caret, the left stick's click shifts, the right's
 * switches to symbols. */
static void grid_osk_step_sound(unsigned d)
{
   grid_sound((d == RETRO_DEVICE_ID_JOYPAD_UP || d == RETRO_DEVICE_ID_JOYPAD_LEFT)
         ? AUDIO_MIXER_SYSTEM_SLOT_UP : AUDIO_MIXER_SYSTEM_SLOT_DOWN);
}

bool grid_home_osk_input(const input_bits_t *held,
      const input_bits_t *pressed)
{
   static const unsigned dirs[4] = {
      RETRO_DEVICE_ID_JOYPAD_UP,   RETRO_DEVICE_ID_JOYPAD_DOWN,
      RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT
   };
   retro_time_t now = cpu_features_get_time_usec();
   bool swap        = config_get_ptr()->bools.input_menu_swap_ok_cancel_buttons;
   unsigned ok_btn  = swap ? RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_A;
   unsigned del_btn = swap ? RETRO_DEVICE_ID_JOYPAD_A : RETRO_DEVICE_ID_JOYPAD_B;
   bool any_dir     = false;
   unsigned k;

   /* Mouse and keyboard mode shows no keyboard (a button press switches
    * to gamepad mode, showing it). */
   if (grid_st.kbm || !grid_edit_line()->enabled)
      return true;
   grid_osk_prepare();

   for (k = 0; k < 4; k++)
   {
      unsigned d = dirs[k];
      if (BIT256_GET_PTR(pressed, d))
      {
         grid_osk_step(d);
         grid_osk_step_sound(d);
         grid_st.osk_rep_dir = (int)d;
         grid_st.osk_rep_at  = now + 400000;
      }
      else if ((int)d == grid_st.osk_rep_dir && BIT256_GET_PTR(held, d)
            && now >= grid_st.osk_rep_at)
      {
         grid_osk_step(d);
         grid_osk_step_sound(d);
         grid_st.osk_rep_at = now + 70000;
      }
      if (BIT256_GET_PTR(held, d))
         any_dir = true;
   }
   if (!any_dir)
      grid_st.osk_rep_dir = -1;

   /* (Done or cancelled: the line is gone, nothing more to do.) */
   if (BIT256_GET_PTR(pressed, ok_btn))
   {
      grid_sound(AUDIO_MIXER_SYSTEM_SLOT_OK);
      grid_osk_press(grid_osk_key_at(grid_st.osk_row, grid_st.osk_col));
      return true;
   }
   if (BIT256_GET_PTR(pressed, del_btn))
   {
      grid_sound(AUDIO_MIXER_SYSTEM_SLOT_CANCEL);
      if (grid_edit_line()->size)
         grid_edit_delete(false);
      else
         grid_edit_cancel();
      return true;
   }
   /* (RetroPad Y: the menu's scan, whose glyph is "X"; RetroPad X its
    * search, "Y". See grid_glyph_button.) */
   if (BIT256_GET_PTR(pressed, RETRO_DEVICE_ID_JOYPAD_Y))
   {
      grid_sound(AUDIO_MIXER_SYSTEM_SLOT_CANCEL);
      grid_edit_cancel();
      return true;
   }
   if (BIT256_GET_PTR(pressed, RETRO_DEVICE_ID_JOYPAD_START))
   {
      grid_sound(AUDIO_MIXER_SYSTEM_SLOT_OK);
      input_keyboard_event(true, '\n', '\n', 0, RETRO_DEVICE_KEYBOARD);
      return true;
   }
   if (BIT256_GET_PTR(pressed, RETRO_DEVICE_ID_JOYPAD_X))
   {
      grid_sound(AUDIO_MIXER_SYSTEM_SLOT_OK);
      grid_osk_type(" ");
   }
   if (BIT256_GET_PTR(pressed, RETRO_DEVICE_ID_JOYPAD_L))
   {
      grid_sound(AUDIO_MIXER_SYSTEM_SLOT_UP);
      grid_edit_move(RETROK_LEFT, false, false);
   }
   if (BIT256_GET_PTR(pressed, RETRO_DEVICE_ID_JOYPAD_R))
   {
      grid_sound(AUDIO_MIXER_SYSTEM_SLOT_DOWN);
      grid_edit_move(RETROK_RIGHT, false, false);
   }
   if (BIT256_GET_PTR(pressed, RETRO_DEVICE_ID_JOYPAD_L3))
   {
      grid_sound(AUDIO_MIXER_SYSTEM_SLOT_OK);
      grid_osk_press(OSK_SHIFT);
   }
   if (BIT256_GET_PTR(pressed, RETRO_DEVICE_ID_JOYPAD_R3))
   {
      grid_sound(AUDIO_MIXER_SYSTEM_SLOT_OK);
      grid_osk_press(OSK_SYMBOLS);
   }
   return true;
}

/* ---------------------------------------------------------------------- */
/* Drawing primitives                                                     */
/* ---------------------------------------------------------------------- */

static void grid_create_disc(void)
{
   struct texture_image ti;
   const unsigned size = 64;
   const float r       = size / 2.0f;
   unsigned x, y;

   ti.width         = size;
   ti.height        = size;
   ti.supports_rgba = video_driver_supports_rgba();
   ti.pixels        = (uint32_t*)malloc(size * size * sizeof(uint32_t));
   if (!ti.pixels)
      return;

   /* White, with alpha falling off over one pixel at the edge.
    * White is the same in RGBA and BGRA, so only alpha matters. */
   for (y = 0; y < size; y++)
   {
      for (x = 0; x < size; x++)
      {
         float dx = x + 0.5f - r;
         float dy = y + 0.5f - r;
         float a  = r - sqrtf(dx * dx + dy * dy) + 0.5f;
         if (a < 0.0f) a = 0.0f;
         if (a > 1.0f) a = 1.0f;
         ti.pixels[y * size + x] = ((uint32_t)(a * 255.0f) << 24) | 0x00ffffff;
      }
   }

   /* Mipmaps keep edges smooth when drawn much smaller than 64px
    * (button glyphs, small corners). */
   video_driver_texture_load(&ti, TEXTURE_FILTER_MIPMAP_LINEAR, &grid_st.disc);
   free(ti.pixels);
}

static void grid_draw_rect(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h, uint32_t rgba)
{
   float color[16];
   grid_set_color(color, rgba);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)x, (int)y, (unsigned)w, (unsigned)h,
         width, height, color, NULL);
}

/* Opaque rounded rectangle: three strips plus four corner circles.
 * All edges are snapped to whole pixels first so the pieces meet
 * exactly (no seams). */
static void grid_draw_rounded(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h, float r, uint32_t rgba)
{
   float color[16];
   int x0 = (int)floorf(x + 0.5f);
   int y0 = (int)floorf(y + 0.5f);
   int x1 = (int)floorf(x + w + 0.5f);
   int y1 = (int)floorf(y + h + 0.5f);
   int ri = (int)floorf(r + 0.5f);
   int d;

   if (ri * 2 > x1 - x0) ri = (x1 - x0) / 2;
   if (ri * 2 > y1 - y0) ri = (y1 - y0) / 2;
   d = ri * 2;

   if (!grid_st.disc || ri < 1)
   {
      grid_draw_rect(p_disp, userdata, width, height,
            x0, y0, x1 - x0, y1 - y0, rgba);
      return;
   }

   grid_set_color(color, rgba);

   /* Centre column full height, side columns between the corners. */
   gfx_display_draw_quad(p_disp, userdata, width, height,
         x0 + ri, y0, (unsigned)(x1 - x0 - d), (unsigned)(y1 - y0),
         width, height, color, NULL);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         x0, y0 + ri, (unsigned)ri, (unsigned)(y1 - y0 - d),
         width, height, color, NULL);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         x1 - ri, y0 + ri, (unsigned)ri, (unsigned)(y1 - y0 - d),
         width, height, color, NULL);

   gfx_display_draw_quad(p_disp, userdata, width, height,
         x0, y0, (unsigned)d, (unsigned)d,
         width, height, color, &grid_st.disc);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         x1 - d, y0, (unsigned)d, (unsigned)d,
         width, height, color, &grid_st.disc);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         x0, y1 - d, (unsigned)d, (unsigned)d,
         width, height, color, &grid_st.disc);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         x1 - d, y1 - d, (unsigned)d, (unsigned)d,
         width, height, color, &grid_st.disc);
}

/* Crops an image in place to the given width/height ratio, keeping the
 * centre. Rows only move towards the start of the buffer, so it can be
 * done in place. */
static void grid_crop_to_aspect(struct texture_image *ti, float aspect)
{
   unsigned w = ti->width, h = ti->height, x0 = 0, y0 = 0, y;

   if (!w || !h)
      return;

   if ((float)w / h > aspect)
   {
      w  = (unsigned)(h * aspect + 0.5f);
      x0 = (ti->width - w) / 2;
   }
   else
   {
      h  = (unsigned)(w / aspect + 0.5f);
      y0 = (ti->height - h) / 2;
   }

   if (w == ti->width && h == ti->height)
      return;

   for (y = 0; y < h; y++)
      memmove(&ti->pixels[y * w],
            &ti->pixels[(y + y0) * ti->width + x0], w * sizeof(uint32_t));

   ti->width  = w;
   ti->height = h;
}

/* Loads an image with its corners rounded in the alpha channel (radius as
 * a fraction of the image width), so covers look rounded over any
 * background. If aspect > 0, the image is first cropped to that
 * width/height ratio. */
static bool grid_load_rounded_texture(const char *path, uintptr_t *tex,
      unsigned *w, unsigned *h, float radius_frac, float aspect)
{
   struct texture_image ti;
   unsigned x, y;
   float r;

   ti.width         = 0;
   ti.height        = 0;
   ti.pixels        = NULL;
   ti.supports_rgba = video_driver_supports_rgba();

   if (string_is_empty(path) || !image_texture_load(&ti, path))
      return false;

   if (aspect > 0.0f)
      grid_crop_to_aspect(&ti, aspect);

   r = ti.width * radius_frac;

   /* Alpha is the top byte in both ARGB and ABGR layouts. Only pixels in
    * the four corner squares can be affected. */
   for (y = 0; y < ti.height; y++)
   {
      float cy = y + 0.5f;
      float dy = cy < r ? r - cy
               : cy > ti.height - r ? cy - (ti.height - r) : 0.0f;
      if (dy <= 0.0f)
         continue;

      for (x = 0; x < ti.width; x++)
      {
         float cx = x + 0.5f;
         float dx = cx < r ? r - cx
                  : cx > ti.width - r ? cx - (ti.width - r) : 0.0f;
         float cover;
         uint32_t *p, a;

         if (dx <= 0.0f)
            continue;

         /* Coverage of the rounded corner, anti-aliased over a pixel. */
         cover = r - sqrtf(dx * dx + dy * dy) + 0.5f;
         if (cover >= 1.0f)
            continue;
         if (cover < 0.0f)
            cover = 0.0f;

         p  = &ti.pixels[y * ti.width + x];
         a  = (uint32_t)(((*p >> 24) & 0xff) * cover);
         *p = (*p & 0x00ffffff) | (a << 24);
      }
   }

   *w = ti.width;
   *h = ti.height;
   video_driver_texture_load(&ti, TEXTURE_FILTER_MIPMAP_LINEAR, tex);
   image_texture_free(&ti);
   return true;
}

/* Scroll indicator for a list screen, in the right margin beside its area
 * (top, h): a faint track, and a thumb as tall as the share of the list
 * on screen, as far down as the list is scrolled (first of total rows,
 * visible at once). Nothing when it all fits. */
static void grid_draw_shape(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h, float r, uint32_t rgba);

static void grid_draw_scrollbar(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, float top, float h,
      size_t first, size_t visible, size_t total)
{
   float w = floorf(REF(3) + 0.5f);
   float x = floorf(width - REF(17) - w / 2.0f + 0.5f);
   float th, ty;

   if (total <= visible || !visible || h <= 0.0f)
      return;
   if (first > total - visible)
      first = total - visible;

   th = h * (float)visible / (float)total;
   if (th < REF(16))
      th = REF(16);
   ty = top + (h - th) * (float)first / (float)(total - visible);

   /* One-texture shapes: rounded pieces overlapping would stack their
    * translucency at the ends. */
   grid_draw_shape(p_disp, userdata, width, height,
         x, floorf(top + 0.5f), w, floorf(h + 0.5f), w / 2.0f, 0xffffff1c);
   grid_draw_shape(p_disp, userdata, width, height,
         x, floorf(ty + 0.5f), w, floorf(th + 0.5f), w / 2.0f, 0xb4b4b4ff);
}

/* Outline rings rasterised at their exact pixel size, so they are smooth
 * at any size (scaling a shape texture makes curves jagged). Kept in a
 * small cache: only a few sizes are on screen at once. */
#define GRID_RING_CACHE 16

static struct
{
   uintptr_t tex;
   int w, h, r, gap, thick;   /* shape, whole pixels */
   float nx, ny, nr;          /* notch (see grid_ring_texture) */
   unsigned tw, th;           /* texture size */
   unsigned last_used;
} grid_ring_cache[GRID_RING_CACHE];
static unsigned grid_ring_clock;

/* Divider capsule, rasterised at its exact size (see grid_draw_divider). */
static uintptr_t grid_divider_tex;
static unsigned  grid_divider_w, grid_divider_h;

/* Hold progress arc, rasterised at its exact size (see grid_arc_texture). */
static uintptr_t grid_arc_tex;
static int       grid_arc_key[4]; /* d, gap, thick, progress step */
/* ...and round a rounded rect (see grid_rect_arc_texture). */
static uintptr_t grid_rect_arc_tex;
static int       grid_rect_arc_key[6]; /* w, h, r, gap, thick, step */

static void grid_free_rings(void)
{
   unsigned i;
   for (i = 0; i < GRID_RING_CACHE; i++)
   {
      if (grid_ring_cache[i].tex)
         video_driver_texture_unload(&grid_ring_cache[i].tex);
      memset(&grid_ring_cache[i], 0, sizeof(grid_ring_cache[i]));
   }
   if (grid_arc_tex)
      video_driver_texture_unload(&grid_arc_tex);
   grid_arc_tex = 0;
   memset(grid_arc_key, 0, sizeof(grid_arc_key));
   if (grid_rect_arc_tex)
      video_driver_texture_unload(&grid_rect_arc_tex);
   grid_rect_arc_tex = 0;
   memset(grid_rect_arc_key, 0, sizeof(grid_rect_arc_key));
   if (grid_divider_tex)
      video_driver_texture_unload(&grid_divider_tex);
   grid_divider_tex = 0;
   grid_divider_w   = grid_divider_h = 0;
}

/* White ring around a w x h rect of corner radius r: empty for gap
 * pixels, then thick pixels of ring, anti-aliased from the rounded
 * rectangle's signed distance. The texture is the rect grown by
 * gap + thick + 1 on each side. A notch of radius nr (0: none) centred on
 * (nx, ny), from the rect's top-left, is cut out (for a badge). */
static uintptr_t *grid_ring_texture_notch(int w, int h, int r, int gap,
      int thick, float nx, float ny, float nr, unsigned *tw, unsigned *th)
{
   unsigned i, x, y, slot = 0;
   struct texture_image ti;
   int m     = gap + thick + 1;
   float hw  = w / 2.0f, hh = h / 2.0f;
   float in  = (float)gap, out = (float)(gap + thick);

   for (i = 0; i < GRID_RING_CACHE; i++)
   {
      if (     grid_ring_cache[i].tex && grid_ring_cache[i].w == w
            && grid_ring_cache[i].h == h && grid_ring_cache[i].r == r
            && grid_ring_cache[i].gap == gap
            && grid_ring_cache[i].thick == thick
            && grid_ring_cache[i].nx == nx && grid_ring_cache[i].ny == ny
            && grid_ring_cache[i].nr == nr)
      {
         grid_ring_cache[i].last_used = ++grid_ring_clock;
         *tw = grid_ring_cache[i].tw;
         *th = grid_ring_cache[i].th;
         return &grid_ring_cache[i].tex;
      }
      if (grid_ring_cache[i].last_used < grid_ring_cache[slot].last_used)
         slot = i;
   }

   /* Replace the least recently used entry. */
   if (grid_ring_cache[slot].tex)
      video_driver_texture_unload(&grid_ring_cache[slot].tex);
   grid_ring_cache[slot].tex = 0;

   ti.width         = (unsigned)(w + 2 * m);
   ti.height        = (unsigned)(h + 2 * m);
   ti.supports_rgba = video_driver_supports_rgba();
   ti.pixels        = (uint32_t*)malloc(ti.width * ti.height * sizeof(uint32_t));
   if (!ti.pixels)
      return NULL;

   if (r * 2 > w) r = w / 2;
   if (r * 2 > h) r = h / 2;

   for (y = 0; y < ti.height; y++)
      for (x = 0; x < ti.width; x++)
      {
         /* Position relative to the rect's centre. */
         float px   = x + 0.5f - m - hw;
         float py   = y + 0.5f - m - hh;
         float qx   = fabsf(px) - (hw - r);
         float qy   = fabsf(py) - (hh - r);
         float ox   = qx > 0.0f ? qx : 0.0f;
         float oy   = qy > 0.0f ? qy : 0.0f;
         float mx   = qx > qy ? qx : qy;
         float d    = sqrtf(ox * ox + oy * oy) + (mx < 0.0f ? mx : 0.0f) - r;
         float a_in = d - in + 0.5f;   /* past the gap */
         float a_out= out - d + 0.5f;  /* before the ring's outer edge */
         float a;

         if (a_in  < 0.0f) a_in  = 0.0f;
         if (a_in  > 1.0f) a_in  = 1.0f;
         if (a_out < 0.0f) a_out = 0.0f;
         if (a_out > 1.0f) a_out = 1.0f;
         a = a_in * a_out;
         if (nr > 0.0f)
         {
            float ndx  = x + 0.5f - m - nx, ndy = y + 0.5f - m - ny;
            float keep = sqrtf(ndx * ndx + ndy * ndy) - nr + 0.5f;
            if (keep < 0.0f) keep = 0.0f;
            if (keep > 1.0f) keep = 1.0f;
            a *= keep;
         }

         ti.pixels[y * ti.width + x] = ((uint32_t)(a * 255.0f + 0.5f) << 24)
            | 0x00ffffff;
      }

   video_driver_texture_load(&ti, TEXTURE_FILTER_NEAREST,
         &grid_ring_cache[slot].tex);
   free(ti.pixels);

   grid_ring_cache[slot].w         = w;
   grid_ring_cache[slot].h         = h;
   grid_ring_cache[slot].r         = r;
   grid_ring_cache[slot].gap       = gap;
   grid_ring_cache[slot].thick     = thick;
   grid_ring_cache[slot].nx        = nx;
   grid_ring_cache[slot].ny        = ny;
   grid_ring_cache[slot].nr        = nr;
   grid_ring_cache[slot].tw        = ti.width;
   grid_ring_cache[slot].th        = ti.height;
   grid_ring_cache[slot].last_used = ++grid_ring_clock;
   *tw = ti.width;
   *th = ti.height;
   return &grid_ring_cache[slot].tex;
}

static uintptr_t *grid_ring_texture(int w, int h, int r, int gap, int thick,
      unsigned *tw, unsigned *th)
{
   return grid_ring_texture_notch(w, h, r, gap, thick, 0.0f, 0.0f, 0.0f,
         tw, th);
}

/* Filled rounded rectangle as one exact-size texture (a "ring" whose
 * inner edge is far inside the shape). Unlike grid_draw_rounded, which
 * overlaps pieces, it works with translucent colours. */
static void grid_draw_shape(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h, float r, uint32_t rgba)
{
   float color[16];
   int ix = (int)floorf(x + 0.5f);
   int iy = (int)floorf(y + 0.5f);
   int iw = (int)floorf(x + w + 0.5f) - ix;
   int ih = (int)floorf(y + h + 0.5f) - iy;
   unsigned tw, th;
   uintptr_t *tex;

   if (iw < 1 || ih < 1)
      return;
   /* Margin of the texture: gap + thick + 1 = 1 pixel. */
   if (!(tex = grid_ring_texture(iw, ih, (int)floorf(r + 0.5f), -100000,
               100000, &tw, &th)) || !*tex)
      return;

   grid_set_color(color, rgba);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         ix - 1, iy - 1, tw, th, width, height, color, tex);
}

/* The selection cursor's colour now: light blue, gently pulsing. */
static uint32_t grid_cursor_color(void)
{
   float t     = cpu_features_get_time_usec() / 1000000.0f;
   float pulse = 0.5f + 0.5f * sinf(t * 3.0f);
   return grid_mix(COL_SELECT, COL_SELECT_HI, pulse);
}

/* The selection cursor's ring in the given colour, around an item of the
 * given rect and corner radius, with a small gap between them. */
static void grid_draw_cursor_ring_notch(gfx_display_t *p_disp,
      void *userdata, unsigned width, unsigned height,
      float x, float y, float w, float h, float r, uint32_t rgba,
      float nx, float ny, float nr)
{
   float color[16];
   /* Whole pixels, so the texture maps 1:1 onto the screen. */
   int ix       = (int)floorf(x + 0.5f);
   int iy       = (int)floorf(y + 0.5f);
   int iw       = (int)floorf(x + w + 0.5f) - ix;
   int ih       = (int)floorf(y + h + 0.5f) - iy;
   int gap      = (int)floorf(REF(2.5) + 0.5f);
   int thick    = (int)floorf(REF(2) + 0.5f);
   int m;
   unsigned tw, th;
   uintptr_t *tex;

   if (gap < 1)   gap   = 1;
   if (thick < 1) thick = 1;
   m = gap + thick + 1;

   /* The ring follows the item's corners, rounded out by the gap. */
   if (!(tex = grid_ring_texture_notch(iw, ih, (int)floorf(r + 0.5f), gap,
               thick, nx, ny, nr, &tw, &th)) || !*tex)
      return;

   grid_set_color(color, rgba);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         ix - m, iy - m, tw, th, width, height, color, tex);
}

static void grid_draw_cursor_ring(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h, float r, uint32_t rgba)
{
   grid_draw_cursor_ring_notch(p_disp, userdata, width, height,
         x, y, w, h, r, rgba, 0.0f, 0.0f, 0.0f);
}

/* Selection cursor: a light blue ring, gently pulsing. */
static void grid_draw_cursor(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h, float r)
{
   grid_draw_cursor_ring(p_disp, userdata, width, height, x, y, w, h, r,
         grid_cursor_color());
}

/* Same, with a notch cut out of it for a badge (see
 * grid_ring_texture_notch). */
static void grid_draw_cursor_notch(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h, float r,
      float nx, float ny, float nr)
{
   grid_draw_cursor_ring_notch(p_disp, userdata, width, height, x, y, w, h,
         r, grid_cursor_color(), nx, ny, nr);
}

#define GRID_ARC_STEPS 240 /* progress steps the arc is redrawn at */

/* White arc of the ring grid_ring_texture makes round a d-wide circle
 * (same edges, so it is as crisp), clockwise from the top for step /
 * GRID_ARC_STEPS of the way, its end anti-aliased too. One texture, made
 * again only when the step changes. */
static uintptr_t *grid_arc_texture(int d, int gap, int thick, int step)
{
   const float pi = 3.14159265f;
   struct texture_image ti;
   unsigned x, y;
   int m     = gap + thick + 1;
   float r   = d / 2.0f;
   float in  = (float)gap, out = (float)(gap + thick);
   float mid = r + gap + thick / 2.0f; /* ring centre line, for the end */
   float end = 2.0f * pi * step / GRID_ARC_STEPS;

   if (     grid_arc_tex && grid_arc_key[0] == d && grid_arc_key[1] == gap
         && grid_arc_key[2] == thick && grid_arc_key[3] == step)
      return &grid_arc_tex;

   if (grid_arc_tex)
      video_driver_texture_unload(&grid_arc_tex);
   grid_arc_tex = 0;

   ti.width         = (unsigned)(d + 2 * m);
   ti.height        = (unsigned)(d + 2 * m);
   ti.supports_rgba = video_driver_supports_rgba();
   ti.pixels        = (uint32_t*)malloc(ti.width * ti.height * sizeof(uint32_t));
   if (!ti.pixels)
      return NULL;

   for (y = 0; y < ti.height; y++)
      for (x = 0; x < ti.width; x++)
      {
         float px    = x + 0.5f - m - r;
         float py    = y + 0.5f - m - r;
         float dist  = sqrtf(px * px + py * py) - r;
         float a_in  = dist - in + 0.5f;
         float a_out = out - dist + 0.5f;
         /* Angle clockwise from the top, 0 .. 2 pi. */
         float ang   = atan2f(px, -py);
         float a_end, a;

         if (ang < 0.0f)
            ang += 2.0f * pi;
         /* Distance past the end along the ring, in pixels. */
         a_end = (end - ang) * mid + 0.5f;

         if (a_in  < 0.0f) a_in  = 0.0f;
         if (a_in  > 1.0f) a_in  = 1.0f;
         if (a_out < 0.0f) a_out = 0.0f;
         if (a_out > 1.0f) a_out = 1.0f;
         if (a_end < 0.0f) a_end = 0.0f;
         if (a_end > 1.0f) a_end = 1.0f;
         a = a_in * a_out * a_end;

         ti.pixels[y * ti.width + x] = ((uint32_t)(a * 255.0f + 0.5f) << 24)
            | 0x00ffffff;
      }

   video_driver_texture_load(&ti, TEXTURE_FILTER_NEAREST, &grid_arc_tex);
   free(ti.pixels);

   grid_arc_key[0] = d;
   grid_arc_key[1] = gap;
   grid_arc_key[2] = thick;
   grid_arc_key[3] = step;
   return &grid_arc_tex;
}

/* Hold progress round a d-wide circle at (x, y), in place of the
 * selection cursor: the cursor's ring turns from blue to a faint dark red
 * track as blend goes 0 .. 1, and a red arc fills it clockwise from the
 * top for the fraction t, at opacity blend (fading out on release). */
static void grid_draw_hold_ring(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, float x, float y, float d, float t,
      float blend)
{
   float color[16];
   int ix         = (int)floorf(x + 0.5f);
   int iy         = (int)floorf(y + 0.5f);
   int id         = (int)floorf(x + d + 0.5f) - ix;
   int gap        = (int)floorf(REF(2.5) + 0.5f);
   int thick      = (int)floorf(REF(2) + 0.5f); /* as grid_draw_cursor */
   int m;
   float e;
   uint32_t track;

   if (gap < 1)   gap   = 1;
   if (thick < 1) thick = 1;
   if (t < 0.0f) t = 0.0f;
   if (t > 1.0f) t = 1.0f;
   if (blend < 0.0f) blend = 0.0f;
   if (blend > 1.0f) blend = 1.0f;
   m = gap + thick + 1;
   e = blend * blend * (3.0f - 2.0f * blend); /* smoothstep */

   /* The cursor's ring, blue to the track's colour and opacity. */
   track = (grid_mix(grid_cursor_color(), COL_DANGER_BG, e) & 0xffffff00)
         | (uint32_t)(255.0f + (0x50 - 255.0f) * e + 0.5f);
   grid_draw_cursor_ring(p_disp, userdata, width, height,
         ix, iy, id, id, id / 2.0f, track);

   {
      uintptr_t *tex = grid_arc_texture(id, gap, thick,
            (int)(t * GRID_ARC_STEPS + 0.5f));
      if (!tex || !*tex || e <= 0.0f)
         return;
      grid_set_color(color, (COL_DANGER & 0xffffff00)
            | (uint32_t)(255.0f * e + 0.5f));
      gfx_display_draw_quad(p_disp, userdata, width, height,
            ix - m, iy - m, (unsigned)(id + 2 * m), (unsigned)(id + 2 * m),
            width, height, color, tex);
   }
}

/* Hold progress arc for a w x h rect of corner radius r: like
 * grid_arc_texture, but following the rounded rectangle, clockwise from
 * the middle of the top edge, by distance along it. One cached texture
 * (grid_rect_arc_tex). */
static uintptr_t *grid_rect_arc_texture(int w, int h, int r, int gap,
      int thick, int step)
{
   const float pi = 3.14159265f;
   struct texture_image ti;
   unsigned x, y;
   int m     = gap + thick + 1;
   float hw  = w / 2.0f, hh = h / 2.0f;
   float in  = (float)gap, out = (float)(gap + thick);
   float rm, a, b, c, len, end;

   if (r * 2 > w) r = w / 2;
   if (r * 2 > h) r = h / 2;
   rm  = r + gap + thick / 2.0f;     /* corners' radius on the ring's middle */
   a   = hw - r;                     /* half a straight top / bottom */
   b   = hh - r;                     /* half a straight side */
   c   = pi * rm / 2.0f;             /* a corner */
   len = 4.0f * a + 4.0f * b + 4.0f * c;
   end = len * step / GRID_ARC_STEPS;

   if (     grid_rect_arc_tex && grid_rect_arc_key[0] == w
         && grid_rect_arc_key[1] == h && grid_rect_arc_key[2] == r
         && grid_rect_arc_key[3] == gap && grid_rect_arc_key[4] == thick
         && grid_rect_arc_key[5] == step)
      return &grid_rect_arc_tex;

   if (grid_rect_arc_tex)
      video_driver_texture_unload(&grid_rect_arc_tex);
   grid_rect_arc_tex = 0;

   ti.width         = (unsigned)(w + 2 * m);
   ti.height        = (unsigned)(h + 2 * m);
   ti.supports_rgba = video_driver_supports_rgba();
   ti.pixels        = (uint32_t*)malloc(ti.width * ti.height * sizeof(uint32_t));
   if (!ti.pixels)
      return NULL;

   for (y = 0; y < ti.height; y++)
      for (x = 0; x < ti.width; x++)
      {
         float px   = x + 0.5f - m - hw;
         float py   = y + 0.5f - m - hh;
         float qx   = fabsf(px) - a;
         float qy   = fabsf(py) - b;
         float ox   = qx > 0.0f ? qx : 0.0f;
         float oy   = qy > 0.0f ? qy : 0.0f;
         float mx   = qx > qy ? qx : qy;
         float d    = sqrtf(ox * ox + oy * oy) + (mx < 0.0f ? mx : 0.0f) - r;
         float a_in = d - in + 0.5f;
         float a_out= out - d + 0.5f;
         float s, a_end, al;

         /* Distance along the ring from the top's middle, clockwise. */
         if (qx > 0.0f && qy > 0.0f)
         {
            if (px >= 0.0f && py < 0.0f)       /* top right */
               s = a + rm * atan2f(qx, qy);
            else if (px >= 0.0f)               /* bottom right */
               s = a + c + 2.0f * b + rm * atan2f(qy, qx);
            else if (py >= 0.0f)               /* bottom left */
               s = 3.0f * a + 2.0f * b + 2.0f * c + rm * atan2f(qx, qy);
            else                               /* top left */
               s = 3.0f * a + 4.0f * b + 3.0f * c + rm * atan2f(qy, qx);
         }
         else if (qy >= qx)                    /* top or bottom */
            s = py < 0.0f ? (px >= 0.0f ? px : len + px)
                          : 2.0f * a + 2.0f * b + 2.0f * c - px;
         else                                  /* right or left */
            s = px >= 0.0f ? a + c + b + py
                           : 3.0f * a + 3.0f * b + 3.0f * c - py;

         a_end = end - s + 0.5f;
         if (a_in  < 0.0f) a_in  = 0.0f;
         if (a_in  > 1.0f) a_in  = 1.0f;
         if (a_out < 0.0f) a_out = 0.0f;
         if (a_out > 1.0f) a_out = 1.0f;
         if (a_end < 0.0f) a_end = 0.0f;
         if (a_end > 1.0f) a_end = 1.0f;
         if (step >= GRID_ARC_STEPS)
            a_end = 1.0f;
         al = a_in * a_out * a_end;

         ti.pixels[y * ti.width + x] = ((uint32_t)(al * 255.0f + 0.5f) << 24)
            | 0x00ffffff;
      }

   video_driver_texture_load(&ti, TEXTURE_FILTER_NEAREST, &grid_rect_arc_tex);
   free(ti.pixels);

   grid_rect_arc_key[0] = w;
   grid_rect_arc_key[1] = h;
   grid_rect_arc_key[2] = r;
   grid_rect_arc_key[3] = gap;
   grid_rect_arc_key[4] = thick;
   grid_rect_arc_key[5] = step;
   return &grid_rect_arc_tex;
}

/* grid_draw_hold_ring for a rounded rect (an Installs card). */
static void grid_draw_hold_rect(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, float x, float y, float w, float h,
      float r, float t, float blend)
{
   float color[16];
   int ix    = (int)floorf(x + 0.5f);
   int iy    = (int)floorf(y + 0.5f);
   int iw    = (int)floorf(x + w + 0.5f) - ix;
   int ih    = (int)floorf(y + h + 0.5f) - iy;
   int gap   = (int)floorf(REF(2.5) + 0.5f);
   int thick = (int)floorf(REF(2) + 0.5f);
   int m;
   float e;
   uint32_t track;
   uintptr_t *tex;

   if (gap < 1)   gap   = 1;
   if (thick < 1) thick = 1;
   if (t < 0.0f) t = 0.0f;
   if (t > 1.0f) t = 1.0f;
   if (blend < 0.0f) blend = 0.0f;
   if (blend > 1.0f) blend = 1.0f;
   m = gap + thick + 1;
   e = blend * blend * (3.0f - 2.0f * blend); /* smoothstep */

   track = (grid_mix(grid_cursor_color(), COL_DANGER_BG, e) & 0xffffff00)
         | (uint32_t)(255.0f + (0x50 - 255.0f) * e + 0.5f);
   grid_draw_cursor_ring(p_disp, userdata, width, height,
         ix, iy, iw, ih, r, track);

   if (     e <= 0.0f
         || !(tex = grid_rect_arc_texture(iw, ih, (int)floorf(r + 0.5f),
               gap, thick, (int)(t * GRID_ARC_STEPS + 0.5f)))
         || !*tex)
      return;
   grid_set_color(color, (COL_DANGER & 0xffffff00)
         | (uint32_t)(255.0f * e + 0.5f));
   gfx_display_draw_quad(p_disp, userdata, width, height,
         ix - m, iy - m, (unsigned)(iw + 2 * m), (unsigned)(ih + 2 * m),
         width, height, color, tex);
}

static void grid_free_fonts(void)
{
   unsigned i;
   for (i = 0; i < grid_st.font_count; i++)
      if (grid_st.fonts[i].font)
         font_driver_free(grid_st.fonts[i].font);
   grid_st.font_count = 0;
}

/* Font of the given weight rasterised at the pixel size for this scale
 * (scale 1.0 = 1/32 of the screen height). Loaded on first use. */
static font_data_t *grid_font(bool bold, float scale, unsigned height)
{
   unsigned i;
   unsigned px = (unsigned)(height / 32.0f * scale + 0.5f);
   const char *path;
   font_data_t *font;

   if (px < 6)
      px = 6;

   /* Resolution changed: sizes are stale. */
   if (grid_st.font_screen_h != height)
   {
      grid_free_fonts();
      grid_st.font_screen_h = height;
   }

   for (i = 0; i < grid_st.font_count; i++)
      if (grid_st.fonts[i].px == px && grid_st.fonts[i].bold == bold)
         return grid_st.fonts[i].font;

   if (grid_st.font_count >= GRID_FONT_CACHE)
      return grid_st.fonts[0].font; /* shouldn't happen: few sizes used */

   path = grid_st.font_path[bold && !string_is_empty(grid_st.font_path[1])];
   font = gfx_display_font_file(disp_get_ptr(), (char*)path, (float)px,
         grid_st.font_threaded);

   grid_st.fonts[grid_st.font_count].font = font;
   grid_st.fonts[grid_st.font_count].px   = px;
   grid_st.fonts[grid_st.font_count].bold = bold;
   grid_st.font_count++;
   return font;
}

static void grid_draw_text_font(bool bold, const char *text,
      float x, float y, unsigned width, unsigned height, uint32_t color,
      enum text_alignment align, float scale)
{
   font_data_t *font = grid_font(bold, scale, height);

   if (!font)
      return;
   if (grid_alpha < 1.0f)
      color = (color & 0xffffff00)
         | (uint32_t)((color & 0xff) * grid_alpha + 0.5f);

   /* Whole-pixel origin keeps glyphs crisp at native size. */
   gfx_display_draw_text(font, text, floorf(x + 0.5f), floorf(y + 0.5f),
         width, height, color, align, 1.0f, false, 0.0f, false);
}

static void grid_draw_text(const char *text, float x, float y,
      unsigned width, unsigned height, uint32_t color,
      enum text_alignment align, float scale)
{
   grid_draw_text_font(false, text, x, y, width, height, color, align, scale);
}

static void grid_draw_text_bold(const char *text, float x, float y,
      unsigned width, unsigned height, uint32_t color,
      enum text_alignment align, float scale)
{
   grid_draw_text_font(true, text, x, y, width, height, color, align, scale);
}

/* Baseline y that vertically centres capital letters of the given scale
 * on cy. */
static float grid_text_y(float cy, unsigned height, float scale)
{
   /* Measured from the rendered capital "H" (the same way button letters
    * are centred), so text lines up with glyphs and dividers whatever
    * the font's metrics. */
   font_data_t *font          = grid_font(false, scale, height);
   const struct font_glyph *g = NULL;

   if (font && font->renderer && font->renderer->get_glyph)
      g = font->renderer->get_glyph(font->renderer_data, (uint32_t)'H');

   /* Glyph box top = baseline + draw_offset_y. */
   if (g && g->height)
      return floorf(cy - g->draw_offset_y - g->height / 2.0f + 0.5f);

   return cy + (height / 32.0f) * scale * (GRID_FONT_CAP / 2.0f);
}

static float grid_text_width_font(bool bold, const char *text,
      unsigned height, float scale)
{
   font_data_t *font = grid_font(bold, scale, height);
   if (!font)
      return 0.0f;
   return font_driver_get_message_width(font, text, strlen(text), 1.0f);
}

static float grid_text_width(const char *text, unsigned height, float scale)
{
   return grid_text_width_font(false, text, height, scale);
}


static void grid_flush_text(unsigned width, unsigned height)
{
   unsigned i;
   for (i = 0; i < grid_st.font_count; i++)
   {
      font_data_t *font = grid_st.fonts[i].font;
      if (font && font->renderer && font->renderer->flush)
         font->renderer->flush(width, height, font->renderer_data);
   }
}

/* In-game overlays (achievement popups, trackers) borrow the grid's
 * shapes and fonts so they match the menus. Sizes are in the grid's
 * reference pixels (REF), text by cap height like GRID_TEXT_CAP. */
void grid_overlay_shape(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h, float r, uint32_t rgba)
{
   grid_draw_shape(p_disp, userdata, width, height, x, y, w, h, r, rgba);
}

void grid_overlay_text(bool bold, const char *text, float x, float cy,
      unsigned width, unsigned height, uint32_t color,
      enum text_alignment align, float cap)
{
   grid_draw_text_font(bold, text, x, grid_text_y(cy, height, CAP(cap)),
         width, height, color, align, CAP(cap));
}

float grid_overlay_text_width(bool bold, const char *text,
      unsigned height, float cap)
{
   return grid_text_width_font(bold, text, height, CAP(cap));
}

void grid_overlay_flush(unsigned width, unsigned height)
{
   grid_flush_text(width, height);
}

/* Draws a single character centred on (cx, cy) using the glyph's actual
 * bounding box, so letters sit exactly in the middle of a button glyph. */
static void grid_draw_char_centered(bool bold, char ch, float cx, float cy,
      unsigned width, unsigned height, uint32_t color, float scale)
{
   char str[2];
   const struct font_glyph *g = NULL;
   font_data_t *font          = grid_font(bold, scale, height);

   if (!font)
      return;

   str[0] = ch;
   str[1] = '\0';

   if (font->renderer && font->renderer->get_glyph)
      g = font->renderer->get_glyph(font->renderer_data, (uint32_t)ch);

   if (g && g->width && g->height)
   {
      /* Glyph box: left = pen + draw_offset_x, top = baseline + draw_offset_y */
      float pen      = floorf(cx - g->draw_offset_x - g->width  / 2.0f + 0.5f);
      float baseline = floorf(cy - g->draw_offset_y - g->height / 2.0f + 0.5f);
      grid_draw_text_font(bold, str, pen, baseline, width, height, color,
            TEXT_ALIGN_LEFT, scale);
   }
   else
      grid_draw_text_font(bold, str, cx, grid_text_y(cy, height, scale),
            width, height, color, TEXT_ALIGN_CENTER, scale);
}

/* Vertical gradient (top colour to bottom colour), e.g. a drop shadow. */
static void grid_draw_vgradient(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h, uint32_t top, uint32_t bottom)
{
   float color[16], c_top[16], c_bot[16];
   int i;

   grid_set_color(c_top, top);
   grid_set_color(c_bot, bottom);

   /* Quad vertices go bottom-left, bottom-right, top-left, top-right
    * (the display driver's y axis points up). */
   for (i = 0; i < 8; i++)
   {
      color[i]     = c_bot[i];
      color[i + 8] = c_top[i];
   }

   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)x, (int)y, (unsigned)w, (unsigned)h,
         width, height, color, NULL);
}

static void grid_glyph_key(char glyph, char *s, size_t len);

/* L / R key glyph, as in the reference: a light rectangle with slightly
 * rounded corners and a dark regular-weight letter in the middle (in
 * mouse and keyboard mode the keyboard's key, when it is one letter). */
static void grid_draw_key(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, char letter,
      float x, float y, float w, float h)
{
   if (grid_st.kbm)
   {
      char key[32];
      grid_glyph_key(letter, key, sizeof(key));
      if (key[0] && !key[1])
         letter = key[0];
   }

   grid_draw_rounded(p_disp, userdata, width, height,
         x, y, w, h, h * 0.16f, COL_GLYPH);

   /* Letter cap height about half the key. */
   grid_draw_char_centered(false, letter, x + w / 2.0f, y + h / 2.0f,
         width, height, COL_TEXT_DARK,
         h * 0.52f / (height / 32.0f) / GRID_FONT_CAP);
}

/* Draws text inside [x, x + w]. If it doesn't fit, it scrolls as a seamless
 * loop (pause, then a continuous ticker repeating the text). key identifies what is being shown
 * so the scroll restarts when it changes; a few keys are tracked at once,
 * so several lines can scroll together. align applies when it fits. */
#define GRID_MARQUEE_SLOTS 4

/* Draws text whose left end is at x, skipping the characters that end
 * before clip_x (they'd be clipped anyway). Needed for scrolling text:
 * the display driver drops a whole string that starts far off screen,
 * which made long scrolling lines vanish. */
static void grid_draw_text_from(bool bold, const char *text, float x,
      float clip_x, float y, unsigned width, unsigned height,
      uint32_t color, float scale)
{
   while (*text)
   {
      /* Width of the next character (all bytes of a UTF-8 sequence). */
      char ch[8];
      size_t n = 1;
      float cw;

      while (text[n] && ((unsigned char)text[n] & 0xc0) == 0x80 && n < 7)
         n++;
      memcpy(ch, text, n);
      ch[n] = '\0';
      cw    = grid_text_width_font(bold, ch, height, scale);

      if (x + cw > clip_x)
         break;
      x    += cw;
      text += n;
   }

   if (*text)
      grid_draw_text_font(bold, text, x, y, width, height, color,
            TEXT_ALIGN_LEFT, scale);
}

static void grid_draw_marquee(gfx_display_t *p_disp, void *userdata,
      bool bold, const char *text, float x, float y, float w,
      unsigned width, unsigned height, uint32_t color, float scale,
      enum text_alignment align, const void *key)
{
   static struct
   {
      const void *key;
      retro_time_t start;
      retro_time_t seen;
   } slots[GRID_MARQUEE_SLOTS];
   const float pause            = 1.0f;           /* seconds */
   const float speed            = height * 0.06f; /* pixels per second */
   float text_w, overflow, t, offset, line_h;
   retro_time_t now, start;
   unsigned s, slot = 0;

   if (!grid_font(bold, scale, height) || w <= 0.0f)
      return;

   text_w = grid_text_width_font(bold, text, height, scale);

   if (text_w <= w)
   {
      grid_draw_text_font(bold, text,
            align == TEXT_ALIGN_CENTER ? x + w / 2.0f : x,
            y, width, height, color, align, scale);
      return;
   }

   /* This key's scroll start; a new key takes the stalest slot (one not
    * drawn for the longest) and starts from the beginning. */
   now = cpu_features_get_time_usec();
   for (s = 0; s < GRID_MARQUEE_SLOTS; s++)
   {
      if (slots[s].key == key)
         break;
      if (slots[s].seen < slots[slot].seen)
         slot = s;
   }
   if (s < GRID_MARQUEE_SLOTS)
      slot = s;
   else
   {
      slots[slot].key   = key;
      slots[slot].start = now;
   }
   /* Not drawn for a while (e.g. selected again later): start over. */
   if (now - slots[slot].seen > 200000)
      slots[slot].start = now;
   slots[slot].seen = now;
   start            = slots[slot].start;

   /* Seamless loop: after a pause, the text scrolls left continuously and
    * a copy follows it a gap behind, so it reads like a ticker. */
   overflow    = text_w + height * 0.05f;   /* text + gap to its copy */
   t           = (now - start) / 1000000.0f;

   if (t < pause)
      offset = 0.0f;
   else
      offset = fmodf((t - pause) * speed, overflow);

   /* Clip to the text box. Earlier text is flushed first so the
    * clip only affects this string. */
   line_h = (height / 32.0f) * scale;

   grid_flush_text(width, height);

   if (p_disp->dispctx && p_disp->dispctx->scissor_begin)
      gfx_display_scissor_begin(p_disp, userdata, width, height,
            (int)x, (int)(y - line_h * 1.1f),
            (unsigned)w, (unsigned)(line_h * 1.6f));

   grid_draw_text_from(bold, text, x - offset, x, y, width, height, color,
         scale);
   grid_draw_text_from(bold, text, x - offset + overflow, x, y, width,
         height, color, scale);
   grid_flush_text(width, height);

   if (p_disp->dispctx && p_disp->dispctx->scissor_end)
      p_disp->dispctx->scissor_end(userdata, width, height);
}

static void grid_draw_image(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h,
      uintptr_t *tex, unsigned tex_w, unsigned tex_h, uint32_t tint)
{
   float color[16];
   float s = w / tex_w;
   float iw, ih;

   /* Fit inside the box, keeping aspect ratio. */
   if (tex_h * s > h)
      s = h / tex_h;
   iw = tex_w * s;
   ih = tex_h * s;

   grid_set_color(color, tint);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)(x + (w - iw) / 2.0f), (int)(y + (h - ih) / 2.0f),
         (unsigned)iw, (unsigned)ih, width, height, color, tex);
}

static void grid_bind_key_text(const struct retro_keybind *b,
      char *s, size_t len);

/* The keyboard key doing what a controller button does in the menu ("A":
 * Enter; X, Y, L and R as bound for player 1). */
static void grid_glyph_key(char glyph, char *s, size_t len)
{
   unsigned id;

   switch (glyph)
   {
      case 'A': strlcpy(s, "Enter", len);     return;
      case 'B': strlcpy(s, "Esc", len);       return;
      /* (Glyph "X" is the menu's scan, RetroPad Y; "Y" its search.) */
      case 'X': id = RETRO_DEVICE_ID_JOYPAD_Y; break;
      case 'Y': id = RETRO_DEVICE_ID_JOYPAD_X; break;
      case 'L': id = RETRO_DEVICE_ID_JOYPAD_L; break;
      case 'R': id = RETRO_DEVICE_ID_JOYPAD_R; break;
      default:
         s[0] = glyph;
         s[1] = '\0';
         return;
   }
   grid_bind_key_text(&input_config_binds[0][id], s, len);
   if (!s[0])
   {
      s[0] = glyph;
      s[1] = '\0';
   }
}

/* Text being typed: shown (the line, or a star per byte of a password)
 * from x on the line centred at cy, clipped to max_w and scrolled to keep
 * the caret in view, with the selection behind it and a blinking caret.
 * Recorded for the mouse (a click places the caret, dragging selects). */
static void grid_draw_edit(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, const char *shown, float x,
      float cy, float max_w, float scale, uint32_t color)
{
   input_keyboard_line_t *line = &input_state_get_ptr()->keyboard_line;
   float *xs    = grid_hits_next.edit_xs;
   size_t len   = strlen(shown);
   float ty     = grid_text_y(cy, height, scale);
   float line_h = (height / 32.0f) * scale;
   float bar_w  = grid_text_width("|", height, scale);
   float off    = 0.0f;
   size_t i, a, b, ptr;

   if (len > GRID_EDIT_MAX)
      len = GRID_EDIT_MAX;

   /* Where each byte starts (inside a UTF-8 character: -1). */
   for (i = 0; i <= len; i++)
   {
      char prefix[GRID_EDIT_MAX + 1];
      if (i > 0 && i < len && ((unsigned char)shown[i] & 0xc0) == 0x80)
      {
         xs[i] = -1.0f;
         continue;
      }
      memcpy(prefix, shown, i);
      prefix[i] = '\0';
      xs[i]     = i ? grid_text_width(prefix, height, scale) : 0.0f;
   }

   ptr = line->ptr < len ? line->ptr : len;
   while (ptr > 0 && xs[ptr] < 0.0f)
      ptr--;
   if (xs[ptr] + bar_w > max_w)
      off = xs[ptr] + bar_w - max_w;

   if (grid_edit_sel(&a, &b) && b <= len && xs[a] >= 0.0f && xs[b] >= 0.0f)
   {
      float x0 = x + xs[a] - off;
      float x1 = x + xs[b] - off;
      if (x0 < x)
         x0 = x;
      if (x1 > x + max_w)
         x1 = x + max_w;
      if (x1 > x0)
         grid_draw_rect(p_disp, userdata, width, height,
               x0, cy - line_h * 0.5f, x1 - x0, line_h,
               (COL_SELECT & 0xffffff00) | 0x90);
   }

   grid_flush_text(width, height);
   if (p_disp->dispctx && p_disp->dispctx->scissor_begin)
      gfx_display_scissor_begin(p_disp, userdata, width, height,
            (int)x, (int)(ty - line_h * 1.1f),
            (unsigned)max_w, (unsigned)(line_h * 1.6f));

   grid_draw_text_from(false, shown, x - off, x, ty, width, height, color,
         scale);
   if (fmodf(cpu_features_get_time_usec() / 1000000.0f, 1.0f) < 0.5f)
      grid_draw_text("|", x + xs[ptr] - off, ty, width, height, color,
            TEXT_ALIGN_LEFT, scale);
   grid_flush_text(width, height);

   if (p_disp->dispctx && p_disp->dispctx->scissor_end)
      p_disp->dispctx->scissor_end(userdata, width, height);

   grid_hits_next.edit     = true;
   grid_hits_next.edit_x   = x;
   grid_hits_next.edit_w   = max_w;
   grid_hits_next.edit_y   = cy - line_h;
   grid_hits_next.edit_h   = 2.0f * line_h;
   grid_hits_next.edit_x0  = x - off;
   grid_hits_next.edit_len = len;
}

/* A text entry's prompt: what is asked over what's typed so far, in a
 * dark rounded box like the message box. */
static void grid_draw_text_prompt(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, const char *label, const char *text,
      float cy)
{
   float ts     = CAP(GRID_TEXT_CAP);
   float line_h = REF(17);
   float pad    = REF(16);
   float max_w  = width * 0.7f;
   float tw     = grid_text_width(text, height, ts)
                + grid_text_width("|", height, ts);
   float w      = grid_text_width(label, height, ts);
   float bx, tx;

   if (w < REF(200))
      w = REF(200);
   if (tw > w)
      w = tw;
   if (w > max_w)
      w = max_w;
   if (tw > w)
      tw = w;
   bx = (width - w) / 2.0f;
   tx = (width - tw) / 2.0f;

   grid_draw_shape(p_disp, userdata, width, height,
         bx - pad, cy - line_h - pad, w + 2.0f * pad, 2.0f * line_h + 2.0f * pad,
         REF(12), 0x1c1c1ef0);
   grid_draw_text(label, width / 2.0f,
         grid_text_y(cy - line_h / 2.0f, height, ts), width, height,
         COL_TEXT, TEXT_ALIGN_CENTER, ts);
   grid_draw_edit(p_disp, userdata, width, height, text, tx,
         cy + line_h / 2.0f, bx + w - tx, ts, COL_TEXT);
}

/* The letter printed on the face button that does a grid glyph's action.
 * Glyphs name actions: "A" OK, "B" back, "X" the menu's scan, "Y" its
 * search. Which button that is depends on player 1's binds: xinput's
 * buttons 0-3 are A, B, X, Y; otherwise the RetroPad's buttons are named
 * by position, as on an Xbox pad (RetroPad X, the top one, is "Y"). */
static char grid_glyph_button(char glyph)
{
   static const char xinput[4] = { 'A', 'B', 'X', 'Y' };
   bool swap = config_get_ptr()->bools.input_menu_swap_ok_cancel_buttons;
   const input_device_driver_t *joypad = input_state_get_ptr()->primary_joypad;
   unsigned id;
   uint16_t joykey;

   switch (glyph)
   {
      case 'A': id = swap ? RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_A; break;
      case 'B': id = swap ? RETRO_DEVICE_ID_JOYPAD_A : RETRO_DEVICE_ID_JOYPAD_B; break;
      case 'X': id = RETRO_DEVICE_ID_JOYPAD_Y; break;
      case 'Y': id = RETRO_DEVICE_ID_JOYPAD_X; break;
      default:  return glyph;
   }

   joykey = input_config_binds[0][id].joykey;
   if (joykey == NO_BTN)
      joykey = input_autoconf_binds[0][id].joykey;
   if (     joypad && joypad->ident && string_is_equal(joypad->ident, "xinput")
         && joykey < 4)
      return xinput[joykey];

   switch (id)
   {
      case RETRO_DEVICE_ID_JOYPAD_A: return 'B';
      case RETRO_DEVICE_ID_JOYPAD_B: return 'A';
      case RETRO_DEVICE_ID_JOYPAD_X: return 'Y';
      default:                       return 'X';
   }
}

/* A controller button's round glyph, d across at (gx, gy): an action's
 * button (see grid_glyph_button), a '+' or '-', or a short name ("L3"). */
static void grid_draw_button_glyph(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, float gx, float gy, float d,
      const char *glyph)
{
   grid_draw_rounded(p_disp, userdata, width, height,
         gx, gy, d, d, d / 2.0f, COL_GLYPH);

   if (glyph[0] && glyph[1])
   {
      /* Two letters: smaller, to fit side by side. */
      float s = d * 0.36f / (height / 32.0f) / GRID_FONT_CAP;
      grid_draw_text(glyph, gx + d / 2.0f,
            grid_text_y(gy + d / 2.0f, height, s), width, height,
            COL_TEXT_DARK, TEXT_ALIGN_CENTER, s);
   }
   else if (glyph[0] == '+' || glyph[0] == '-')
   {
      /* Drawn as bars ('+' two, '-' one): the font's symbols sit on the
       * math axis, not centred. Reference: strokes 2px of 30, arms ~half
       * the circle; whole pixels. */
      float t   = floorf(d * 0.07f + 0.5f);
      float len = 2.0f * floorf(d * 0.25f) + 1.0f;
      if (t < 1.0f)
         t = 1.0f;
      grid_draw_rect(p_disp, userdata, width, height,
            gx + floorf((d - len) / 2.0f), gy + floorf((d - t) / 2.0f),
            len, t, COL_TEXT_DARK);
      if (glyph[0] == '+')
         grid_draw_rect(p_disp, userdata, width, height,
               gx + floorf((d - t) / 2.0f), gy + floorf((d - len) / 2.0f),
               t, len, COL_TEXT_DARK);
   }
   else
      /* Letter cap height ~45% of the circle, so letters have the same
       * visual size as the '+'. */
      grid_draw_char_centered(false, grid_glyph_button(glyph[0]),
            gx + d / 2.0f, gy + d / 2.0f, width, height, COL_TEXT_DARK,
            d * 0.45f / (height / 32.0f) / GRID_FONT_CAP);
}

/* Round button glyph (e.g. "A") with its label to the right; in mouse and
 * keyboard mode the key doing the same instead, the hint clickable.
 * Returns the total width drawn. Draws nothing when measure_only. */
static float grid_draw_hint(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, float x, float cy,
      const char *glyph, const char *label, bool measure_only)
{
   /* Reference: circle 15px, label 6px after it, label caps GRID_TEXT_CAP.
    * Odd pixel sizes so the circle has a centre pixel row/column and the
    * symbol's strokes can sit exactly on it. A key: a rounded box as tall,
    * its name the size of the circle's letter. */
   float scale  = CAP(GRID_TEXT_CAP);
   float d      = 2.0f * floorf(REF(15) / 2.0f) + 1.0f;
   float gap    = REF(6);
   float lw     = grid_text_width(label, height, scale);
   float ks     = d * 0.45f / (height / 32.0f) / GRID_FONT_CAP;
   float gw     = d;
   char key[32];

   if (grid_st.kbm)
   {
      grid_glyph_key(glyph[0], key, sizeof(key));
      gw = floorf(grid_text_width(key, height, ks) + 2.0f * REF(4) + 0.5f);
      if (gw < d)
         gw = d;
   }

   if (!measure_only)
   {
      float gx = floorf(x + 0.5f);
      float gy = floorf(cy - d / 2.0f + 0.5f);
      float hp = floorf(REF(4) + 0.5f);
      float hw = floorf(gw + gap + lw + 2.0f * hp + 0.5f);
      enum menu_action action = MENU_ACTION_NOOP;

      switch (glyph[0])
      {
         case 'A': action = MENU_ACTION_OK;     break;
         case 'B': action = MENU_ACTION_CANCEL; break;
         case 'X': action = MENU_ACTION_SCAN;   break;
         case 'Y': action = MENU_ACTION_SEARCH; break;
      }
      grid_hit_add(gx - hp, gy - hp, hw, d + 2.0f * hp, HIT_NONE, 0, action);
      if (grid_hover(gx - hp, gy - hp, hw, d + 2.0f * hp))
         grid_draw_shape(p_disp, userdata, width, height,
               gx - hp, gy - hp, hw, d + 2.0f * hp, (d + 2.0f * hp) / 2.0f,
               COL_DIVIDER);

      if (grid_st.kbm)
      {
         grid_draw_rounded(p_disp, userdata, width, height,
               gx, gy, gw, d, floorf(d * 0.3f), COL_GLYPH);
         grid_draw_text(key, gx + gw / 2.0f,
               grid_text_y(gy + d / 2.0f, height, ks), width, height,
               COL_TEXT_DARK, TEXT_ALIGN_CENTER, ks);
         grid_draw_text(label, gx + gw + gap, grid_text_y(cy, height, scale),
               width, height, COL_TEXT, TEXT_ALIGN_LEFT, scale);
         return gw + gap + lw;
      }

      grid_draw_button_glyph(p_disp, userdata, width, height, gx, gy, d,
            glyph);
      grid_draw_text(label, gx + d + gap, grid_text_y(cy, height, scale),
            width, height, COL_TEXT, TEXT_ALIGN_LEFT, scale);
   }

   return gw + gap + lw;
}

/* ---------------------------------------------------------------------- */
/* Background                                                             */
/* ---------------------------------------------------------------------- */

/* One box-blur pass over an RGB float image, horizontally then
 * vertically, radius r. */
static void grid_box_blur(float *img, float *tmp, unsigned w, unsigned h, int r)
{
   unsigned x, y;
   int k, c;

   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
         for (c = 0; c < 3; c++)
         {
            float sum = 0.0f;
            for (k = -r; k <= r; k++)
            {
               int sx = (int)x + k;
               if (sx < 0) sx = 0;
               if (sx >= (int)w) sx = w - 1;
               sum += img[(y * w + sx) * 3 + c];
            }
            tmp[(y * w + x) * 3 + c] = sum / (2 * r + 1);
         }

   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
         for (c = 0; c < 3; c++)
         {
            float sum = 0.0f;
            for (k = -r; k <= r; k++)
            {
               int sy = (int)y + k;
               if (sy < 0) sy = 0;
               if (sy >= (int)h) sy = h - 1;
               sum += tmp[(sy * w + x) * 3 + c];
            }
            img[(y * w + x) * 3 + c] = sum / (2 * r + 1);
         }
}

/* Reads one source pixel of the game's frame as RGB 0-255. */
static void grid_read_pixel(const uint8_t *row, unsigned x,
      enum retro_pixel_format fmt, float *rgb)
{
   if (fmt == RETRO_PIXEL_FORMAT_XRGB8888)
   {
      uint32_t p = ((const uint32_t*)row)[x];
      rgb[0] = (p >> 16) & 0xff;
      rgb[1] = (p >>  8) & 0xff;
      rgb[2] =  p        & 0xff;
   }
   else if (fmt == RETRO_PIXEL_FORMAT_RGB565)
   {
      uint16_t p = ((const uint16_t*)row)[x];
      rgb[0] = ((p >> 11) & 0x1f) * (255.0f / 31.0f);
      rgb[1] = ((p >>  5) & 0x3f) * (255.0f / 63.0f);
      rgb[2] = ( p        & 0x1f) * (255.0f / 31.0f);
   }
   else /* 0RGB1555 */
   {
      uint16_t p = ((const uint16_t*)row)[x];
      rgb[0] = ((p >> 10) & 0x1f) * (255.0f / 31.0f);
      rgb[1] = ((p >>  5) & 0x1f) * (255.0f / 31.0f);
      rgb[2] = ( p        & 0x1f) * (255.0f / 31.0f);
   }
}

/* Background source: reads pixel (x, y) as RGB 0-255. */
typedef void (*grid_bg_reader_t)(const void *ctx, unsigned x, unsigned y,
      float *rgb);

static void grid_bg_read_frame(const void *ctx, unsigned x, unsigned y,
      float *rgb)
{
   video_driver_state_t *video_st = (video_driver_state_t*)ctx;
   grid_read_pixel((const uint8_t*)video_st->frame_cache_data
         + y * video_st->frame_cache_pitch, x, video_st->pix_fmt, rgb);
}

static void grid_bg_read_image(const void *ctx, unsigned x, unsigned y,
      float *rgb)
{
   const struct texture_image *ti = (const struct texture_image*)ctx;
   uint32_t p                     = ti->pixels[y * ti->width + x]; /* ARGB */
   rgb[0] = (p >> 16) & 0xff;
   rgb[1] = (p >>  8) & 0xff;
   rgb[2] =  p        & 0xff;
}

/* Shrinks an sw x sh source to GRID_BG_WIDTH px wide, blurs it (the linear
 * upscale softens it further) and darkens it evenly (by GRID_BG_DIM, or
 * more to bring its brightest pixel down to GRID_BG_MAX_LUMA), so the
 * grid stays readable. Returns a
 * new ARGB buffer (free() it), or NULL. */
static uint32_t *grid_bg_make(grid_bg_reader_t read, const void *ctx,
      unsigned sw, unsigned sh, unsigned *out_w, unsigned *out_h)
{
   const unsigned dw = GRID_BG_WIDTH;
   unsigned dh, x, y, i;
   float k, max_luma = 0.0f;
   float *img, *tmp;
   uint32_t *argb = NULL;

   if (!sw || !sh)
      return NULL;

   dh = (unsigned)(dw * (float)sh / sw + 0.5f);
   if (dh < 1)
      dh = 1;

   img  = (float*)calloc(dw * dh * 3, sizeof(float));
   tmp  = (float*)calloc(dw * dh * 3, sizeof(float));
   argb = (uint32_t*)malloc(dw * dh * sizeof(uint32_t));

   if (img && tmp && argb)
   {
      /* Shrink: average each block of source pixels (sampled at most
       * 4x4 per block, enough for a blurred result). */
      for (y = 0; y < dh; y++)
      {
         unsigned y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
         unsigned ys;
         if (y1 <= y0) y1 = y0 + 1;
         ys = (y1 - y0 + 3) / 4;

         for (x = 0; x < dw; x++)
         {
            unsigned x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            unsigned sx, sy, xs, n = 0;
            float acc[3] = { 0.0f, 0.0f, 0.0f };

            if (x1 <= x0) x1 = x0 + 1;
            xs = (x1 - x0 + 3) / 4;

            for (sy = y0; sy < y1 && sy < sh; sy += ys)
               for (sx = x0; sx < x1 && sx < sw; sx += xs)
               {
                  float rgb[3];
                  read(ctx, sx, sy, rgb);
                  acc[0] += rgb[0];
                  acc[1] += rgb[1];
                  acc[2] += rgb[2];
                  n++;
               }

            for (i = 0; i < 3; i++)
               img[(y * dw + x) * 3 + i] = n ? acc[i] / n : 0.0f;
         }
      }

      /* Two box passes: a soft, dreamy blur. */
      for (i = 0; i < 2; i++)
         grid_box_blur(img, tmp, dw, dh, 2);

      /* Greyed out: colours pulled part of the way towards grey. Notes the
       * brightest pixel on the way. */
      for (i = 0; i < dw * dh; i++)
      {
         float *p   = &img[i * 3];
         float luma = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];

         p[0] = luma + (p[0] - luma) * GRID_BG_SATURATION;
         p[1] = luma + (p[1] - luma) * GRID_BG_SATURATION;
         p[2] = luma + (p[2] - luma) * GRID_BG_SATURATION;

         if (luma > max_luma)
            max_luma = luma;
      }

      /* The whole image darkened by one factor, so hues and shading keep
       * their balance: at least GRID_BG_DIM, more if its brightest pixel
       * would still be over the cap. */
      k = GRID_BG_DIM;
      if (max_luma * k > GRID_BG_MAX_LUMA)
         k = GRID_BG_MAX_LUMA / max_luma;

      for (i = 0; i < dw * dh; i++)
      {
         float r = img[i * 3 + 0] * k;
         float g = img[i * 3 + 1] * k;
         float b = img[i * 3 + 2] * k;

         argb[i] = 0xff000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8)
            | (uint32_t)b;
      }

      *out_w = dw;
      *out_h = dh;
   }
   else
   {
      free(argb);
      argb = NULL;
   }

   free(img);
   free(tmp);
   return argb;
}

/* Uploads an ARGB buffer (converted in place for RGBA drivers). */
static bool grid_bg_upload(uint32_t *argb, unsigned w, unsigned h,
      grid_bg_t *out)
{
   struct texture_image ti;
   unsigned i;

   if (video_driver_supports_rgba())
      for (i = 0; i < w * h; i++)
      {
         uint32_t p = argb[i];
         /* RGBA-capable drivers take R in the low byte. */
         argb[i] = (p & 0xff00ff00) | ((p >> 16) & 0xff) | ((p & 0xff) << 16);
      }

   ti.pixels        = argb;
   ti.width         = w;
   ti.height        = h;
   ti.supports_rgba = video_driver_supports_rgba();

   video_driver_texture_load(&ti, TEXTURE_FILTER_LINEAR, &out->tex);
   out->w = w;
   out->h = h;
   return out->tex != 0;
}

/* "<dir>/1234.png" -> "<dir>/1234.bg8.png": the blurred background
 * cached next to its hero. */
static void grid_bg_cache_path(const char *hero, char *s, size_t len)
{
   strlcpy(s, hero, len);
   path_remove_extension(s);
   strlcat(s, GRID_BG_CACHE_EXT, len);
}

/* Blurs a hero image and saves the result next to it. Returns the ARGB
 * buffer (free() it), or NULL. */
static uint32_t *grid_bg_make_hero(const char *hero, unsigned *w, unsigned *h)
{
   struct texture_image ti;
   char cache[PATH_MAX_LENGTH];
   uint32_t *argb;

   ti.width         = 0;
   ti.height        = 0;
   ti.pixels        = NULL;
   ti.supports_rgba = false; /* ARGB, as grid_bg_read_image expects */

   if (!image_texture_load(&ti, hero))
      return NULL;
   argb = grid_bg_make(grid_bg_read_image, &ti, ti.width, ti.height, w, h);
   image_texture_free(&ti);

   if (argb)
   {
      /* Written under a temporary name, then renamed, so a reader never
       * sees a half-written file (the blur thread and the main thread can
       * both get here). */
      char tmp[PATH_MAX_LENGTH + 8];
      grid_bg_cache_path(hero, cache, sizeof(cache));
      snprintf(tmp, sizeof(tmp), "%s.tmp", cache);
      if (rpng_save_image_argb(tmp, argb, *w, *h, *w * sizeof(uint32_t)))
      {
         filestream_delete(cache);
         filestream_rename(tmp, cache);
      }
   }
   return argb;
}

/* ---- Blurring ahead of time ------------------------------------------ */

/* Heroes without a cached blur are blurred on a worker thread, a batch at
 * a time, so the main thread never decodes a full-size banner. The main
 * thread sweeps the games a few per frame to find them. (Quitting while a
 * batch runs is safe: files are written under a temporary name first.) */
#define GRID_BLUR_BATCH 32

typedef struct
{
   char paths[GRID_BLUR_BATCH][PATH_MAX_LENGTH];
   unsigned count;
   volatile bool done;
   sthread_t *thread;
} grid_blur_job_t;

static grid_blur_job_t *grid_blur_job;      /* running batch, or NULL */
static grid_blur_job_t *grid_blur_next;     /* batch being filled */
static size_t grid_blur_sweep;              /* next game to check */

static void grid_blur_thread(void *data)
{
   grid_blur_job_t *job = (grid_blur_job_t*)data;
   unsigned i, w, h;

   for (i = 0; i < job->count; i++)
      free(grid_bg_make_hero(job->paths[i], &w, &h));
   job->done = true;
}

static bool grid_blur_queued(const char *hero)
{
   unsigned i;
   if (grid_blur_next)
      for (i = 0; i < grid_blur_next->count; i++)
         if (string_is_equal(grid_blur_next->paths[i], hero))
            return true;
   if (grid_blur_job)
      for (i = 0; i < grid_blur_job->count; i++)
         if (string_is_equal(grid_blur_job->paths[i], hero))
            return true;
   return false;
}

/* Per frame: check a few games for a downloaded hero without a cached
 * blur, and hand batches to the worker. */
static void grid_blur_poll(void)
{
   unsigned k;

   /* Batch finished: free it. */
   if (grid_blur_job && grid_blur_job->done)
   {
      sthread_join(grid_blur_job->thread);
      free(grid_blur_job);
      grid_blur_job = NULL;
   }

   for (k = 0; k < 8 && grid_st.count; k++)
   {
      grid_game_t *game = &grid_st.games[grid_blur_sweep++ % grid_st.count];
      char hero[PATH_MAX_LENGTH], cache[PATH_MAX_LENGTH];

      if (game->bg_ready || !grid_cover_hero_peek(game->title,
               hero, sizeof(hero)))
         continue;

      grid_bg_cache_path(hero, cache, sizeof(cache));
      if (path_is_valid(cache))
      {
         game->bg_ready = true;
         continue;
      }

      if (grid_blur_queued(hero))
         continue;
      if (!grid_blur_next)
         grid_blur_next = (grid_blur_job_t*)calloc(1, sizeof(*grid_blur_next));
      if (!grid_blur_next || grid_blur_next->count >= GRID_BLUR_BATCH)
         break;
      strlcpy(grid_blur_next->paths[grid_blur_next->count++], hero,
            sizeof(grid_blur_next->paths[0]));
   }

   /* Start the next batch when the worker is free. */
   if (!grid_blur_job && grid_blur_next && grid_blur_next->count)
   {
      grid_blur_job  = grid_blur_next;
      grid_blur_next = NULL;
      if (!(grid_blur_job->thread = sthread_create(grid_blur_thread,
                  grid_blur_job)))
      {
         free(grid_blur_job);
         grid_blur_job = NULL;
      }
   }
}

/* Whether the paused game's last frame is readable. Only software-rendered
 * cores keep a CPU copy; hardware cores don't. */
static bool grid_bg_game_available(void)
{
   video_driver_state_t *video_st = video_state_get_ptr();
   return video_st->frame_cache_data
      && video_st->frame_cache_data != RETRO_HW_FRAME_BUFFER_VALID
      && video_st->frame_cache_width && video_st->frame_cache_height
      && video_st->frame_cache_pitch;
}

/* Builds the background for key: GRID_BG_GAME (paused game) or a hero
 * image path (using its cached blurred version when there is one). */
static bool grid_bg_load(const char *key, grid_bg_t *out)
{
   uint32_t *argb = NULL;
   unsigned w = 0, h = 0;
   bool ok    = false;

   if (string_is_equal(key, GRID_BG_GAME))
   {
      video_driver_state_t *video_st = video_state_get_ptr();
      if (!grid_bg_game_available())
         return false;
      argb = grid_bg_make(grid_bg_read_frame, video_st,
            video_st->frame_cache_width, video_st->frame_cache_height,
            &w, &h);
   }
   else
   {
      struct texture_image ti;
      char cache[PATH_MAX_LENGTH];

      grid_bg_cache_path(key, cache, sizeof(cache));

      ti.width         = 0;
      ti.height        = 0;
      ti.pixels        = NULL;
      ti.supports_rgba = video_driver_supports_rgba();

      if (path_is_valid(cache) && image_texture_load(&ti, cache))
      {
         /* Not this build's blur (wrong width): made again below. */
         if (ti.width != GRID_BG_WIDTH)
         {
            image_texture_free(&ti);
            filestream_delete(cache);
         }
         else
         {
            video_driver_texture_load(&ti, TEXTURE_FILTER_LINEAR, &out->tex);
            out->w = ti.width;
            out->h = ti.height;
            image_texture_free(&ti);
            return out->tex != 0;
         }
      }

      argb = grid_bg_make_hero(key, &w, &h);
   }

   if (argb)
      ok = grid_bg_upload(argb, w, h, out);
   free(argb);
   return ok;
}

/* Puts a hero at the front of the blur worker's next batch (if it isn't
 * on its way already), for a selection that got ahead of the sweep. */
static void grid_blur_request(const char *hero)
{
   if (grid_blur_queued(hero))
      return;
   if (!grid_blur_next)
      grid_blur_next = (grid_blur_job_t*)calloc(1, sizeof(*grid_blur_next));
   if (!grid_blur_next)
      return;

   if (grid_blur_next->count >= GRID_BLUR_BATCH)
      grid_blur_next->count = GRID_BLUR_BATCH - 1; /* drop the last */
   memmove(grid_blur_next->paths[1], grid_blur_next->paths[0],
         grid_blur_next->count * sizeof(grid_blur_next->paths[0]));
   strlcpy(grid_blur_next->paths[0], hero, sizeof(grid_blur_next->paths[0]));
   grid_blur_next->count++;
}

/* ---- Background textures, preloaded ----------------------------------- */

/* Blurred backgrounds already uploaded, keyed by hero path. The games
 * around the selection are loaded ahead of time (one per frame), so
 * moving the selection swaps the background without touching the disk. */
#define GRID_BG_TEXCACHE 48

static struct
{
   char key[PATH_MAX_LENGTH];
   grid_bg_t bg;
   unsigned used;
} grid_bg_tex[GRID_BG_TEXCACHE];
static unsigned grid_bg_clock;

static void grid_bg_tex_free_all(void)
{
   unsigned i;
   for (i = 0; i < GRID_BG_TEXCACHE; i++)
   {
      grid_bg_free(&grid_bg_tex[i].bg);
      memset(&grid_bg_tex[i], 0, sizeof(grid_bg_tex[i]));
   }
}

static grid_bg_t *grid_bg_tex_lookup(const char *key)
{
   unsigned i;
   for (i = 0; i < GRID_BG_TEXCACHE; i++)
      if (grid_bg_tex[i].bg.tex && string_is_equal(grid_bg_tex[i].key, key))
      {
         grid_bg_tex[i].used = ++grid_bg_clock;
         return &grid_bg_tex[i].bg;
      }
   return NULL;
}

/* The texture for a hero, loading its cached blur into the least
 * recently used slot if needed. NULL if it can't be loaded. */
static grid_bg_t *grid_bg_tex_fetch(const char *key)
{
   unsigned i, slot = 0;
   grid_bg_t *bg    = grid_bg_tex_lookup(key);

   if (bg)
      return bg;

   for (i = 1; i < GRID_BG_TEXCACHE; i++)
      if (grid_bg_tex[i].used < grid_bg_tex[slot].used)
         slot = i;

   grid_bg_free(&grid_bg_tex[slot].bg);
   grid_bg_tex[slot].key[0] = '\0';
   if (!grid_bg_load(key, &grid_bg_tex[slot].bg))
      return NULL;

   strlcpy(grid_bg_tex[slot].key, key, sizeof(grid_bg_tex[slot].key));
   grid_bg_tex[slot].used = ++grid_bg_clock;
   return &grid_bg_tex[slot].bg;
}

/* Whether a hero's blur is cached on disk (so loading it is cheap). */
static bool grid_bg_hero_ready(const char *hero)
{
   char cache[PATH_MAX_LENGTH];
   grid_bg_cache_path(hero, cache, sizeof(cache));
   return path_is_valid(cache);
}

/* Loads one not-yet-loaded background among the games within a screen's
 * width of the visible columns (nearest the selection first). */
static void grid_bg_prefetch(size_t first_col, size_t cols_visible)
{
   size_t lo   = first_col > cols_visible ? first_col - cols_visible : 0;
   size_t hi   = (first_col + 2 * cols_visible + 1) * grid_st.rows;
   size_t sel  = grid_st.selection;
   size_t step;

   if (hi > grid_st.view_count)
      hi = grid_st.view_count;
   lo *= grid_st.rows;
   if (lo >= hi || sel < lo || sel >= hi)
      return;

   /* Spiral out from the selection: sel, sel+1, sel-1, sel+2, ... */
   for (step = 0; step < 2 * (hi - lo) + 1; step++)
   {
      size_t off = (step + 1) / 2;
      size_t i;
      char hero[PATH_MAX_LENGTH];
      grid_game_t *game;

      if (step & 1)
      {
         if (sel + off >= hi)
            continue;
         i = sel + off;
      }
      else
      {
         if (sel < lo + off)
            continue;
         i = sel - off;
      }

      game = &grid_st.games[grid_st.view[i]];
      if (     !game->bg_ready
            || !grid_cover_hero_peek(game->title, hero, sizeof(hero))
            || grid_bg_tex_lookup(hero))
         continue;

      grid_bg_tex_fetch(hero);
      return;
   }
}

/* Picks the background for the selected game, switching the moment the
 * selection moves: the game's blurred SteamGridDB hero (usually
 * preloaded). */
static void grid_bg_update(const grid_game_t *game)
{
   char want[PATH_MAX_LENGTH];

   /* Choosing a wallpaper: preview the hovered one (its small preview
    * image, quick to blur on the spot). */
   if (     grid_st.panel == PANEL_ART && grid_st.art_kind == GRID_CHOICE_HERO
         && grid_st.panel_sel < grid_cover_choices_count())
   {
      const char *thumb = grid_cover_choice_thumb(grid_st.panel_sel);
      if (!string_is_empty(thumb))
      {
         if (!string_is_equal(thumb, grid_st.bg_key)
               && grid_bg_tex_fetch(thumb))
         {
            strlcpy(grid_st.bg_prev, grid_st.bg_key, sizeof(grid_st.bg_prev));
            grid_st.bg_fade_start = cpu_features_get_time_usec();
            strlcpy(grid_st.bg_key, thumb, sizeof(grid_st.bg_key));
         }
         return;
      }
   }

   /* Always the game's set wallpaper, even for the running game. */
   want[0] = '\0';
   if (game && grid_cover_hero_get(game->title, want, sizeof(want))
         == GRID_META_PENDING)
      return; /* keep the current background until it arrives */

   if (string_is_equal(want, grid_st.bg_key))
      return;

   if (!string_is_empty(want) && !grid_bg_tex_lookup(want))
   {
      /* Not blurred yet: have the worker do it next, and keep the current
       * background meanwhile rather than decoding the banner here. */
      if (!grid_bg_hero_ready(want))
      {
         grid_blur_request(want);
         return;
      }
      grid_bg_tex_fetch(want);
   }

   /* The old background fades out under the new one. */
   strlcpy(grid_st.bg_prev, grid_st.bg_key, sizeof(grid_st.bg_prev));
   grid_st.bg_fade_start = cpu_features_get_time_usec();
   strlcpy(grid_st.bg_key, want, sizeof(grid_st.bg_key));
}

/* The texture for a background key ("" or not loaded: NULL). */
static grid_bg_t *grid_bg_for_key(const char *key)
{
   grid_bg_t *bg = NULL;

   if (string_is_equal(key, GRID_BG_GAME))
      bg = &grid_st.bg;
   else if (!string_is_empty(key))
      bg = grid_bg_tex_lookup(key);

   return (bg && bg->tex && bg->w && bg->h) ? bg : NULL;
}

/* Draws a background covering the screen (keeping its aspect ratio) at
 * the given opacity. */
static void grid_bg_draw(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, grid_bg_t *bg, float alpha)
{
   float color[16];
   float s, bw, bh;

   if (!bg || alpha <= 0.0f)
      return;

   s = (float)width / bg->w;
   if (bg->h * s < height)
      s = (float)height / bg->h;
   bw = bg->w * s;
   bh = bg->h * s;

   grid_set_color(color, 0xffffff00 | (uint32_t)(alpha * 255.0f + 0.5f));
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)((width - bw) / 2.0f), (int)((height - bh) / 2.0f),
         (unsigned)bw, (unsigned)bh, width, height, color, &bg->tex);
}

/* Dark gradient, with the selected game's blurred background over it,
 * cross-fading over GRID_BG_FADE when it changes. */
static void grid_draw_background(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, const grid_game_t *game)
{
   grid_bg_t *cur, *prev;
   float t;

   grid_bg_update(game);

   t = (cpu_features_get_time_usec() - grid_st.bg_fade_start)
      / (float)GRID_BG_FADE;
   if (t > 1.0f)
      t = 1.0f;
   if (t < 0.0f)
      t = 0.0f;

   grid_draw_vgradient(p_disp, userdata, width, height,
         0, 0, width, height, COL_BG_TOP, COL_BG);

   cur  = grid_bg_for_key(grid_st.bg_key);
   prev = t < 1.0f ? grid_bg_for_key(grid_st.bg_prev) : NULL;

   /* Previous fully under the new one, or fading out if there is none. */
   grid_bg_draw(p_disp, userdata, width, height, prev, cur ? 1.0f : 1.0f - t);
   grid_bg_draw(p_disp, userdata, width, height, cur, t);
}

/* ---------------------------------------------------------------------- */
/* Screen parts                                                           */
/* ---------------------------------------------------------------------- */

/* Thin vertical divider centred on (x, cy): a capsule (rounded ends) of
 * translucent white (COL_DIVIDER), so it lightens what is behind it rather than being a
 * flat grey. Rasterised at its exact pixel size for clean ends. */
static void grid_draw_divider(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, float x, float cy)
{
   float color[16];
   unsigned w = (unsigned)floorf(REF(1.15) + 0.5f);
   unsigned h = (unsigned)floorf(REF(11.5) + 0.5f);

   if (w < 1)
      w = 1;
   if (h < w)
      h = w;

   if (grid_divider_w != w || grid_divider_h != h)
   {
      struct texture_image ti;
      unsigned px, py;
      float r = w / 2.0f;

      if (grid_divider_tex)
         video_driver_texture_unload(&grid_divider_tex);
      grid_divider_tex = 0;
      grid_divider_w   = w;
      grid_divider_h   = h;

      ti.width         = w;
      ti.height        = h;
      ti.supports_rgba = video_driver_supports_rgba();
      if (!(ti.pixels = (uint32_t*)malloc(w * h * sizeof(uint32_t))))
         return;

      /* Distance to the capsule's centre line, minus its radius. */
      for (py = 0; py < h; py++)
         for (px = 0; px < w; px++)
         {
            float dx = px + 0.5f - r;
            float dy = fabsf(py + 0.5f - h / 2.0f) - (h / 2.0f - r);
            float d, a;
            if (dy < 0.0f)
               dy = 0.0f;
            d = sqrtf(dx * dx + dy * dy) - r;
            a = 0.5f - d;
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            ti.pixels[py * w + px] = ((uint32_t)(a * 255.0f + 0.5f) << 24)
               | 0x00ffffff;
         }

      video_driver_texture_load(&ti, TEXTURE_FILTER_NEAREST, &grid_divider_tex);
      free(ti.pixels);
   }

   if (!grid_divider_tex)
      return;

   grid_set_color(color, COL_DIVIDER);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)floorf(x - w / 2.0f + 0.5f), (int)floorf(cy - h / 2.0f + 0.5f),
         w, h, width, height, color, &grid_divider_tex);
}

static void grid_fit_text(bool bold, const char *text, float max_w,
      unsigned height, float scale, char *out, size_t out_len);

/* Square texture icon of the given size centred on (cx, cy). */
static void grid_draw_icon(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, uintptr_t *tex,
      float cx, float cy, float size, uint32_t rgba)
{
   float color[16];

   if (!*tex)
      return;
   grid_set_color(color, rgba);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)floorf(cx - size / 2.0f + 0.5f),
         (int)floorf(cy - size / 2.0f + 0.5f),
         (unsigned)size, (unsigned)size, width, height, color, tex);
}

/* Loads an icon's shape (its alpha channel) from a PNG. */
static void grid_icon_load(grid_icon_t *ic, const char *path)
{
   struct texture_image ti;
   unsigned i;

   ti.width         = 0;
   ti.height        = 0;
   ti.pixels        = NULL;
   ti.supports_rgba = false;

   if (!image_texture_load(&ti, path))
      return;

   if ((ic->alpha = (uint8_t*)malloc(ti.width * ti.height)))
   {
      unsigned x, y;
      unsigned x0 = ti.width, y0 = ti.height, x1 = 0, y1 = 0;

      /* Alpha is the top byte in ARGB. */
      for (i = 0; i < ti.width * ti.height; i++)
         ic->alpha[i] = (uint8_t)(ti.pixels[i] >> 24);
      ic->w = ti.width;
      ic->h = ti.height;

      /* Bounding box of the visible shape: icons are drawn centred on
       * it, not on the image (artwork is rarely exactly centred). */
      for (y = 0; y < ic->h; y++)
         for (x = 0; x < ic->w; x++)
            if (ic->alpha[y * ic->w + x] > 127)
            {
               if (x < x0) x0 = x;
               if (x > x1) x1 = x;
               if (y < y0) y0 = y;
               if (y > y1) y1 = y;
            }
      if (x1 >= x0 && y1 >= y0)
      {
         ic->off_x = ((x0 + x1 + 1) / 2.0f - ic->w / 2.0f) / ic->w;
         ic->off_y = ((y0 + y1 + 1) / 2.0f - ic->h / 2.0f) / ic->h;
      }
   }
   image_texture_free(&ti);
}

static void grid_icon_free(grid_icon_t *ic)
{
   if (ic->tex)
      video_driver_texture_unload(&ic->tex);
   if (ic->btn_tex)
      video_driver_texture_unload(&ic->btn_tex);
   free(ic->alpha);
   memset(ic, 0, sizeof(*ic));
}

/* Coverage (0-255) of the icon's shape over output pixel (x, y) when the
 * icon is scaled to ip x ip: average of the source block under it. */
static unsigned grid_icon_coverage(const grid_icon_t *ic, unsigned ip,
      unsigned x, unsigned y)
{
   unsigned x0 = x * ic->w / ip, x1 = (x + 1) * ic->w / ip;
   unsigned y0 = y * ic->h / ip, y1 = (y + 1) * ic->h / ip;
   unsigned sx, sy, sum = 0, n = 0;

   if (x1 <= x0) x1 = x0 + 1;
   if (y1 <= y0) y1 = y0 + 1;
   for (sy = y0; sy < y1 && sy < ic->h; sy++)
      for (sx = x0; sx < x1 && sx < ic->w; sx++)
      {
         sum += ic->alpha[sy * ic->w + sx];
         n++;
      }
   return n ? sum / n : 0;
}

/* Round icon button d pixels wide with its top-left at (x, y): a circle
 * of circle_rgba with the icon (frac of the circle wide) in icon_rgba
 * (alpha 0: cut out of the circle). Both are rasterised together into one texture at the exact size, so
 * the icon sits precisely in the middle of the circle. A notch of radius
 * nr (0: none) centred on (nx, ny), in pixels from the top-left, is cut
 * out of both, leaving room for a badge drawn there. */
static void grid_icon_button_draw_notch(gfx_display_t *p_disp,
      void *userdata, unsigned width, unsigned height, grid_icon_t *ic,
      float x, float y, unsigned d, float frac,
      uint32_t circle_rgba, uint32_t icon_rgba,
      float nx, float ny, float nr)
{
   float color[16];
   unsigned ip = (unsigned)(d * frac + 0.5f);

   if (!ic->alpha || !d)
      return;

   /* Same odd/even as the circle, so the margins are whole pixels. */
   if ((d - ip) & 1)
      ip++;
   if (ip > d)
      ip = d;

   if (     ic->btn_d != d || ic->btn_ip != ip || ic->btn_nx != nx
         || ic->btn_ny != ny || ic->btn_nr != nr)
   {
      struct texture_image ti;
      unsigned px, py;
      unsigned m = (d - ip) / 2; /* icon margin */
      float r    = d / 2.0f;
      float cr   = ((circle_rgba >> 24) & 0xff), cg = ((circle_rgba >> 16) & 0xff);
      float cb   = ((circle_rgba >> 8) & 0xff);
      float ir   = ((icon_rgba >> 24) & 0xff), ig = ((icon_rgba >> 16) & 0xff);
      float ib   = ((icon_rgba >> 8) & 0xff);
      bool rgba  = video_driver_supports_rgba();

      if (ic->btn_tex)
         video_driver_texture_unload(&ic->btn_tex);
      ic->btn_tex = 0;
      ic->btn_d   = d;
      ic->btn_ip  = ip;
      ic->btn_nx  = nx;
      ic->btn_ny  = ny;
      ic->btn_nr  = nr;

      ti.width         = d;
      ti.height        = d;
      ti.supports_rgba = rgba;
      if (!(ti.pixels = (uint32_t*)malloc(d * d * sizeof(uint32_t))))
         return;

      for (py = 0; py < d; py++)
         for (px = 0; px < d; px++)
         {
            float dx = px + 0.5f - r, dy = py + 0.5f - r;
            float ac = r - sqrtf(dx * dx + dy * dy) + 0.5f; /* circle */
            float ai = 0.0f;                                /* icon */
            uint32_t R, G, B, A;

            float ca, ia, oa;

            if (ac < 0.0f) ac = 0.0f;
            if (ac > 1.0f) ac = 1.0f;
            if (px >= m && py >= m && px < m + ip && py < m + ip)
               ai = grid_icon_coverage(ic, ip, px - m, py - m) / 255.0f;

            /* Icon over circle, each with its own opacity (the circle
             * may be translucent); a fully transparent icon colour cuts
             * the icon out of the circle instead. */
            ca = ac * (circle_rgba & 0xff) / 255.0f;
            if (!(icon_rgba & 0xff))
               ca *= 1.0f - ai;
            ia = ai * ac * (icon_rgba & 0xff) / 255.0f;
            if (nr > 0.0f)
            {
               float ndx  = px + 0.5f - nx, ndy = py + 0.5f - ny;
               float keep = sqrtf(ndx * ndx + ndy * ndy) - nr + 0.5f;
               if (keep < 0.0f) keep = 0.0f;
               if (keep > 1.0f) keep = 1.0f;
               ca *= keep;
               ia *= keep;
            }
            oa = ia + ca * (1.0f - ia);

            if (oa > 0.0f)
            {
               R = (uint32_t)((ir * ia + cr * ca * (1.0f - ia)) / oa + 0.5f);
               G = (uint32_t)((ig * ia + cg * ca * (1.0f - ia)) / oa + 0.5f);
               B = (uint32_t)((ib * ia + cb * ca * (1.0f - ia)) / oa + 0.5f);
            }
            else
               R = G = B = 0;
            A = (uint32_t)(oa * 255.0f + 0.5f);

            /* RGBA-capable drivers take R in the low byte, others ARGB. */
            ti.pixels[py * d + px] = rgba
               ? (A << 24) | (B << 16) | (G << 8) | R
               : (A << 24) | (R << 16) | (G << 8) | B;
         }

      video_driver_texture_load(&ti, TEXTURE_FILTER_NEAREST, &ic->btn_tex);
      free(ti.pixels);
   }

   if (!ic->btn_tex)
      return;

   grid_set_color(color, 0xffffffff);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)floorf(x + 0.5f), (int)floorf(y + 0.5f), d, d,
         width, height, color, &ic->btn_tex);
}

static void grid_icon_button_draw(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, grid_icon_t *ic,
      float x, float y, unsigned d, float frac,
      uint32_t circle_rgba, uint32_t icon_rgba)
{
   grid_icon_button_draw_notch(p_disp, userdata, width, height, ic,
         x, y, d, frac, circle_rgba, icon_rgba, 0.0f, 0.0f, 0.0f);
}

/* Draws an icon size pixels wide (tinted) centred on (cx, cy), from a
 * texture rasterised at exactly that size and placed on whole pixels. */
static void grid_icon_draw(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, grid_icon_t *ic,
      float cx, float cy, float size, uint32_t rgba)
{
   float color[16];
   unsigned px = (unsigned)(size + 0.5f);
   unsigned ph;
   float left;

   if (!ic->alpha || !px)
      return;

   /* Same odd/even as the space it sits in, so it can be exactly
    * centred on whole pixels (otherwise it rounds half a pixel off). */
   cx  -= ic->off_x * px;
   cy  -= ic->off_y * px;
   left = cx - px / 2.0f;
   if (fabsf(left - floorf(left) - 0.5f) < 0.25f)
      px++;

   ph = (unsigned)((float)px * ic->h / ic->w + 0.5f);
   if (!ph)
      ph = 1;
   {
      float top = cy - ph / 2.0f;
      if (fabsf(top - floorf(top) - 0.5f) < 0.25f)
         ph++;
   }

   if (ic->px != px || ic->ph != ph)
   {
      struct texture_image ti;
      unsigned x, y;

      if (ic->tex)
         video_driver_texture_unload(&ic->tex);
      ic->tex = 0;
      ic->px  = px;
      ic->ph  = ph;

      ti.width         = px;
      ti.height        = ph;
      ti.supports_rgba = video_driver_supports_rgba();
      if (!(ti.pixels = (uint32_t*)malloc(px * ph * sizeof(uint32_t))))
         return;

      /* Area average: each output pixel covers a block of the source. */
      for (y = 0; y < ph; y++)
      {
         unsigned y0 = y * ic->h / ph, y1 = (y + 1) * ic->h / ph;
         if (y1 <= y0) y1 = y0 + 1;
         for (x = 0; x < px; x++)
         {
            unsigned x0 = x * ic->w / px, x1 = (x + 1) * ic->w / px;
            unsigned sx, sy, sum = 0, n = 0;
            if (x1 <= x0) x1 = x0 + 1;
            for (sy = y0; sy < y1 && sy < ic->h; sy++)
               for (sx = x0; sx < x1 && sx < ic->w; sx++)
               {
                  sum += ic->alpha[sy * ic->w + sx];
                  n++;
               }
            /* White: the same in RGBA and ARGB, so only alpha matters. */
            ti.pixels[y * px + x] = ((n ? sum / n : 0) << 24) | 0x00ffffff;
         }
      }

      video_driver_texture_load(&ti, TEXTURE_FILTER_NEAREST, &ic->tex);
      free(ti.pixels);
   }

   if (!ic->tex)
      return;

   grid_set_color(color, rgba);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)floorf(cx - px / 2.0f + 0.5f),
         (int)floorf(cy - ph / 2.0f + 0.5f),
         px, ph, width, height, color, &ic->tex);
}

static void grid_free_profile_textures(void)
{
   unsigned i;
   for (i = 0; i < GRID_PROFILE_MAX; i++)
   {
      if (grid_st.profile_tex[i])
         video_driver_texture_unload(&grid_st.profile_tex[i]);
      grid_st.profile_tex[i]         = 0;
      grid_st.profile_tex_path[i][0] = '\0';
   }
}

/* A profile's avatar, d wide at (x, y): its picture clipped to a circle,
 * or its initial on its colour. */
static void grid_draw_profile_avatar(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, unsigned idx,
      float x, float y, float d)
{
   const char *icon = grid_profile_icon(idx);
   const char *name = grid_profile_name(idx);

   if (idx >= GRID_PROFILE_MAX)
      return;

   /* (Re)load the picture when it changed. */
   if (!string_is_equal(icon, grid_st.profile_tex_path[idx]))
   {
      unsigned w, h;
      if (grid_st.profile_tex[idx])
         video_driver_texture_unload(&grid_st.profile_tex[idx]);
      grid_st.profile_tex[idx] = 0;
      strlcpy(grid_st.profile_tex_path[idx], icon,
            sizeof(grid_st.profile_tex_path[idx]));
      if (!string_is_empty(icon))
         grid_load_rounded_texture(icon, &grid_st.profile_tex[idx],
               &w, &h, 0.5f, 1.0f);
   }

   if (grid_st.profile_tex[idx])
   {
      grid_draw_icon(p_disp, userdata, width, height,
            &grid_st.profile_tex[idx], x + d / 2.0f, y + d / 2.0f, d,
            0xffffffff);
      return;
   }

   grid_draw_rounded(p_disp, userdata, width, height,
         x, y, d, d, d / 2.0f, grid_profile_color(idx));
   grid_draw_char_centered(true,
         (char)toupper((unsigned char)(name[0] ? name[0] : '?')),
         x + d / 2.0f, y + d / 2.0f, width, height, 0xffffffff,
         d * 0.42f / (height / 32.0f) / GRID_FONT_CAP);
}

/* Top row: icon bar on the left (profile, settings, installs, search; the
 * search icon grows into a search box while searching); on the right
 * "[L] console count", a divider and "[R] sort order". No bar behind it:
 * it sits on the background. */
static void grid_draw_top_bar(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height)
{
   /* All positions/sizes in reference pixels (see REF/CAP). */
   float side         = REF(20);
   float row1         = REF(31);          /* row centre */
   /* Screens that take the grid's place (Settings, achievements, art,
    * pictures, Installs) can't reach the icon bar: no clock, profile or
    * icons there, leaving the row to the screen. */
   bool full_screen   = grid_settings_shown()
                     || grid_st.panel == PANEL_CHEEVOS
                     || grid_st.panel == PANEL_ART
                     || grid_st.panel == PANEL_ICONS
                     || grid_st.panel == PANEL_UPDATES
                     || grid_st.panel == PANEL_SETUP;

   /* Icon bar: 27px circles, 9px apart. */
   if (!full_screen)
   {
      const char *search = grid_search_text();
      bool typing        = menu_input_dialog_get_display_kb()
                           && grid_st.kb == KB_SEARCH;
      float d            = 2.0f * floorf(REF(27) / 2.0f) + 1.0f;
      float gap          = REF(9);
      float y            = floorf(row1 - d / 2.0f + 0.5f);
      float x            = floorf(side);
      float open_w       = REF(230);
      float target       = (typing || !string_is_empty(search)) ? 1.0f : 0.0f;
      unsigned i;
      float region_end;  /* right edge of the clock + profile region */

      /* Search box grows/shrinks smoothly. */
      grid_st.search_open += (target - grid_st.search_open) * 0.3f;
      if (fabsf(target - grid_st.search_open) < 0.01f)
         grid_st.search_open = target;

      /* Clock, a divider, then the profile button. While the profiles
       * menu is open, a strip of Exit, all profiles and Add grows sideways
       * from the profile button (the clock stays), pushing the other
       * buttons right. */
      {
         char clock[16];
         time_t now_t      = time(NULL);
         struct tm *tm_now = localtime(&now_t);
         float ts          = CAP(GRID_TEXT_CAP);
         float sp          = gap / 2.0f + REF(6); /* each side of a divider */
         float x0          = x;
         float cw          = 0.0f;
         float pad         = floorf(REF(4) + 0.5f); /* strip padding */
         float igap        = REF(8);                /* between profiles */
         unsigned items    = grid_profile_items();
         float strip_w     = 2.0f * pad + items * d + (items - 1) * igap;
         float closed_end, open_end, open;
         float target_open = (grid_st.panel == PANEL_PROFILES
               || grid_st.panel == PANEL_ICONS) ? 1.0f : 0.0f;

         grid_st.profile_open += (target_open - grid_st.profile_open) * 0.3f;
         if (fabsf(target_open - grid_st.profile_open) < 0.01f)
            grid_st.profile_open = target_open;
         open = grid_st.profile_open;

         clock[0] = '\0';
         if (tm_now && strftime(clock, sizeof(clock), "%H:%M", tm_now))
         {
            /* A fixed slot as wide as the widest possible time, so the
             * buttons after it don't shift as the digits change. */
            char digit[2] = { 0, 0 };
            float md      = 0.0f;
            char c;
            for (c = '0'; c <= '9'; c++)
            {
               float dw;
               digit[0] = c;
               if ((dw = grid_text_width(digit, height, ts)) > md)
                  md = dw;
            }
            cw = 4.0f * md + grid_text_width(":", height, ts);
         }

         /* The clock always shows; the strip grows from the profile
          * button, after the clock's divider. */
         if (!string_is_empty(clock))
         {
            grid_draw_text(clock, x0, grid_text_y(row1, height, ts),
                  width, height, COL_TEXT, TEXT_ALIGN_LEFT, ts);
            grid_draw_divider(p_disp, userdata, width, height,
                  x0 + cw + sp, row1);
            x0 = floorf(x0 + cw + 2.0f * sp + 0.5f);
         }

         closed_end = x0 + d;
         open_end   = x0 - pad + strip_w;
         region_end = closed_end + (open_end - closed_end) * open;

         if (open > 0.0f)
         {
            unsigned k;
            float sx = x0 - pad;
            float ix = x0;

            grid_draw_shape(p_disp, userdata, width, height,
                  sx, y - pad, region_end - sx, d + 2.0f * pad,
                  (d + 2.0f * pad) / 2.0f, COL_DIVIDER);

            /* Contents once the strip has (nearly) opened. */
            for (k = 0; open > 0.9f && k < items; k++)
            {
               float kx = floorf(ix + k * (d + igap) + 0.5f);

               bool sel = grid_st.panel == PANEL_PROFILES
                     && k == grid_st.panel_sel;

               if (grid_st.panel == PANEL_PROFILES)
                  grid_hit_add(kx, y, d, d, HIT_PANEL, k, MENU_ACTION_OK);

               if (k == GRID_STRIP_EXIT)
               {
                  /* Held: the cursor turns into a red ring that fills
                   * round it, and back when let go. */
                  bool holding = grid_st.exit_hold_start != 0;
                  float target = holding ? 1.0f : 0.0f;

                  grid_st.exit_blend += (target - grid_st.exit_blend) * 0.08f;
                  if (fabsf(target - grid_st.exit_blend) < 0.01f)
                     grid_st.exit_blend = target;
                  if (holding)
                  {
                     grid_st.exit_hold_t = (cpu_features_get_time_usec()
                           - grid_st.exit_hold_start) / (float)GRID_EXIT_HOLD;
                     /* The full ring is on screen: quitting may follow. */
                     if (     grid_st.exit_hold_t >= 1.0f && sel
                           && !grid_st.exit_full_at)
                        grid_st.exit_full_at = cpu_features_get_time_usec();
                  }
                  if (!sel)
                     grid_st.exit_blend = 0.0f;

                  if (sel && grid_st.exit_blend > 0.0f)
                     grid_draw_hold_ring(p_disp, userdata, width, height,
                           kx, y, d, grid_st.exit_hold_t, grid_st.exit_blend);
                  else if (sel)
                     grid_draw_cursor(p_disp, userdata, width, height,
                           kx, y, d, d, d / 2.0f);

                  grid_icon_button_draw(p_disp, userdata, width, height,
                        &grid_st.icon_exit, kx, y, (unsigned)d, 0.5f,
                        COL_DIVIDER, COL_TEXT);
               }
               else if (sel)
                  grid_draw_cursor(p_disp, userdata, width, height,
                        kx, y, d, d, d / 2.0f);

               if (k == GRID_STRIP_EXIT)
                  continue;
               if (k - GRID_STRIP_FIRST < grid_profile_count())
                  grid_draw_profile_avatar(p_disp, userdata, width, height,
                        k - GRID_STRIP_FIRST, kx, y, d);
               else
               {
                  /* Add: "+" made of two bars on a faint circle. */
                  float t   = 2.0f * floorf(d * 0.04f) + 1.0f;
                  float len = 2.0f * floorf(d * 0.2f) + 1.0f;
                  grid_draw_shape(p_disp, userdata, width, height,
                        kx, y, d, d, d / 2.0f, COL_DIVIDER);
                  grid_draw_rect(p_disp, userdata, width, height,
                        kx + (d - len) / 2.0f, y + (d - t) / 2.0f, len, t,
                        COL_TEXT);
                  grid_draw_rect(p_disp, userdata, width, height,
                        kx + (d - t) / 2.0f, y + (d - len) / 2.0f, t, len,
                        COL_TEXT);
               }
            }

            /* Name of the selected profile (or why it can't switch) in a
             * small label under the strip. */
            if (open > 0.9f && grid_st.panel == PANEL_PROFILES)
            {
               unsigned s   = grid_st.panel_sel;
               const char *label = s == GRID_STRIP_EXIT ? "Exit RetroArch"
                  : s - GRID_STRIP_FIRST < grid_profile_count()
                  ? grid_profile_name(s - GRID_STRIP_FIRST) : "Add profile";
               float ls     = CAP(GRID_TEXT_CAP - 2);
               float lcx    = floorf(ix + s * (d + igap) + 0.5f) + d / 2.0f;
               float lcy    = y + d + pad + REF(13);
               float lw     = grid_text_width(label, height, ls);
               float lh     = REF(15);
               float lpad   = REF(8);
               float lx     = lcx - lw / 2.0f - lpad;

               if (lx < x0 - pad)
                  lx = x0 - pad;
               grid_draw_shape(p_disp, userdata, width, height,
                     lx, lcy - lh / 2.0f, lw + 2.0f * lpad, lh, lh / 2.0f,
                     0x000000b0);
               grid_draw_text(label, lx + lpad, grid_text_y(lcy, height, ls),
                     width, height, COL_TEXT, TEXT_ALIGN_LEFT, ls);

               if (     grid_content_running() && s != GRID_STRIP_EXIT
                     && s - GRID_STRIP_FIRST != grid_profile_current())
                  grid_draw_text(s - GRID_STRIP_FIRST < grid_profile_count()
                        ? "Switching closes the running game"
                        : "Close the running game to add profiles",
                        x0 - pad, grid_text_y(lcy + lh, height, ls),
                        width, height, COL_TEXT_DIM, TEXT_ALIGN_LEFT, ls);
            }
         }

         /* The profile button sits at the region's end (when shown). */
         x = region_end - d;
      }

      for (i = 0; i < BAR_COUNT; i++)
      {
         float w;
         bool  sel;

         /* Whole pixels, so the circle, its icon and the ring agree. */
         x   = floorf(x + 0.5f);
         w   = d;
         sel = grid_st.bar_focus && grid_st.bar_sel == i
               && grid_st.panel == PANEL_NONE && !grid_settings_shown();

         if (i == BAR_SEARCH)
            w = d + (open_w - d) * grid_st.search_open;

         if (     grid_st.panel == PANEL_NONE
               && !(i == BAR_PROFILE && grid_st.profile_open > 0.0f))
            grid_hit_add(x, y, w, d, HIT_BAR, i, MENU_ACTION_OK);

         /* (Installs with its dot draws its own, notched.) */
         if (sel && !(i == BAR_UPDATES && grid_updates_available()))
            grid_draw_cursor(p_disp, userdata, width, height,
                  x, y, w, d, d / 2.0f);

         switch (i)
         {
            case BAR_PROFILE:
               /* Hidden under the profiles strip while it is open. */
               if (grid_st.profile_open <= 0.0f)
                  grid_draw_profile_avatar(p_disp, userdata, width, height,
                        grid_profile_current(), x, y, d);
               break;
            case BAR_SETTINGS:
               grid_icon_button_draw(p_disp, userdata, width, height,
                     &grid_st.icon_settings, x, y, (unsigned)d, 0.6f,
                     COL_DIVIDER, COL_TEXT);
               break;
            case BAR_UPDATES:
            {
               /* A blue dot at the top right, set in a see-through notch
                * cut out of the button (and of the selection ring round
                * it): while updates wait. */
               grid_icon_t *ic = &grid_st.icon_download;
               bool dot        = grid_updates_available() > 0;
               float frac      = 0.53f;
               if (dot)
               {
                  /* Centred on the circle's edge, up and right at 45
                   * degrees (0.7071 = cos 45). */
                  float dd = 2.0f * floorf(REF(9) / 2.0f) + 1.0f;
                  float b  = floorf(REF(2) + 0.5f);
                  float dx = floorf(x + d / 2.0f * (1.0f + 0.7071f)
                        - dd / 2.0f + 0.5f);
                  float dy = floorf(y + d / 2.0f * (1.0f - 0.7071f)
                        - dd / 2.0f + 0.5f);
                  float nx = dx - x + dd / 2.0f, ny = dy - y + dd / 2.0f;
                  float nr = dd / 2.0f + b;
                  if (sel)
                     grid_draw_cursor_notch(p_disp, userdata, width, height,
                           x, y, d, d, d / 2.0f, nx, ny, nr);
                  grid_icon_button_draw_notch(p_disp, userdata, width,
                        height, ic, x, y, (unsigned)d,
                        frac, COL_DIVIDER, COL_TEXT, nx, ny, nr);
                  grid_draw_rounded(p_disp, userdata, width, height,
                        dx, dy, dd, dd, dd / 2.0f, COL_SELECT);
               }
               else
                  grid_icon_button_draw(p_disp, userdata, width, height,
                        ic, x, y, (unsigned)d, frac,
                        COL_DIVIDER, COL_TEXT);
               break;
            }
            case BAR_SEARCH:
            {
               grid_draw_shape(p_disp, userdata, width, height,
                     x, y, w, d, d / 2.0f, COL_DIVIDER);
               /* Centred on the snapped circle's pixel centre (x and y are
                * whole pixels, d is odd). */
               grid_icon_draw(p_disp, userdata, width, height,
                     &grid_st.icon_search, x + d / 2.0f, y + d / 2.0f,
                     d * 0.45f, COL_TEXT);

               /* Text once the box is mostly open. */
               if (grid_st.search_open > 0.6f)
               {
                  float ts = CAP(GRID_TEXT_CAP);
                  float tx = x + d;
                  float tw = w - d - d / 2.0f;

                  /* Typing: with its caret (and selection). */
                  if (string_is_empty(search))
                     grid_draw_text("Search games",
                           tx, grid_text_y(row1, height, ts), width, height,
                           COL_TEXT_DIM, TEXT_ALIGN_LEFT, ts);
                  if (typing && tw > 0.0f)
                     grid_draw_edit(p_disp, userdata, width, height, search,
                           tx, row1, tw, ts, COL_TEXT);
                  else if (!string_is_empty(search) && tw > 0.0f)
                  {
                     char fit[160];
                     grid_fit_text(false, search, tw, height, ts,
                           fit, sizeof(fit));
                     grid_draw_text(fit, tx, grid_text_y(row1, height, ts),
                           width, height, COL_TEXT, TEXT_ALIGN_LEFT, ts);
                  }
               }
               break;
            }
         }

         x += w + gap;

         /* One divider between the profile and the other buttons. */
         if (i == BAR_PROFILE)
         {
            float div_sp = REF(6); /* extra space each side of it */
            x += div_sp;
            grid_draw_divider(p_disp, userdata, width, height,
                  x - gap / 2.0f, row1);
            x += div_sp;
         }
      }
   }

   /* Right side, laid out from the right edge leftwards:
    *    [L] Console 12  |  [R] Recent
    * Reference: 17x14 keys, label caps GRID_TEXT_CAP, 7px after the key. */
   {
      float key_h     = floorf(REF(14) + 0.5f);
      float key_w     = floorf(REF(17) + 0.5f);
      float ls        = CAP(GRID_TEXT_CAP);
      float gap       = REF(7);   /* key to label */
      float div_sp    = REF(11);  /* label to divider, each side */
      float cgap      = REF(6);   /* label to count */
      float gy        = floorf(row1 - key_h / 2.0f + 0.5f);
      float ty        = grid_text_y(row1, height, ls);
      /* What L and R do depends on the screen: the games grid's console
       * and sort, the achievements' or Installs' filter and sort, or
       * nothing (NULL: not shown). */
      const char *srt = grid_sort_label[grid_st.sort];
      const char *con = grid_current_tab()->label;
      char count[16];
      float sw, cw, cnw, x;

      /* Settings: the categories centred across the row,
       *    [L]  Graphics  (Audio)  Controls  ...  System  [R]
       * the current one bright on a pill. Dimmed below a category's top
       * page, where L / R page through the list instead. */
      if (grid_settings_shown())
      {
         /* The tabs: the categories, or a game's Settings' four. */
         const char *names[GRID_SETTINGS_CAT_COUNT + 4];
         unsigned cats[GRID_SETTINGS_CAT_COUNT + 4]; /* what a click opens */
         unsigned k, shown = 0, cur = 0;
         bool top      = !grid_st.cc_level
                       && grid_menu_depth() <= grid_settings_base();
         float pill_px = REF(9);   /* pill padding, each side of a name */
         float cat_gap = REF(6);   /* between pills */
         float pill_h  = floorf(REF(20) + 0.5f);
         float total   = 2.0f * (key_w + gap);
         float max_w   = width - 2.0f * side;

         if (grid_st.settings_quick)
            return;

         if (grid_st.game_settings)
         {
            for (k = 0; k < 4; k++)
            {
               cats[shown]    = k;
               names[shown++] = grid_cc_bucket_name[k];
            }
            cur = (unsigned)grid_st.cc_sub;
            top = grid_st.cc_level == 2;
         }
         else
            for (k = 0; k < GRID_SETTINGS_CAT_COUNT; k++)
               if (grid_settings_cat_visible(k))
               {
                  if (k == grid_st.settings_cat)
                     cur = shown;
                  cats[shown]    = k;
                  names[shown++] = grid_settings_cats[k].name;
               }

         for (k = 0; k < shown; k++)
            total += grid_text_width(names[k], height, ls)
               + 2.0f * pill_px + (k ? cat_gap : 0.0f);
         /* Too wide: tighten the pills. */
         if (total > max_w && shown)
         {
            float over = total - max_w;
            float give = (2.0f * shown) * pill_px + (shown - 1) * cat_gap;
            float f    = give > over ? 1.0f - over / give : 0.0f;
            total     -= give * (1.0f - f);
            pill_px   *= f;
            cat_gap   *= f;
         }

         x = floorf((width - total) / 2.0f + 0.5f);
         grid_draw_key(p_disp, userdata, width, height, 'L',
               x, gy, key_w, key_h);
         grid_hit_add(x, floorf(row1 - pill_h / 2.0f + 0.5f), key_w, pill_h,
               HIT_NONE, 0, MENU_ACTION_SCROLL_UP);
         x += key_w + gap;

         for (k = 0; k < shown; k++)
         {
            float nw = grid_text_width(names[k], height, ls);
            float py = floorf(row1 - pill_h / 2.0f + 0.5f);

            if (k)
               x += cat_gap;
            grid_hit_add(x, py, nw + 2.0f * pill_px, pill_h, HIT_CAT,
                  cats[k], MENU_ACTION_NOOP);
            /* Pointed at: a fainter pill. */
            if (k != cur && grid_hover(x, py, nw + 2.0f * pill_px, pill_h))
               grid_draw_shape(p_disp, userdata, width, height,
                     x, py, nw + 2.0f * pill_px, pill_h, pill_h / 2.0f,
                     0xffffff18);
            if (k == cur)
               grid_draw_shape(p_disp, userdata, width, height,
                     x, floorf(row1 - pill_h / 2.0f + 0.5f),
                     nw + 2.0f * pill_px, pill_h, pill_h / 2.0f,
                     COL_DIVIDER);
            grid_draw_text(names[k], x + pill_px, ty, width, height,
                  k == cur && top ? COL_TEXT : COL_TEXT_DIM,
                  TEXT_ALIGN_LEFT, ls);
            x += nw + 2.0f * pill_px;
         }

         grid_draw_key(p_disp, userdata, width, height, 'R',
               floorf(x + gap + 0.5f), gy, key_w, key_h);
         grid_hit_add(floorf(x + gap + 0.5f),
               floorf(row1 - pill_h / 2.0f + 0.5f), key_w, pill_h,
               HIT_NONE, 0, MENU_ACTION_SCROLL_DOWN);
         return;
      }

      snprintf(count, sizeof(count), "%u", (unsigned)grid_st.view_count);
      switch (grid_st.panel)
      {
         case PANEL_CHEEVOS:
            srt = grid_cheevo_sort_label[grid_st.cheevo_sort];
            con = grid_cheevo_filter_label[grid_st.cheevo_filter];
            grid_build_list_view();
            snprintf(count, sizeof(count), "%u",
                  (unsigned)grid_st.list_count);
            break;
         case PANEL_UPDATES:
            srt = grid_dl_sort_label[grid_st.dl_sort];
            con = grid_dl_filter_label[grid_st.dl_filter];
            grid_build_list_view();
            snprintf(count, sizeof(count), "%u",
                  (unsigned)grid_st.list_count);
            break;
         case PANEL_ART:
         case PANEL_ICONS:
         case PANEL_SETUP:
            srt = con = NULL;
            break;
         default:
            break;
      }

      if (!srt)
         return;

      sw  = grid_text_width(srt, height, ls);

      /* [R] sort */
      x = width - side - sw;
      grid_draw_text(srt, x, ty, width, height, COL_TEXT,
            TEXT_ALIGN_LEFT, ls);
      x -= gap + key_w;
      grid_draw_key(p_disp, userdata, width, height, 'R',
            floorf(x + 0.5f), gy, key_w, key_h);
      /* (Clicking the key or its label does what the key does.) */
      grid_hit_add(x, row1 - REF(10), width - side - x, REF(20),
            HIT_NONE, 0, MENU_ACTION_SCROLL_DOWN);

      if (!con)
         return;

      cw  = grid_text_width(con, height, ls);
      cnw = grid_text_width(count, height, ls);

      x -= div_sp;
      grid_draw_divider(p_disp, userdata, width, height, x, row1);
      x -= div_sp;

      /* [L] console (or filter) + how many it shows */
      x -= cnw;
      grid_draw_text(count, x, ty, width, height, COL_TEXT_DIM,
            TEXT_ALIGN_LEFT, ls);
      x -= cgap + cw;
      grid_draw_text(con, x, ty, width, height, COL_TEXT,
            TEXT_ALIGN_LEFT, ls);
      x -= gap + key_w;
      grid_draw_key(p_disp, userdata, width, height, 'L',
            floorf(x + 0.5f), gy, key_w, key_h);
      grid_hit_add(x, row1 - REF(10), key_w + gap + cw + cgap + cnw, REF(20),
            HIT_NONE, 0, MENU_ACTION_SCROLL_UP);
   }
}

/* Play time in words: "2 hours 15 minutes", "1 hour", "15 minutes",
 * "< 1 minute" or "Not played". */
static void grid_format_played(unsigned secs, char *s, size_t len)
{
   unsigned hours = secs / 3600;
   unsigned mins  = (secs / 60) % 60;

   if (!secs)
      strlcpy(s, "Not played", len);
   else if (secs < 60)
      strlcpy(s, "< 1 minute", len);
   else if (!hours)
      snprintf(s, len, "%u minute%s", mins, mins == 1 ? "" : "s");
   else if (!mins)
      snprintf(s, len, "%u hour%s", hours, hours == 1 ? "" : "s");
   else
      snprintf(s, len, "%u hour%s %u minute%s", hours, hours == 1 ? "" : "s",
            mins, mins == 1 ? "" : "s");
}

/* Bottom row: selected game's title and play time on the left, button
 * hints on the right. */
static void grid_draw_bottom_bar(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height)
{
   const char *glyphs[6], *labels[6];
   unsigned i, n  = 0;
   bool running   = grid_content_running();
   bool has_sel   = grid_st.selection < grid_st.view_count;
   float cy       = REF(390);  /* row centre */
   float sp       = REF(19);   /* label end to next circle */
   float side     = REF(20);   /* screen edge margins */
   float hints_w  = 0.0f, hx;

   /* Hints, right-aligned, only those that apply right now, in the order
    * Y, X, A, B: Zoom outside menus, Back only inside them. Face buttons
    * are labelled as on the player's controller ("X" = the menu's scan
    * action, "Y" = search). */
   if (grid_st.panel == PANEL_NONE && !grid_settings_shown())
   {
      glyphs[n] = "Y"; labels[n++] = "Zoom";
   }

   if (grid_settings_shown() && grid_st.cc_level == 1)
   {
      glyphs[n] = "A"; labels[n++] = "Select";
      glyphs[n] = "B"; labels[n++] = "Back";
   }
   else if (grid_settings_shown() && grid_st.cc_level >= 2)
   {
      /* What the selected row takes: a value (X default, A change), a
       * page (A select), reset (A reset) or nothing. */
      static grid_cc_row_t rows[GRID_CC_MAX_ROWS];
      unsigned rn = grid_cc_rows(rows, GRID_CC_MAX_ROWS);
      enum grid_cc_row_kind kind = grid_st.cc_sel < rn
         ? rows[grid_st.cc_sel].kind : CC_ROW_NOTE;

      if (     kind == CC_ROW_OPT || kind == CC_ROW_BUTTON
            || (kind == CC_ROW_EMULATOR && grid_cc_cores.count > 1))
      {
         glyphs[n] = "X"; labels[n++] = "Default";
         glyphs[n] = "A"; labels[n++] = "Change";
      }
      else if (kind == CC_ROW_BINDINGS)
      {
         glyphs[n] = "A"; labels[n++] = "Select";
      }
      else if (kind == CC_ROW_RESET)
      {
         glyphs[n] = "A"; labels[n++] = "Reset";
      }
      glyphs[n] = "B"; labels[n++] = "Back";
   }
   else if (grid_settings_shown())
   {
      glyphs[n] = "Y"; labels[n++] = grid_settings_advanced()
         ? "Hide advanced" : "Show advanced";
      glyphs[n] = "X"; labels[n++] = "Default";
      glyphs[n] = "A"; labels[n++] = "Select";
      glyphs[n] = "B"; labels[n++] = "Back";
   }
   else if (grid_st.panel == PANEL_PROFILES)
   {
      bool on_exit = grid_st.panel_sel == GRID_STRIP_EXIT;
      bool on_add  = !on_exit && grid_st.panel_sel - GRID_STRIP_FIRST
                                 >= grid_profile_count();
      if (!on_exit && !on_add)
      {
         glyphs[n] = "Y"; labels[n++] = "Rename";
         glyphs[n] = "X"; labels[n++] = "Change picture";
      }
      if (on_exit)
      {
         glyphs[n] = "A"; labels[n++] = "Hold to exit";
      }
      else if (!running || !on_add)
      {
         glyphs[n] = "A"; labels[n++] = on_add ? "Add" : "Select";
      }
      glyphs[n] = "B"; labels[n++] = "Back";
   }
   else if (grid_st.panel == PANEL_GAME_MENU)
   {
      glyphs[n] = "A"; labels[n++] = "Open";
      glyphs[n] = "B"; labels[n++] = "Back";
   }
   else if (grid_st.panel == PANEL_CHEEVOS)
   {
      glyphs[n] = "B"; labels[n++] = "Back";
   }
   else if (grid_st.panel == PANEL_SETUP)
   {
      unsigned items[GRID_SETUP_MAX];
      float col[GRID_SETUP_MAX], row[GRID_SETUP_MAX];
      unsigned sn = grid_setup_items(items, col, row);
      unsigned it = grid_st.panel_sel < sn ? items[grid_st.panel_sel]
                                           : SETUP_CONTINUE;
      if (it == SETUP_KEY || it == SETUP_USER || it == SETUP_PASS)
      {
         glyphs[n] = "A"; labels[n++] = "Type";
      }
      else if (it != SETUP_CONTINUE)
      {
         glyphs[n] = "A"; labels[n++] = "Open in browser";
      }
      else if (grid_setup_ready())
      {
         glyphs[n] = "A"; labels[n++] = "Continue";
      }
   }
   else if (grid_st.panel == PANEL_ART)
   {
      if (grid_cover_choices_count())
      {
         glyphs[n] = "A"; labels[n++] = "Choose";
      }
      glyphs[n] = "B"; labels[n++] = "Back";
   }
   else if (grid_st.panel == PANEL_ICONS)
   {
      glyphs[n] = "A"; labels[n++] = "Choose";
      glyphs[n] = "B"; labels[n++] = "Back";
   }
   else if (grid_st.panel == PANEL_UPDATES)
   {
      const grid_update_t *u;
      size_t item = GRID_DL_UPDATE_ALL;

      grid_build_list_view();
      if (grid_st.panel_sel < grid_st.list_count)
         item = grid_st.list_view[grid_st.panel_sel];
      u = item != GRID_DL_UPDATE_ALL ? grid_updates_get(item) : NULL;

      glyphs[n] = "Y"; labels[n++] = "Search";
      if (u && u->removable)
      {
         glyphs[n] = "X"; labels[n++] = "Hold to remove";
      }
      if (grid_st.panel_sel < grid_st.list_count && !u)
      {
         glyphs[n] = "A"; labels[n++] = "Update all";
      }
      else if (u && grid_updates_pick(item) && u->state != GRID_UPDATE_WORKING)
      {
         glyphs[n] = "A"; labels[n++] = u->state == GRID_UPDATE_CURRENT
            ? "Replace" : "Choose file";
      }
      else if (u && u->state == GRID_UPDATE_AVAILABLE)
      {
         glyphs[n] = "A";
         labels[n++] = u->section == GRID_SECTION_INSTALL
            ? "Install" : "Update";
      }
      else if (u && u->state == GRID_UPDATE_FAILED)
      {
         glyphs[n] = "A"; labels[n++] = "Try again";
      }
      glyphs[n] = "B"; labels[n++] = string_is_empty(grid_st.dl_search)
         ? "Back" : "Clear search";
   }
   else if (grid_st.bar_focus)
   {
      glyphs[n] = "A"; labels[n++] = "Select";
   }
   else
   {
      if (has_sel && !grid_st.games[grid_st.view[grid_st.selection]].pending)
      {
         glyphs[n] = "X"; labels[n++] = "Game Options";
      }
      if (has_sel && !grid_st.games[grid_st.view[grid_st.selection]].pending)
      {
         glyphs[n] = "A";
         labels[n++] = grid_is_running(&grid_st.games[grid_st.view[grid_st.selection]])
            ? "Continue" : "Play";
      }
   }

   for (i = 0; i < n; i++)
      hints_w += (i ? sp : 0.0f) + grid_draw_hint(p_disp, userdata,
            width, height, 0, cy, glyphs[i], labels[i], true);

   hx = width - side - hints_w;
   for (i = 0; i < n; i++)
      hx += grid_draw_hint(p_disp, userdata, width, height, hx, cy,
            glyphs[i], labels[i], false) + sp;

   /* Settings: the selected entry's description shows under it. */
   if (grid_settings_shown())
      return;

   /* Selected game's title, then a divider and its play time, then (on
    * RetroAchievements) a trophy and how many achievements are unlocked.
    * The title scrolls if it doesn't fit next to the rest. Shown with the
    * game menu open too (it is for the selected game). */
   if (     has_sel && !grid_st.bar_focus
         && (grid_st.panel == PANEL_NONE || grid_st.panel == PANEL_GAME_MENU))
   {
      static size_t sel_game     = (size_t)-1;
      static retro_time_t sel_at = 0;
      retro_time_t now  = cpu_features_get_time_usec();
      grid_game_t *game = &grid_st.games[grid_st.view[grid_st.selection]];
      float ts          = CAP(GRID_TEXT_CAP);
      float ty          = grid_text_y(cy, height, ts);
      float div_sp      = REF(11);
      float avail       = width - side - hints_w - REF(24) - side;
      float d           = floorf(REF(17) + 0.5f); /* trophy circle */
      float pw, tw, cw  = 0.0f, x;
      unsigned unlocked, total;
      bool cheevos;
      char played[32], count[32];

      /* Progress is looked up once the selection rests on a game, not for
       * every game scrolled past. */
      if (grid_st.view[grid_st.selection] != sel_game)
      {
         sel_game = grid_st.view[grid_st.selection];
         sel_at   = now;
      }
      cheevos = grid_cheevos_progress(grid_meta_game_id(game->path),
            now - sel_at > 400000, &unlocked, &total) && total > 0;

      grid_format_played(game->played, played, sizeof(played));
      pw = grid_text_width(played, height, ts);
      if (cheevos)
      {
         snprintf(count, sizeof(count), "%u/%u", unlocked, total);
         cw = 2.0f * div_sp + d + REF(6) + grid_text_width(count, height, ts);
      }
      tw = grid_text_width(game->title, height, ts);
      if (tw > avail - pw - cw - 2.0f * div_sp)
         tw = avail - pw - cw - 2.0f * div_sp;

      if (tw > 0.0f)
         grid_draw_marquee(p_disp, userdata, false, game->title,
               side, ty, tw, width, height, COL_TEXT, ts,
               TEXT_ALIGN_LEFT, game);

      x = side + tw + div_sp;
      grid_draw_divider(p_disp, userdata, width, height, x, cy);
      x += div_sp;
      grid_draw_text(played, x, ty,
            width, height, COL_TEXT, TEXT_ALIGN_LEFT, ts);

      if (cheevos)
      {
         x += pw + div_sp;
         grid_draw_divider(p_disp, userdata, width, height, x, cy);
         x += div_sp;
         /* Lime circle with the trophy cut out of it; count in lime. */
         grid_icon_button_draw(p_disp, userdata, width, height,
               &grid_st.icon_trophy, floorf(x + 0.5f),
               floorf(cy - d / 2.0f + 0.5f), (unsigned)d, 0.58f,
               COL_CHEEVO, 0);
         grid_draw_text(count, x + d + REF(6), ty,
               width, height, COL_CHEEVO, TEXT_ALIGN_LEFT, ts);
      }
   }
}

static void grid_draw_screen_heading(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, const char **parts, unsigned count);

/* A heading part being typed in: what follows its "Label: " is the line,
 * drawn with its caret. NULL: none. */
static const char *grid_heading_edit = NULL;

/* Choosing a profile picture: its own screen like the art screen. "None"
 * (the profile's initial), then every downloaded game icon, as many per row
 * as fit. */
static void grid_draw_picker_screen(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height)
{
   static size_t first_row = 0;
   size_t i, n       = grid_st.pick_count + 1;
   const char *name  = grid_profile_name(grid_st.pick_target);
   const char *cur   = grid_profile_icon(grid_st.pick_target);
   float left        = REF(35);
   float top         = REF(70);
   float area_h      = REF(364) - top;
   float gap         = REF(14);
   float d           = floorf(REF(56));
   float ts          = CAP(GRID_TEXT_CAP);
   unsigned cols;
   size_t rows, sel_row;

   {
      const char *parts[2];
      parts[0] = "Change picture";
      parts[1] = name;
      grid_draw_screen_heading(p_disp, userdata, width, height, parts, 2);
   }

   cols = (unsigned)((width - 2.0f * left + gap) / (d + gap));
   if (cols < 1)
      cols = 1;
   grid_st.pick_cols = cols;

   /* Keep the selected row visible. */
   rows = (size_t)((area_h + gap) / (d + gap));
   if (rows < 1)
      rows = 1;
   sel_row = grid_st.panel_sel / cols;
   if (sel_row < first_row)
      first_row = sel_row;
   else if (sel_row >= first_row + rows)
      first_row = sel_row - rows + 1;
   grid_draw_scrollbar(p_disp, userdata, width, height, top, area_h,
         first_row, rows, (n + cols - 1) / cols);

   for (i = first_row * cols; i < n && i < (first_row + rows) * cols; i++)
   {
      float ix     = floorf(left + (i % cols) * (d + gap) + 0.5f);
      float iy     = floorf(top + (i / cols - first_row) * (d + gap) + 0.5f);
      bool current = false;

      grid_hit_add(ix, iy, d, d, HIT_PANEL, (unsigned)i, MENU_ACTION_OK);
      if (i == grid_st.panel_sel)
         grid_draw_cursor(p_disp, userdata, width, height,
               ix, iy, d, d, d / 2.0f);

      if (i == 0)
      {
         /* No picture: the initial on the profile's colour. */
         grid_draw_rounded(p_disp, userdata, width, height,
               ix, iy, d, d, d / 2.0f, grid_profile_color(grid_st.pick_target));
         grid_draw_char_centered(true,
               (char)toupper((unsigned char)(name[0] ? name[0] : '?')),
               ix + d / 2.0f, iy + d / 2.0f, width, height, 0xffffffff,
               d * 0.42f / (height / 32.0f) / GRID_FONT_CAP);
         current = string_is_empty(cur);
      }
      else
      {
         char path[PATH_MAX_LENGTH];
         bool have = grid_cover_icon_get(i - 1, path, sizeof(path));

         /* Icons load as they scroll into view. */
         if (have && grid_st.pick_tex && !grid_st.pick_tried[i - 1])
         {
            unsigned tw, th;
            grid_st.pick_tried[i - 1] = true;
            grid_load_rounded_texture(path, &grid_st.pick_tex[i - 1],
                  &tw, &th, 0.5f, 1.0f);
         }

         if (grid_st.pick_tex && grid_st.pick_tex[i - 1])
            grid_draw_icon(p_disp, userdata, width, height,
                  &grid_st.pick_tex[i - 1], ix + d / 2.0f, iy + d / 2.0f, d,
                  0xffffffff);
         else
            grid_draw_rounded(p_disp, userdata, width, height,
                  ix, iy, d, d, d / 2.0f, COL_TILE);
         current = have && !string_is_empty(cur) && string_is_equal(path, cur);
      }

      /* The one in use now: a lime dot, like the running-game badge. */
      if (current)
      {
         float b  = floorf(d * 0.04f + 0.5f);
         float dd = floorf(d * 0.24f + 0.5f);
         float bx = ix + d - dd;
         float by = iy + d - dd;
         grid_draw_rounded(p_disp, userdata, width, height,
               bx - b, by - b, dd + 2.0f * b, dd + 2.0f * b, dd / 2.0f + b,
               0xffffffff);
         grid_draw_rounded(p_disp, userdata, width, height,
               bx, by, dd, dd, dd / 2.0f, COL_LIME);
      }
   }

   if (n == 1)
      grid_draw_text("Game icons appear here as they download from SteamGridDB",
            width / 2.0f, grid_text_y(top + d + REF(40), height, ts),
            width, height, COL_TEXT_DIM, TEXT_ALIGN_CENTER, ts);
}

/* A texture filling the rect (x, y, w, h) on whole pixels. */
static void grid_draw_icon_rect(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, uintptr_t *tex,
      float x, float y, float w, float h)
{
   float color[16];
   grid_set_color(color, 0xffffffff);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)floorf(x + 0.5f), (int)floorf(y + 0.5f),
         (unsigned)(w + 0.5f), (unsigned)(h + 0.5f),
         width, height, color, tex);
}

/* Menus: rows of labels in a dark rounded box (the game menu's slides out
 * from under its tile), the selected row highlighted. */
#define GRID_MENU_ITEM_H     REF(30)
#define GRID_MENU_PAD        REF(7)
#define GRID_MENU_RADIUS     REF(14)
#define GRID_MENU_TEXT_X     REF(14) /* label inset from the highlight */
#define GRID_MENU_OPEN_USEC  260000
#define GRID_MENU_CLOSE_USEC 160000

static float grid_clamp01(float v)
{
   return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* rgba with its alpha scaled by a (0..1). */
static uint32_t grid_fade(uint32_t rgba, float a)
{
   a = grid_clamp01(a);
   return (rgba & 0xffffff00) | (uint32_t)((rgba & 0xff) * a + 0.5f);
}

/* Translucent rounded rect drawn into (x, y, w, h) from the texture made
 * for an fw x fh one: it can be stretched while animating without a new
 * texture each frame (the corners squash a little on the way). At its
 * own size it is grid_draw_shape. */
static void grid_draw_shape_stretched(gfx_display_t *p_disp,
      void *userdata, unsigned width, unsigned height,
      float x, float y, float w, float h, float fw, float fh, float r,
      uint32_t rgba)
{
   float color[16];
   int ifw = (int)floorf(fw + 0.5f);
   int ifh = (int)floorf(fh + 0.5f);
   float sx, sy;
   unsigned tw, th;
   uintptr_t *tex;

   if (ifw < 1 || ifh < 1 || w < 1.0f || h < 1.0f)
      return;
   if (!(tex = grid_ring_texture(ifw, ifh, (int)floorf(r + 0.5f), -100000,
               100000, &tw, &th)) || !*tex)
      return;

   sx = w / ifw;
   sy = h / ifh;
   grid_set_color(color, rgba);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)floorf(x - sx + 0.5f), (int)floorf(y - sy + 0.5f),
         (unsigned)(tw * sx + 0.5f), (unsigned)(th * sy + 0.5f),
         width, height, color, tex);
}

/* Text size of a menu whose rows are item_h tall: smaller rows (on small
 * tiles) get smaller text, in a few steps (each size is a font). */
static float grid_menu_scale(float item_h, unsigned height)
{
   float s = floorf(item_h / GRID_MENU_ITEM_H * 10.0f + 0.5f) / 10.0f;
   return s < 0.5f ? 0.5f : s > 1.0f ? 1.0f : s;
}

/* Widest label plus insets, at text scale s (1: full size). */
static float grid_menu_width(const char **labels, unsigned count, float s,
      unsigned height)
{
   unsigned k;
   float w = 0.0f;

   for (k = 0; k < count; k++)
   {
      float tw = grid_text_width_font(false, labels[k], height,
            CAP(GRID_TEXT_CAP * s));
      if (tw > w)
         w = tw;
   }
   return w + 2.0f * (GRID_MENU_PAD + GRID_MENU_TEXT_X * s);
}

/* Where row k's top-left is from the menu's: per_col rows of item_h per
 * column of col_w, left to right. k is fractional while the highlight
 * glides between rows. */
static void grid_menu_row_pos(float k, unsigned per_col, float col_w,
      float item_h, unsigned height, float *ox, float *oy)
{
   unsigned i;
   int k0  = (int)floorf(k);
   float f = k - k0;
   float x[2], y[2];

   for (i = 0; i < 2; i++)
   {
      unsigned kk = (unsigned)k0 + i;
      x[i] = (kk / per_col) * col_w;
      y[i] = GRID_MENU_PAD + (kk % per_col) * item_h;
   }
   *ox = x[0] + (x[1] - x[0]) * f;
   *oy = y[0] + (y[1] - y[0]) * f;
}

/* A menu's rows in the box at (x, y) of corner radius r: per_col rows of
 * item_h per column of col_w, text sized to the rows. Row sel is under a highlight that
 * glides from row to row (jumping when it wraps round); the danger row's
 * label is red, turning white on a red highlight when selected.
 * ca (0..1): how shown the rows are, each a
 * little after the one before, sliding up into place. */
static void grid_draw_menu_rows(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, const char **labels, unsigned count,
      int danger, unsigned sel, float x, float y, unsigned per_col,
      float col_w, float item_h, float r, float ca)
{
   static float hl             = 0.0f; /* highlight's row, gliding to sel */
   static retro_time_t hl_last = 0;
   retro_time_t now = cpu_features_get_time_usec();
   unsigned k;
   float s      = grid_menu_scale(item_h, height);
   float ts     = CAP(GRID_TEXT_CAP * s);
   float pad    = GRID_MENU_PAD;
   float hr     = r - pad;
   float d;     /* how much the highlight is on the danger row (0..1) */
   float rx, ry;

   if (ca <= 0.0f || count == 0)
      return;
   if (per_col < 1)
      per_col = 1;
   if (hr < REF(5))
      hr = REF(5);
   if (sel >= count)
      sel = count - 1;

   /* The highlight snaps into place while the menu appears, and when it
    * wraps round (rather than sweeping across every row). */
   if (ca < 1.0f || now - hl_last > 100000 || fabsf((float)sel - hl) > 1.5f)
      hl = (float)sel;
   else
   {
      hl += ((float)sel - hl) * 0.35f;
      if (fabsf((float)sel - hl) < 0.01f)
         hl = (float)sel;
   }
   hl_last = now;
   d = danger >= 0 ? grid_clamp01(1.0f - fabsf(hl - danger)) : 0.0f;
   {
      uint32_t c = (grid_mix(0xffffffff, COL_DANGER_BG, d) & 0xffffff00)
         | (uint32_t)(0x33 + (0xff - 0x33) * d);

      grid_menu_row_pos(hl, per_col, col_w, item_h, height, &rx, &ry);
      grid_draw_shape(p_disp, userdata, width, height,
            x + rx + pad, y + ry + (1.0f - ca) * REF(6),
            col_w - 2.0f * pad, item_h, hr, grid_fade(c, ca));
   }

   for (k = 0; k < count; k++)
   {
      float ra    = grid_clamp01(ca * 1.6f - k * 0.12f);
      bool is_sel = k == sel;
      uint32_t col;

      if (ra <= 0.0f)
         continue;

      grid_menu_row_pos((float)k, per_col, col_w, item_h, height,
            &rx, &ry);

      if ((int)k == danger)
         col = grid_mix(COL_DANGER, 0xffffffff, d);
      else
         col = is_sel ? 0xffffffff : COL_TEXT_SOFT;

      grid_draw_text_font(false, labels[k],
            x + rx + pad + GRID_MENU_TEXT_X * s,
            grid_text_y(y + ry + (1.0f - ra) * REF(6) + item_h / 2.0f,
               height, ts), width, height,
            grid_fade(col, ra), TEXT_ALIGN_LEFT, ts);
   }
}

/* Game menu's animation this frame: whether its drawer shows, how far out
 * it is (e: 0..1, overshooting a little) and how shown its rows are (ca).
 * It slides out when it opens and back in when it closes to the grid
 * (gone at once for the screens it opens). */
static bool grid_game_menu_anim(bool in_settings, float *e, float *ca)
{
   retro_time_t now = cpu_features_get_time_usec();
   bool open        = grid_st.panel == PANEL_GAME_MENU && !in_settings;
   float p;

   if (open && !grid_st.gm_open)
   {
      grid_st.gm_open    = true;
      grid_st.gm_closing = false;
      grid_st.gm_start   = now;
   }
   else if (!open && grid_st.gm_open)
   {
      grid_st.gm_open    = false;
      grid_st.gm_closing = grid_st.panel == PANEL_NONE && !in_settings;
      grid_st.gm_start   = now;
   }
   if (grid_st.gm_closing && (grid_st.panel != PANEL_NONE || in_settings))
      grid_st.gm_closing = false;

   if (open)
   {
      /* Ease-out with a slight overshoot ("back"). */
      float s = 1.2f, q;
      p   = grid_clamp01((float)(now - grid_st.gm_start)
            / GRID_MENU_OPEN_USEC);
      q   = p - 1.0f;
      *e  = 1.0f + (s + 1.0f) * q * q * q + s * q * q;
      *ca = (p - 0.35f) / 0.65f;
      grid_st.gm_sel = grid_st.panel_sel;
      return true;
   }
   if (grid_st.gm_closing)
   {
      p = (float)(now - grid_st.gm_start) / GRID_MENU_CLOSE_USEC;
      if (p >= 1.0f || p < 0.0f)
      {
         grid_st.gm_closing = false;
         return false;
      }
      *e  = 1.0f - p * p; /* ease-in: speeds up back under the tile */
      *ca = 1.0f - p * 2.5f;
      return true;
   }
   return false;
}

/* The game menu laid out in a drawer th tall: its rows (labels, how many,
 * which is Quit game: -1 none), one column of rows item_h tall, col_w
 * wide, shrinking (with their text) to fit small tiles. Returns the
 * drawer's width. */
static float grid_game_drawer_layout(unsigned height, float th,
      const char **labels, unsigned *n, int *danger, float *item_h,
      float *col_w)
{
   unsigned items[GAME_MENU_COUNT];
   unsigned k;
   float s;

   *n      = grid_game_menu_items(items);
   *danger = -1;
   for (k = 0; k < *n; k++)
   {
      labels[k] = grid_game_menu_label[items[k]];
      if (items[k] == GAME_MENU_QUIT)
         *danger = (int)k;
   }

   *item_h = (th - 2.0f * GRID_MENU_PAD) / (*n ? *n : 1);
   if (*item_h > GRID_MENU_ITEM_H)
      *item_h = GRID_MENU_ITEM_H;

   s      = grid_menu_scale(*item_h, height);
   *col_w = grid_menu_width(labels, *n, s, height);
   if (*col_w < REF(170) * s)
      *col_w = REF(170) * s;
   return *col_w;
}

/* Game menu: a drawer as tall as its game's tile (tx, ty, tw, th, corner
 * radius tr), sliding out to the right from under it, the rest of the
 * tile's row pushed aside to make room, listing what can be done with the
 * game. Drawn before the tile, which covers its inner end; the selection
 * ring (ring) goes round both. e, ca: see grid_game_menu_anim. */
static void grid_draw_game_drawer(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, float tx, float ty, float tw,
      float th, float tr, bool ring, float e, float ca)
{
   const char *labels[GAME_MENU_COUNT];
   unsigned n;
   int danger;
   float item_h, col_w;
   float w      = grid_game_drawer_layout(height, th, labels, &n, &danger,
         &item_h, &col_w);
   float out    = e > 0.0f ? w * e : 0.0f; /* how far it shows past the tile */
   float dw     = w + tw / 2.0f;          /* reaching under the tile's middle */
   float clip_x = tx + tw / 2.0f;

   if (out >= 1.0f && clip_x < width)
   {
      /* Clipped to the right of the tile's middle, so while it is still
       * mostly under the tile none of it pokes out of the far side.
       * Earlier text is flushed first so the clip only affects this. */
      grid_flush_text(width, height);
      if (p_disp->dispctx && p_disp->dispctx->scissor_begin)
         gfx_display_scissor_begin(p_disp, userdata, width, height,
               (int)clip_x, 0, (unsigned)(width - clip_x), height);

      /* Translucent light, like the buttons. */
      grid_draw_shape_stretched(p_disp, userdata, width, height,
            tx + tw + out - dw, ty, dw, th, dw, th, tr, COL_DIVIDER);
      grid_draw_menu_rows(p_disp, userdata, width, height, labels, n,
            danger, grid_st.gm_sel, tx + tw + out - w, ty, n,
            col_w, item_h, tr, ca);

      /* Its rows, once out. */
      if (ca >= 1.0f && grid_st.panel == PANEL_GAME_MENU)
      {
         unsigned k;
         for (k = 0; k < n; k++)
         {
            float rx, ry;
            grid_menu_row_pos((float)k, n, col_w, item_h, height, &rx, &ry);
            grid_hit_add(tx + tw + out - w + rx + GRID_MENU_PAD, ty + ry,
                  col_w - 2.0f * GRID_MENU_PAD, item_h, HIT_PANEL, k,
                  MENU_ACTION_OK);
         }
      }

      grid_flush_text(width, height);
      if (p_disp->dispctx && p_disp->dispctx->scissor_end)
         p_disp->dispctx->scissor_end(userdata, width, height);
   }

   if (ring)
      grid_draw_cursor(p_disp, userdata, width, height,
            tx, ty, tw + out, th, tr);
}

/* Heading of a screen: its parts separated by dividers, the first in
 * bold. In Settings one line below the tabs, e.g. "Graphics | Video".
 * Elsewhere the first part alone on the top row (level with the L/R
 * labels) and the details on a dimmer row under it:
 *    Achievements
 *    Super Metroid | 12 / 40 unlocked | 120 / 400 points */
static void grid_draw_screen_heading(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, const char **parts, unsigned count)
{
   unsigned k;
   bool one_line = grid_settings_shown();
   float ts      = CAP(GRID_TEXT_CAP);
   float x0      = REF(35);
   float x       = x0;
   float cy      = one_line ? REF(70) : REF(31);
   float y       = grid_text_y(cy, height, ts);
   float div_sp  = REF(11);
   bool first    = true; /* nothing drawn on this row yet */

   for (k = 0; k < count; k++)
   {
      bool bold = k == 0;
      if (string_is_empty(parts[k]))
         continue;
      if (k == 1 && !one_line)
      {
         /* Details row. */
         ts    = CAP(GRID_TEXT_CAP - 1.5f);
         x     = x0;
         cy    = REF(53);
         y     = grid_text_y(cy, height, ts);
         first = true;
      }
      if (!first)
      {
         x += div_sp;
         grid_draw_divider(p_disp, userdata, width, height, x, cy);
         x += div_sp;
      }
      if (parts[k] == grid_heading_edit && strstr(parts[k], ": "))
      {
         /* "Label: " then the line being typed, to the screen's edge. */
         char label[64];
         size_t n = strstr(parts[k], ": ") - parts[k] + 2;
         strlcpy(label, parts[k], n + 1 < sizeof(label) ? n + 1
               : sizeof(label));
         grid_draw_text_font(bold, label, x, y, width, height,
               bold || one_line ? COL_TEXT : COL_TEXT_DIM, TEXT_ALIGN_LEFT,
               ts);
         x += grid_text_width_font(bold, label, height, ts);
         grid_draw_edit(p_disp, userdata, width, height, parts[k] + n, x, cy,
               width - x0 - x, ts, COL_TEXT);
         x += grid_text_width_font(bold, parts[k] + n, height, ts);
         first = false;
         continue;
      }
      grid_draw_text_font(bold, parts[k], x, y, width, height,
            bold || one_line ? COL_TEXT : COL_TEXT_DIM, TEXT_ALIGN_LEFT, ts);
      x    += grid_text_width_font(bold, parts[k], height, ts);
      first = false;
   }
}

/* Achievements: the game's set, unlocked ones in full colour, with how
 * many are unlocked and points earned in the heading. */
static void grid_draw_cheevos_screen(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height)
{
   static size_t first_row = 0;
   size_t i, n       = grid_cheevos_count(); /* all of them */
   size_t shown;                             /* after the filter */
   const char *title = grid_st.menu_game < grid_st.count
      ? grid_st.games[grid_st.menu_game].title : "";
   float left        = REF(35);
   float top         = REF(70);
   float area_h      = REF(364) - top;
   float cgap        = REF(10);
   float card_w      = (width - 2.0f * left - (GRID_CHEEVO_COLS - 1) * cgap)
                       / GRID_CHEEVO_COLS;
   float card_h      = REF(50);   /* five rows fit */
   float bd          = floorf(REF(38));   /* badge */
   float cpad        = REF(8);
   float ts          = CAP(GRID_TEXT_CAP);
   float ss          = CAP(GRID_TEXT_CAP - 2);
   const char *msg   = NULL;
   size_t rows, sel_row;
   char unlocked[48], points[48];
   const char *parts[4];
   unsigned np = 0;

   switch (grid_cheevos_state())
   {
      case GRID_CHEEVOS_LOGGED_OUT:
         msg = "Log in to RetroAchievements in Settings to see achievements";
         break;
      case GRID_CHEEVOS_NO_GAME:
         msg = "This game has no achievements on RetroAchievements";
         break;
      case GRID_CHEEVOS_LOADING:
         msg = "Loading...";
         break;
      case GRID_CHEEVOS_ERROR:
         msg = "Couldn't reach RetroAchievements";
         break;
      default:
         if (!n)
            msg = "This game has no achievements yet";
         break;
   }

   /* Achievements | Game | 12 / 40 unlocked | 120 / 400 points */
   parts[np++] = "Achievements";
   parts[np++] = title;
   if (!msg)
   {
      unsigned got = 0, pts = 0, all_pts = 0;
      for (i = 0; i < n; i++)
      {
         const grid_cheevo_t *c = grid_cheevos_get(i);
         all_pts += c->points;
         if (c->unlocked)
         {
            got++;
            pts += c->points;
         }
      }
      snprintf(unlocked, sizeof(unlocked), "%u / %u unlocked",
            got, (unsigned)n);
      snprintf(points, sizeof(points), "%u / %u points", pts, all_pts);
      parts[np++] = unlocked;
      parts[np++] = points;
   }
   grid_draw_screen_heading(p_disp, userdata, width, height, parts, np);

   grid_build_list_view();
   shown = grid_st.list_count;
   if (!msg && !shown)
      msg = grid_st.cheevo_filter == CHEEVO_FILTER_UNLOCKED
         ? "None unlocked yet" : "All unlocked!";

   if (msg)
   {
      grid_draw_text(msg, width / 2.0f,
            grid_text_y(top + area_h / 2.0f, height, ts), width, height,
            COL_TEXT_SOFT, TEXT_ALIGN_CENTER, ts);
      return;
   }

   /* Keep the selected row visible. */
   rows = (size_t)((area_h + cgap) / (card_h + cgap));
   if (rows < 1)
      rows = 1;
   sel_row = grid_st.panel_sel / GRID_CHEEVO_COLS;
   if (sel_row < first_row)
      first_row = sel_row;
   else if (sel_row >= first_row + rows)
      first_row = sel_row - rows + 1;
   grid_draw_scrollbar(p_disp, userdata, width, height, top, area_h,
         first_row, rows, (shown + GRID_CHEEVO_COLS - 1) / GRID_CHEEVO_COLS);

   for (i = first_row * GRID_CHEEVO_COLS;
         i < shown && i < (first_row + rows) * GRID_CHEEVO_COLS; i++)
   {
      size_t src             = grid_st.list_view[i]; /* set index */
      const grid_cheevo_t *c = grid_cheevos_get(src);
      size_t col  = i % GRID_CHEEVO_COLS;
      size_t row  = i / GRID_CHEEVO_COLS - first_row;
      float cx    = floorf(left + col * (card_w + cgap) + 0.5f);
      float cy    = floorf(top + row * (card_h + cgap) + 0.5f);
      float mid   = cy + card_h / 2.0f;
      float tx    = cx + cpad + bd + REF(10);
      char pts[16], line[320];
      float pw, tw;

      if (!c)
         break;

      grid_hit_add(cx, cy, card_w, card_h, HIT_PANEL, (unsigned)i,
            MENU_ACTION_NOOP);
      if (i == grid_st.panel_sel)
         grid_draw_cursor(p_disp, userdata, width, height,
               cx, cy, card_w, card_h, REF(10));
      grid_draw_shape(p_disp, userdata, width, height,
            cx, cy, card_w, card_h, REF(10), COL_DIVIDER);

      /* Badge (the locked one is greyed by RetroAchievements). */
      if (     src < GRID_MAX_CHEEVOS && !grid_st.badge_tried[src]
            && !string_is_empty(c->badge))
      {
         unsigned bw, bh;
         grid_st.badge_tried[src] = true;
         grid_load_rounded_texture(c->badge, &grid_st.badge_tex[src],
               &bw, &bh, 0.12f, 1.0f);
      }
      if (src < GRID_MAX_CHEEVOS && grid_st.badge_tex[src])
         grid_draw_icon(p_disp, userdata, width, height,
               &grid_st.badge_tex[src], cx + cpad + bd / 2.0f, mid, bd,
               0xffffffff);
      else
         grid_draw_shape(p_disp, userdata, width, height,
               cx + cpad, mid - bd / 2.0f, bd, bd, bd * 0.12f, COL_DIVIDER);

      /* Points on the right. */
      snprintf(pts, sizeof(pts), "%u", c->points);
      pw = grid_text_width(pts, height, ss);
      grid_draw_text(pts, cx + card_w - cpad,
            grid_text_y(mid - REF(7), height, ss), width, height,
            COL_TEXT_SOFT, TEXT_ALIGN_RIGHT, ss);

      tw = cx + card_w - cpad - tx;
      if (i == grid_st.panel_sel)
      {
         grid_draw_marquee(p_disp, userdata, true, c->title,
               tx, grid_text_y(mid - REF(7), height, ts), tw - pw - REF(8),
               width, height, c->unlocked ? COL_TEXT : COL_TEXT_SOFT, ts,
               TEXT_ALIGN_LEFT, c->title);
         grid_draw_marquee(p_disp, userdata, false, c->description,
               tx, grid_text_y(mid + REF(8), height, ss), tw,
               width, height, COL_TEXT_SOFT, ss, TEXT_ALIGN_LEFT,
               c->description);
      }
      else
      {
         grid_fit_text(true, c->title, tw - pw - REF(8), height, ts,
               line, sizeof(line));
         grid_draw_text_bold(line, tx, grid_text_y(mid - REF(7), height, ts),
               width, height, c->unlocked ? COL_TEXT : COL_TEXT_SOFT,
               TEXT_ALIGN_LEFT, ts);
         grid_fit_text(false, c->description, tw, height, ss,
               line, sizeof(line));
         grid_draw_text(line, tx, grid_text_y(mid + REF(8), height, ss),
               width, height, COL_TEXT_SOFT, TEXT_ALIGN_LEFT, ss);
      }
   }
}

/* Installs: what L's filter shows (All, Updates, Installable, Up to
 * date) as cards in two columns, R's order, narrowed by the Y search.
 * A card: the name
 * (bold) with its console (or Emulator / Core) on the right, then how it
 * stands (blue
 * when there is something to get, red when it failed) with its version
 * (Eden's tag, "Steam build" or a build date) on the right; a bar along
 * the bottom while it downloads. */
static void grid_draw_downloads_screen(gfx_display_t *p_disp,
      void *userdata, unsigned width, unsigned height)
{
   static size_t first_row = 0;
   size_t i, shown;
   const char *search = grid_dl_search_text();
   bool typing        = menu_input_dialog_get_display_kb()
                     && grid_st.kb == KB_DOWNLOADS;
   float left         = REF(35);
   float top          = REF(70);
   float area_h       = REF(364) - top;
   float cgap         = REF(10);
   float card_w       = (width - 2.0f * left - (GRID_DL_COLS - 1) * cgap)
                        / GRID_DL_COLS;
   float card_h       = REF(46);
   float cpad         = REF(12);
   float ts           = CAP(GRID_TEXT_CAP);
   float ss           = CAP(GRID_TEXT_CAP - 2);
   const char *msg    = NULL;
   char msg_buf[192], waiting[48], search_part[160];
   const char *parts[4];
   unsigned np = 0, avail = grid_updates_available();
   size_t rows, sel_row;

   grid_build_list_view();
   shown = grid_st.list_count;

   /* Installs | 3 updates | Checking... | "search" */
   parts[np++] = "Installs";
   if (avail)
   {
      snprintf(waiting, sizeof(waiting), "%u update%s waiting", avail,
            avail == 1 ? "" : "s");
      parts[np++] = waiting;
   }
   if (grid_updates_checking())
      parts[np++] = "Checking...";
   else if (grid_updates_cores_failed())
      parts[np++] = "Couldn't reach the core server";
   if (!string_is_empty(search) || typing)
   {
      snprintf(search_part, sizeof(search_part), "Search: %s", search);
      parts[np++] = search_part;
   }
   grid_heading_edit = typing ? search_part : NULL;
   grid_draw_screen_heading(p_disp, userdata, width, height, parts, np);
   grid_heading_edit = NULL;

   if (!shown)
   {
      if (!string_is_empty(search))
      {
         snprintf(msg_buf, sizeof(msg_buf), "Nothing here matches \"%s\"",
               search);
         msg = msg_buf;
      }
      else if (grid_updates_checking())
         msg = "Checking...";
      else if (grid_st.dl_filter == DL_FILTER_UPDATES)
         msg = "Everything is up to date";
      else if (grid_st.dl_filter == DL_FILTER_INSTALLABLE)
         msg = "Nothing new to install";
      else
         msg = "Nothing to show";
   }

   if (msg)
   {
      grid_draw_text(msg, width / 2.0f,
            grid_text_y(top + area_h / 2.0f, height, ts), width, height,
            COL_TEXT_SOFT, TEXT_ALIGN_CENTER, ts);
      return;
   }

   /* Keep the selected row visible. */
   rows = (size_t)((area_h + cgap) / (card_h + cgap));
   if (rows < 1)
      rows = 1;
   sel_row = grid_st.panel_sel / GRID_DL_COLS;
   if (sel_row < first_row)
      first_row = sel_row;
   else if (sel_row >= first_row + rows)
      first_row = sel_row - rows + 1;
   grid_draw_scrollbar(p_disp, userdata, width, height, top, area_h,
         first_row, rows, (shown + GRID_DL_COLS - 1) / GRID_DL_COLS);

   for (i = first_row * GRID_DL_COLS;
         i < shown && i < (first_row + rows) * GRID_DL_COLS; i++)
   {
      size_t item = grid_st.list_view[i];
      const grid_update_t *u = item != GRID_DL_UPDATE_ALL
         ? grid_updates_get(item) : NULL;
      size_t col  = i % GRID_DL_COLS;
      size_t row  = i / GRID_DL_COLS - first_row;
      float cx    = floorf(left + col * (card_w + cgap) + 0.5f);
      float cy    = floorf(top + row * (card_h + cgap) + 0.5f);
      float mid   = cy + card_h / 2.0f;
      float tx    = cx + cpad;
      float tw    = card_w - 2.0f * cpad;
      uint32_t dcol = COL_TEXT_SOFT;
      const char *detail;
      char line[320], console[128];
      float kw, dw = 0.0f;

      grid_hit_add(cx, cy, card_w, card_h, HIT_PANEL, (unsigned)i,
            MENU_ACTION_OK);
      if (i == grid_st.panel_sel)
      {
         /* X held to remove: the cursor turns into a red ring that fills
          * round the card, and back when let go (as Exit). */
         bool holding = grid_st.dl_hold_start
                     && grid_st.dl_hold_item == item;
         float target = holding ? 1.0f : 0.0f;

         if (grid_st.dl_hold_item != item)
            grid_st.dl_hold_blend = 0.0f; /* moved off it */
         grid_st.dl_hold_blend += (target - grid_st.dl_hold_blend) * 0.08f;
         if (fabsf(target - grid_st.dl_hold_blend) < 0.01f)
            grid_st.dl_hold_blend = target;
         if (holding)
         {
            grid_st.dl_hold_t = (cpu_features_get_time_usec()
                  - grid_st.dl_hold_start) / (float)GRID_DL_HOLD;
            /* The full ring is on screen: removing may follow. */
            if (grid_st.dl_hold_t >= 1.0f && !grid_st.dl_hold_full_at)
               grid_st.dl_hold_full_at = cpu_features_get_time_usec();
         }

         if (grid_st.dl_hold_blend > 0.0f)
            grid_draw_hold_rect(p_disp, userdata, width, height,
                  cx, cy, card_w, card_h, REF(10), grid_st.dl_hold_t,
                  grid_st.dl_hold_blend);
         else
            grid_draw_cursor(p_disp, userdata, width, height,
                  cx, cy, card_w, card_h, REF(10));
      }
      grid_draw_shape(p_disp, userdata, width, height,
            cx, cy, card_w, card_h, REF(10), COL_DIVIDER);

      /* "Update all": the waiting updates in one go. */
      if (item == GRID_DL_UPDATE_ALL)
      {
         snprintf(line, sizeof(line), "%u updates waiting", avail);
         grid_draw_text_bold("Update all", tx,
               grid_text_y(mid - REF(7), height, ts),
               width, height, COL_TEXT, TEXT_ALIGN_LEFT, ts);
         grid_draw_text(line, tx, grid_text_y(mid + REF(8), height, ss),
               width, height, COL_SELECT, TEXT_ALIGN_LEFT, ss);
         continue;
      }
      if (!u)
         break;

      detail = u->detail;
      switch (u->state)
      {
         case GRID_UPDATE_AVAILABLE: dcol = COL_SELECT; break;
         case GRID_UPDATE_FAILED:    dcol = COL_DANGER; break;
         case GRID_UPDATE_WORKING:   dcol = COL_CURSOR; break;
         case GRID_UPDATE_DONE:      dcol = COL_TEXT;   break;
         default:                    break;
      }

      /* Console (else Emulator / Core) and version on the right. */
      grid_fit_text(false, string_is_empty(u->console) ? u->kind
            : u->console, card_w * 0.45f, height, ss, console,
            sizeof(console));
      kw = grid_text_width(console, height, ss);
      grid_draw_text(console, cx + card_w - cpad,
            grid_text_y(mid - REF(7), height, ss), width, height,
            COL_TEXT_DIM, TEXT_ALIGN_RIGHT, ss);
      if (!string_is_empty(u->version))
      {
         dw = grid_text_width(u->version, height, ss);
         grid_draw_text(u->version, cx + card_w - cpad,
               grid_text_y(mid + REF(8), height, ss), width, height,
               COL_TEXT_DIM, TEXT_ALIGN_RIGHT, ss);
      }

      if (i == grid_st.panel_sel)
         grid_draw_marquee(p_disp, userdata, true, u->name,
               tx, grid_text_y(mid - REF(7), height, ts), tw - kw - REF(8),
               width, height, COL_TEXT, ts, TEXT_ALIGN_LEFT, u->name);
      else
      {
         grid_fit_text(true, u->name, tw - kw - REF(8), height, ts,
               line, sizeof(line));
         grid_draw_text_bold(line, tx, grid_text_y(mid - REF(7), height, ts),
               width, height, COL_TEXT, TEXT_ALIGN_LEFT, ts);
      }
      grid_fit_text(false, detail, tw - dw - REF(8), height, ss,
            line, sizeof(line));
      grid_draw_text(line, tx, grid_text_y(mid + REF(8), height, ss),
            width, height, dcol, TEXT_ALIGN_LEFT, ss);

      /* Downloading: how far, along the bottom of the card. */
      if (u->state == GRID_UPDATE_WORKING && u->progress > 0)
      {
         float bh = floorf(REF(2) + 0.5f);
         float by = cy + card_h - bh - REF(4);
         grid_draw_rounded(p_disp, userdata, width, height,
               tx, by, tw, bh, bh / 2.0f, 0x3a3a3cff);
         grid_draw_rounded(p_disp, userdata, width, height,
               tx, by, tw * (u->progress > 100 ? 100 : u->progress) / 100.0f,
               bh, bh / 2.0f, COL_CURSOR);
      }
   }
}

/* First start: what Bomba needs and why, then a panel per service (its
 * name, what it gives, its fields, and its web page at the bottom, so
 * both panels line up) and Continue under them (see grid_setup_items).
 * Fields are typed into in place (the on-screen keyboard shows the text
 * in its own box). */
static void grid_draw_setup_screen(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height)
{
   unsigned items[GRID_SETUP_MAX];
   float col[GRID_SETUP_MAX], row[GRID_SETUP_MAX];
   unsigned i, n     = grid_setup_items(items, col, row);
   unsigned cols     = (grid_st.setup_key ? 1 : 0) + (grid_st.setup_ra ? 1 : 0);
   unsigned prows    = grid_st.setup_ra ? 3 : 2; /* rows in a panel */
   float ppad        = REF(12);
   float cgap        = 2.0f * ppad + REF(12); /* between panels' cards */
   float col_w       = REF(250);
   float x0          = (width - cols * col_w - (cols - 1) * cgap) / 2.0f;
   float card_h      = REF(36);
   float rgap        = REF(6);
   float cpad        = REF(12);
   float ts          = CAP(GRID_TEXT_CAP);
   float ss          = CAP(GRID_TEXT_CAP - 2);
   float intro_y, panel_y, top, panel_h;
   bool typing       = menu_input_dialog_get_display_kb()
                    && grid_st.kb == KB_SETUP;
   const char *error = bomba_setup_error();
   const char *parts[1];
   char line[256];

   parts[0] = "Setup";
   grid_draw_screen_heading(p_disp, userdata, width, height, parts, 1);

   intro_y = REF(96);
   panel_y = REF(124);
   top     = REF(166);                       /* first row of cards */
   panel_h = top - panel_y + prows * (card_h + rgap) - rgap + ppad;

   /* What's needed, and why. */
   snprintf(line, sizeof(line), "To use Bomba GUI, %s%s%s.",
         grid_st.setup_key ? "enter a SteamGridDB API key" : "",
         cols > 1 ? " and " : "",
         grid_st.setup_ra ? "log in to RetroAchievements" : "");
   grid_draw_text(line, width / 2.0f, grid_text_y(intro_y, height, ts),
         width, height, COL_TEXT, TEXT_ALIGN_CENTER, ts);
   grid_draw_text("This is how you get the names, covers, banners, icons, "
         "and achievements for your roms.",
         width / 2.0f, grid_text_y(intro_y + REF(15), height, ss),
         width, height, COL_TEXT_SOFT, TEXT_ALIGN_CENTER, ss);

   /* The panels: name and what it's for. */
   for (i = 0; i < cols; i++)
   {
      bool key  = grid_st.setup_key && i == 0;
      float px  = floorf(x0 + i * (col_w + cgap) + 0.5f);
      grid_draw_shape(p_disp, userdata, width, height,
            px - ppad, panel_y, col_w + 2.0f * ppad, panel_h, REF(14),
            0x00000060);
      grid_draw_text_bold(key ? "SteamGridDB" : "RetroAchievements", px,
            grid_text_y(panel_y + REF(14), height, ts), width, height,
            COL_TEXT, TEXT_ALIGN_LEFT, ts);
      grid_draw_text(key ? "Covers, banners and icons"
            : "Game names, icons and achievements", px,
            grid_text_y(panel_y + REF(29), height, ss), width, height,
            COL_TEXT_SOFT, TEXT_ALIGN_LEFT, ss);
   }

   for (i = 0; i < n; i++)
   {
      bool sel     = i == grid_st.panel_sel;
      float cw     = col_w;
      float cx     = floorf(x0 + col[i] * (col_w + cgap) + 0.5f);
      float cy     = floorf(top + row[i] * (card_h + rgap) + 0.5f);
      float mid;
      const char *label = NULL, *value = NULL;
      char shown[300], fitted[300];

      switch (items[i])
      {
         case SETUP_KEY:
            label = "API key";
            value = grid_st.setup_griddb;
            break;
         case SETUP_USER:
            label = "Username";
            value = grid_st.setup_user;
            break;
         case SETUP_PASS:
            label = "Password";
            value = grid_st.setup_pass;
            break;
         case SETUP_KEY_PAGE:
            label = "Get an API key";
            break;
         case SETUP_RA_PAGE:
            label = "Create an account";
            break;
         case SETUP_CONTINUE:
            label = bomba_setup_busy() ? "Logging in..." : "Continue";
            cw    = REF(160);
            cx    = floorf((width - cw) / 2.0f + 0.5f);
            cy    = floorf(panel_y + panel_h + REF(12) + 0.5f);
            break;
      }

      mid = cy + card_h / 2.0f;
      grid_hit_add(cx, cy, cw, card_h, HIT_PANEL, i, MENU_ACTION_OK);
      if (sel)
         grid_draw_cursor(p_disp, userdata, width, height,
               cx, cy, cw, card_h, REF(10));

      /* Buttons: a label in the middle; Continue blue once it can go. */
      if (!value)
      {
         bool go = items[i] == SETUP_CONTINUE && grid_setup_ready();
         grid_draw_shape(p_disp, userdata, width, height,
               cx, cy, cw, card_h, REF(10), go ? COL_SELECT : COL_DIVIDER);
         grid_draw_text_bold(label, cx + cw / 2.0f,
               grid_text_y(mid, height, ts), width, height,
               items[i] == SETUP_CONTINUE && !go ? COL_TEXT_DIM : COL_TEXT,
               TEXT_ALIGN_CENTER, ts);
         continue;
      }

      /* Fields: the label above what's typed (stars for the password),
       * live while typing. */
      grid_draw_shape(p_disp, userdata, width, height,
            cx, cy, cw, card_h, REF(10), COL_DIVIDER);
      if (typing && grid_st.setup_typing == items[i])
      {
         const char *buf = menu_input_dialog_get_buffer();
         value = buf ? buf : "";
      }
      if (items[i] == SETUP_PASS)
      {
         size_t len = strlen(value);
         if (len >= sizeof(shown))
            len = sizeof(shown) - 1;
         memset(shown, '*', len);
         shown[len] = '\0';
      }
      else
         strlcpy(shown, value, sizeof(shown));

      /* Empty and not being typed in: just the label, in the middle. */
      if (string_is_empty(shown)
            && !(typing && grid_st.setup_typing == items[i]))
      {
         grid_draw_text(label, cx + cpad, grid_text_y(mid, height, ts),
               width, height, COL_TEXT_DIM, TEXT_ALIGN_LEFT, ts);
         continue;
      }

      grid_draw_text(label, cx + cpad, grid_text_y(mid - REF(8), height, ss),
            width, height, COL_TEXT_DIM, TEXT_ALIGN_LEFT, ss);
      if (typing && grid_st.setup_typing == items[i])
         grid_draw_edit(p_disp, userdata, width, height, shown, cx + cpad,
               mid + REF(7), cw - 2.0f * cpad, ts, COL_TEXT);
      else
      {
         grid_fit_text(false, shown, cw - 2.0f * cpad, height, ts,
               fitted, sizeof(fitted));
         grid_draw_text(fitted, cx + cpad,
               grid_text_y(mid + REF(7), height, ts),
               width, height, COL_TEXT, TEXT_ALIGN_LEFT, ts);
      }
   }

   /* Why the login failed, under Continue. */
   if (!string_is_empty(error) && !bomba_setup_busy())
      grid_draw_text(error, width / 2.0f,
            grid_text_y(panel_y + panel_h + REF(12) + card_h + REF(14),
               height, ss),
            width, height, COL_DANGER, TEXT_ALIGN_CENTER, ss);
}

/* Choosing cover art or a wallpaper: SteamGridDB's options as a grid of
 * previews (portrait for covers, wide for wallpapers). */
static void grid_draw_art_screen(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height)
{
   static size_t first_row = 0;
   bool cover        = grid_st.art_kind == GRID_CHOICE_COVER;
   size_t i, n       = grid_cover_choices_count();
   const char *title = grid_st.menu_game < grid_st.count
      ? grid_st.games[grid_st.menu_game].title : "";
   float left        = REF(35);
   float top         = REF(70);
   float area_h      = REF(364) - top;
   float gap         = REF(10);
   float cell_h      = cover ? REF(126) : REF(70);
   float cell_w      = cover ? cell_h * GRID_TILE_ASPECT
                             : cell_h * (1920.0f / 620.0f);
   float aspect      = cell_w / cell_h;
   float radius      = cover ? cell_w * GRID_COVER_RADIUS : REF(6);
   float ts          = CAP(GRID_TEXT_CAP);
   const char *msg   = NULL;
   unsigned cols;
   size_t rows, sel_row;
   enum grid_choice_apply apply = grid_cover_choose_state();
   int current;                    /* the art in use now, or -1 */

   {
      const char *parts[2];
      parts[0] = cover ? "Change cover art" : "Change wallpaper";
      parts[1] = title;
      grid_draw_screen_heading(p_disp, userdata, width, height, parts, 2);
   }

   if (grid_cover_choices_state() == GRID_META_PENDING)
      msg = "Loading...";
   else if (!n)
      msg = "No other art for this game on SteamGridDB";

   if (msg)
   {
      grid_draw_text(msg, width / 2.0f,
            grid_text_y(top + area_h / 2.0f, height, ts), width, height,
            COL_TEXT_DIM, TEXT_ALIGN_CENTER, ts);
      return;
   }

   /* Preview textures, made for this list. */
   if (grid_st.art_count != n)
   {
      grid_free_art();
      grid_st.art_tex   = (uintptr_t*)calloc(n, sizeof(uintptr_t));
      grid_st.art_tried = (bool*)calloc(n, sizeof(bool));
      if (!grid_st.art_tex || !grid_st.art_tried)
      {
         grid_free_art();
         return;
      }
      grid_st.art_count = n;
   }

   cols = (unsigned)((width - 2.0f * left + gap) / (cell_w + gap));
   if (cols < 1)
      cols = 1;
   grid_st.art_cols = cols;

   /* Start on the game's current art. */
   current = grid_cover_choice_current();
   if (!grid_st.art_sel_set)
   {
      grid_st.art_sel_set = true;
      if (current >= 0)
         grid_st.panel_sel = (unsigned)current;
   }

   /* Keep the selected row visible. */
   rows = (size_t)((area_h + gap) / (cell_h + gap));
   if (rows < 1)
      rows = 1;
   sel_row = grid_st.panel_sel / cols;
   if (sel_row < first_row)
      first_row = sel_row;
   else if (sel_row >= first_row + rows)
      first_row = sel_row - rows + 1;
   grid_draw_scrollbar(p_disp, userdata, width, height, top, area_h,
         first_row, rows, (n + cols - 1) / cols);

   for (i = first_row * cols; i < n && i < (first_row + rows) * cols; i++)
   {
      float x = floorf(left + (i % cols) * (cell_w + gap) + 0.5f);
      float y = floorf(top + (i / cols - first_row) * (cell_h + gap) + 0.5f);
      const char *thumb = grid_cover_choice_thumb(i);

      grid_hit_add(x, y, cell_w, cell_h, HIT_PANEL, (unsigned)i,
            MENU_ACTION_OK);
      if (i == grid_st.panel_sel)
         grid_draw_cursor(p_disp, userdata, width, height,
               x, y, cell_w, cell_h, radius);
      grid_draw_rounded(p_disp, userdata, width, height,
            x, y, cell_w, cell_h, radius, COL_TILE);

      /* Previews load as they arrive and scroll into view. */
      if (!grid_st.art_tried[i] && !string_is_empty(thumb))
      {
         unsigned tw, th;
         grid_st.art_tried[i] = true;
         grid_load_rounded_texture(thumb, &grid_st.art_tex[i], &tw, &th,
               radius / cell_w, aspect);
      }
      if (grid_st.art_tex[i])
         grid_draw_icon_rect(p_disp, userdata, width, height,
               &grid_st.art_tex[i], x, y, cell_w, cell_h);

      /* The one in use now: a "Current" tag in its bottom-left corner. */
      if ((int)i == current)
      {
         float ss = CAP(GRID_TEXT_CAP - 2);
         float th = REF(15);
         float tp = REF(7);
         float tw = grid_text_width_font(true, "Current", height, ss)
                    + 2.0f * tp;
         float tx = x + REF(5);
         float ty = y + cell_h - th - REF(5);

         grid_draw_shape(p_disp, userdata, width, height,
               tx, ty, tw, th, th / 2.0f, COL_SELECT);
         grid_draw_text_bold("Current", tx + tp,
               grid_text_y(ty + th / 2.0f, height, ss), width, height,
               COL_TEXT, TEXT_ALIGN_LEFT, ss);
      }
   }

   /* The chosen one downloading, or failing. */
   if (apply == GRID_CHOICE_APPLY_WORKING || apply == GRID_CHOICE_APPLY_FAILED)
   {
      const char *s = apply == GRID_CHOICE_APPLY_WORKING
         ? "Applying..." : "Couldn't download that one, try another";
      float sw      = grid_text_width(s, height, ts) + REF(28);
      float sh      = REF(26);
      grid_draw_shape(p_disp, userdata, width, height,
            (width - sw) / 2.0f, REF(364) - sh, sw, sh, sh / 2.0f,
            0x000000c0);
      grid_draw_text(s, width / 2.0f,
            grid_text_y(REF(364) - sh / 2.0f, height, ts), width, height,
            COL_TEXT, TEXT_ALIGN_CENTER, ts);
   }
}

/* On-screen keyboard's slide this frame: how far in it is (0 gone .. 1
 * in), easing out as it opens and in as it closes, and turning back from
 * wherever it is if it's reopened or closed midway. */
static float grid_osk_anim(bool open)
{
   static bool was_open;
   static float from, last;
   static retro_time_t start;
   retro_time_t now = cpu_features_get_time_usec();
   float p, q;

   if (open != was_open)
   {
      was_open = open;
      from     = last;
      start    = now;
   }
   p = grid_clamp01((float)(now - start)
         / (open ? GRID_OSK_OPEN_USEC : GRID_OSK_CLOSE_USEC));
   if (open)
   {
      q    = 1.0f - p;
      last = from + (1.0f - from) * (1.0f - q * q * q);
   }
   else
      last = from * (1.0f - p * p);
   return last;
}

/* On-screen keyboard (gamepad mode), like a console's: what's typed in a
 * box at the top, the keys (see grid_osk_key_at) on a dark panel below
 * and the buttons' hints under them. shown: how far in it is (it slides
 * in, box from the top and keys from the bottom); live: open now, else
 * closing, showing what it last did. */
static void grid_draw_keyboard(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, float shown_t, bool live)
{
   static char last_label[128];
   static char last_shown[GRID_EDIT_MAX + 1];
   static const int specials[] = {
      OSK_BACKSPACE, OSK_CANCEL, OSK_OK, OSK_SHIFT, OSK_SYMBOLS, OSK_SPACE
   };
   /* (The keys with a button of their own show it in their corner.) */
   static const char *hint_glyphs[] = { "L3", "A" };
   static const char *hint_labels[] = { "Shift", "Select" };
   /* Reference: 11 key columns 56px apart and a 69px column right of
    * them, rows 38px apart from y 196, on a panel from y 159; the text box
    * above, inset 38px from the keys' sides. */
   float px      = REF(56.4f);
   float py      = REF(37.6f);
   float gap     = floorf(REF(3) + 0.5f);
   float right_w = REF(69);
   float keys_w  = (GRID_OSK_COLS - 1) * px + right_w;
   float x0      = floorf((width - keys_w) / 2.0f + 0.5f);
   float top     = REF(196);
   float panel_y = REF(159);
   float r       = REF(3);
   float ts      = CAP(GRID_TEXT_CAP);
   float ss      = CAP(GRID_TEXT_CAP - 2);
   /* Characters big on their keys, caps about 40% of the key (as on a
    * console's keyboard); named keys at the menu's text size. */
   float cs      = CAP(13);
   float sel_x = 0.0f, sel_y = 0.0f, sel_w = 0.0f, sel_h = 0.0f;
   float bx      = x0 + REF(38);
   float bw      = keys_w - 2.0f * REF(38);
   float by      = REF(31);
   float bh      = REF(103);
   float pad     = REF(14);
   int sel       = grid_osk_key_at(grid_st.osk_row, grid_st.osk_col);
   const char *label = menu_state_get_ptr()->input_dialog_kb_label;
   const char *buf   = menu_input_dialog_get_buffer();
   bool password     = false;
   char shown[GRID_EDIT_MAX + 1];
   float hints_w = 0.0f, hx;
   float hint_y;
   unsigned k;

   grid_osk_prepare();

   if (live)
   {
      /* Setup's fields are named by the field. */
      if (grid_st.kb == KB_SETUP)
      {
         label    = grid_st.setup_typing == SETUP_KEY ? "API key"
                  : grid_st.setup_typing == SETUP_USER ? "Username" : "Password";
         password = grid_st.setup_typing == SETUP_PASS;
      }
      if (!buf)
         buf = "";
      if (password)
      {
         size_t len = strlen(buf);
         if (len >= sizeof(shown))
            len = sizeof(shown) - 1;
         memset(shown, '*', len);
         shown[len] = '\0';
      }
      else
         strlcpy(shown, buf, sizeof(shown));
      /* Kept to show while it slides out. */
      strlcpy(last_label, label ? label : "", sizeof(last_label));
      strlcpy(last_shown, shown, sizeof(last_shown));
   }
   else
      strlcpy(shown, last_shown, sizeof(shown));
   label = last_label;

   /* Sliding: the keys up from below the screen, the box down from above
    * it. */
   {
      float drop = floorf((1.0f - shown_t) * (height - panel_y) + 0.5f);
      top     += drop;
      panel_y += drop;
      hint_y   = REF(402) + drop;
      by      -= floorf((1.0f - shown_t) * (by + bh + REF(4)) + 0.5f);
   }

   /* The screen dimmed; the typed text in its box. */
   grid_draw_rect(p_disp, userdata, width, height, 0, 0, width, height,
         (uint32_t)(0xb0 * shown_t + 0.5f));
   grid_draw_rounded(p_disp, userdata, width, height, bx, by, bw, bh,
         REF(10), 0x232325ff);
   if (!string_is_empty(label))
      grid_draw_text(label, bx + pad, grid_text_y(by + REF(16), height, ss),
            width, height, COL_TEXT_DIM, TEXT_ALIGN_LEFT, ss);
   grid_draw_edit(p_disp, userdata, width, height, shown, bx + pad,
         by + REF(36), bw - 2.0f * pad, ts, COL_TEXT);

   grid_draw_rect(p_disp, userdata, width, height,
         0, panel_y, width, height - panel_y, 0x18181aff);

   for (k = 0; k < BOMBA_KEYMAP_KEYS + ARRAY_SIZE(specials); k++)
   {
      int id   = k < BOMBA_KEYMAP_KEYS ? (int)k
               : specials[k - BOMBA_KEYMAP_KEYS];
      int col  = id % BOMBA_KEYMAP_COLS, row = id / BOMBA_KEYMAP_COLS;
      int cols = 1, rows = 1;
      uint32_t bg = 0x2e2e31ff, fg = COL_TEXT;
      float x, y, w, h;

      switch (id)
      {
         case OSK_BACKSPACE: col = 11; row = 0;            break;
         case OSK_CANCEL:    col = 11; row = 1; rows = 2;  break;
         case OSK_OK:        col = 11; row = 3; rows = 2;  break;
         case OSK_SHIFT:     col = 0;  row = 4;            break;
         case OSK_SYMBOLS:   col = 1;  row = 4;            break;
         case OSK_SPACE:     col = 2;  row = 4; cols = 9;  break;
         default:                                          break;
      }
      x = floorf(x0 + col * px + 0.5f);
      y = floorf(top + row * py + 0.5f);
      w = floorf((col == GRID_OSK_COLS - 1 ? right_w : cols * px) - gap + 0.5f);
      h = floorf(rows * py - gap + 0.5f);

      if (id >= OSK_BACKSPACE)
         bg = 0x232326ff;
      if (id == OSK_OK)
         bg = COL_SELECT;
      if (     (id == OSK_SHIFT && grid_st.osk_shift)
            || (id == OSK_SYMBOLS && grid_st.osk_symbols))
         bg = 0x48484cff;

      /* (Its ring goes on last, over the keys round it.) */
      if (id == sel)
      {
         sel_x = x;
         sel_y = y;
         sel_w = w;
         sel_h = h;
      }
      grid_draw_rounded(p_disp, userdata, width, height, x, y, w, h, r, bg);

      switch (id)
      {
         case OSK_BACKSPACE:
            grid_icon_draw(p_disp, userdata, width, height,
                  &grid_st.icon_backspace, x + w / 2.0f, y + h / 2.0f,
                  h * 0.72f, fg);
            break;
         case OSK_SHIFT:
            grid_icon_draw(p_disp, userdata, width, height,
                  &grid_st.icon_shift, x + w / 2.0f, y + h / 2.0f,
                  h * 0.62f, fg);
            break;
         case OSK_CANCEL:
         case OSK_SYMBOLS:
         case OSK_SPACE:
            grid_draw_text(id == OSK_CANCEL ? "Cancel"
                  : id == OSK_SPACE ? "Space"
                  : grid_st.osk_symbols ? "ABC" : "#+=",
                  x + w / 2.0f, grid_text_y(y + h / 2.0f, height, ts),
                  width, height, fg, TEXT_ALIGN_CENTER, ts);
            break;
         case OSK_OK:
            grid_draw_text("OK", x + w / 2.0f,
                  grid_text_y(y + h / 2.0f, height, ts), width, height,
                  0xffffffff, TEXT_ALIGN_CENTER, ts);
            break;
         default:
            grid_draw_text(grid_osk_label(id), x + w / 2.0f,
                  grid_text_y(y + h / 2.0f, height, cs), width, height,
                  fg, TEXT_ALIGN_CENTER, cs);
            break;
      }

      /* A key with a button of its own shows it in its top-right
       * corner. */
      {
         /* (As actions: Space is on the search button, Cancel on scan.) */
         const char *btn = id == OSK_BACKSPACE ? "B"
                         : id == OSK_SPACE ? "Y" : id == OSK_OK ? "+"
                         : id == OSK_CANCEL ? "X" : NULL;
         if (btn)
         {
            float bd = 2.0f * floorf(REF(11) / 2.0f) + 1.0f;
            grid_draw_button_glyph(p_disp, userdata, width, height,
                  floorf(x + w - bd - REF(4) + 0.5f),
                  floorf(y + REF(4) + 0.5f), bd, btn);
         }
      }
   }

   if (sel_w > 0.0f)
      grid_draw_cursor(p_disp, userdata, width, height,
            sel_x, sel_y, sel_w, sel_h, r);

   /* The buttons, right-aligned under the keys. */
   for (k = 0; k < ARRAY_SIZE(hint_glyphs); k++)
      hints_w += (k ? REF(19) : 0.0f) + grid_draw_hint(p_disp, userdata,
            width, height, 0, hint_y, hint_glyphs[k], hint_labels[k], true);
   hx = x0 + keys_w - gap - hints_w;
   for (k = 0; k < ARRAY_SIZE(hint_glyphs); k++)
      hx += grid_draw_hint(p_disp, userdata, width, height, hx, hint_y,
            hint_glyphs[k], hint_labels[k], false) + REF(19);
}

/* A game's loading screen (see grid_launch), at opacity a: black, the
 * game's name and "Loading..." with stepping dots under it. Laid out as
 * the Switch core's own (eden_libretro.c, loading_draw), which follows it
 * seamlessly. */
static void grid_draw_loading(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, float a)
{
   float ts = CAP(13);
   float ss = CAP(GRID_TEXT_CAP);
   int dots = (int)(cpu_features_get_time_usec() / 400000 % 4);
   char title[NAME_MAX_LENGTH];
   char text[16];

   grid_draw_rect(p_disp, userdata, width, height, 0, 0, width, height,
         grid_fade(0x000000ff, a));

   grid_fit_text(true, grid_st.loading_game.title, width - 2.0f * REF(35),
         height, ts, title, sizeof(title));
   grid_draw_text_bold(title, width / 2.0f, floorf(REF(208) + 0.5f),
         width, height, grid_fade(COL_TEXT, a), TEXT_ALIGN_CENTER, ts);

   /* "Loading" stays put as the dots change. */
   strlcpy(text, "Loading...", sizeof(text));
   text[7 + dots] = '\0';
   grid_draw_text(text,
         floorf((width - grid_text_width("Loading...", height, ss)) / 2.0f + 0.5f),
         floorf(REF(232) + 0.5f), width, height, grid_fade(COL_TEXT_DIM, a),
         TEXT_ALIGN_LEFT, ss);
   grid_flush_text(width, height);
}

/* Copies text to out, cut to fit max_w with "..." when too wide. Never
 * splits a UTF-8 character. */
static void grid_fit_text(bool bold, const char *text, float max_w,
      unsigned height, float scale, char *out, size_t out_len)
{
   size_t len;

   strlcpy(out, text, out_len);
   if (grid_text_width_font(bold, out, height, scale) <= max_w)
      return;

   len = strlen(out);
   while (len > 0)
   {
      do
         len--;
      while (len > 0 && ((unsigned char)out[len] & 0xc0) == 0x80);

      out[len] = '\0';
      while (len > 0 && out[len - 1] == ' ')
         out[--len] = '\0';
      if (len + 4 > out_len)
         continue;
      strlcat(out, "...", out_len);
      if (grid_text_width_font(bold, out, height, scale) <= max_w)
         return;
      out[len] = '\0';
   }
}

/* Tile without a cover: icon or boxart (if any) as a square in the upper
 * part, title and console centred below it. */
static void grid_draw_plain_tile(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, grid_game_t *game,
      float x, float y, float tile_w, float tile_h)
{
   char line[NAME_MAX_LENGTH];
   float pad = tile_w * 0.12f;
   float s   = tile_w - 2.0f * pad;   /* icon size */
   float tw  = tile_w - 2.0f * pad;   /* text width */
   float ts  = CAP(6.5);
   float ss  = CAP(5.5);
   float cx  = x + tile_w / 2.0f;
   float ty  = y + pad + s + REF(14); /* title centre */

   if (game->art && game->art_w && game->art_h)
      grid_draw_image(p_disp, userdata, width, height,
            x + pad, y + pad, s, s, &game->art, game->art_w, game->art_h,
            game->pending ? 0x5a5a5aff : 0xffffffff);
   else
      ty = y + tile_h * 0.45f;

   grid_fit_text(true, game->title, tw, height, ts, line, sizeof(line));
   grid_draw_text_bold(line, cx, grid_text_y(ty, height, ts),
         width, height, COL_TEXT, TEXT_ALIGN_CENTER, ts);

   grid_fit_text(false, game->system, tw, height, ss, line, sizeof(line));
   grid_draw_text(line, cx, grid_text_y(ty + REF(11), height, ss),
         width, height, COL_TEXT_DIM, TEXT_ALIGN_CENTER, ss);
}

/* Whether a menu entry's value is a switch: 1 on, 0 off, -1 not one.
 * Core options with two values count (RetroArch marks them ST_BOOL). */
static int grid_switch_state(const menu_entry_t *entry)
{
   static const char *on_vals[]  = { "enabled",  "true",  "on"  };
   static const char *off_vals[] = { "disabled", "false", "off" };
   const char *v = entry->value;
   unsigned k;

   if (string_is_equal(v, msg_hash_to_str(MENU_ENUM_LABEL_VALUE_ON)))
      return 1;
   if (string_is_equal(v, msg_hash_to_str(MENU_ENUM_LABEL_VALUE_OFF)))
      return 0;
   if (entry->setting_type == ST_BOOL)
      for (k = 0; k < 3; k++)
      {
         if (!grid_strcmp_noncase(v, on_vals[k]))
            return 1;
         if (!grid_strcmp_noncase(v, off_vals[k]))
            return 0;
      }
   return -1;
}

#define GRID_WRAP_LINES 6

/* Splits text into at most max_lines lines no wider than max_w, breaking
 * at spaces and at the text's own line breaks; the last line ends in
 * "..." if the text is cut. Returns the line count. */
static unsigned grid_wrap_text(bool bold, const char *text, float max_w,
      unsigned height, float scale, char lines[][256], unsigned max_lines)
{
   const char *p = text ? text : "";
   unsigned n    = 0;
   char cur[256];

   cur[0] = '\0';
   while (*p && n < max_lines)
   {
      const char *w;
      char cand[256];
      int wl;

      if (*p == '\n')
      {
         strlcpy(lines[n++], cur, 256);
         cur[0] = '\0';
         p++;
         continue;
      }
      if (*p == ' ')
      {
         p++;
         continue;
      }

      for (w = p; *p && *p != ' ' && *p != '\n'; p++);
      wl = (int)(p - w);
      snprintf(cand, sizeof(cand), "%s%s%.*s", cur, *cur ? " " : "", wl, w);

      if (*cur && grid_text_width_font(bold, cand, height, scale) > max_w)
      {
         strlcpy(lines[n++], cur, 256);
         snprintf(cur, sizeof(cur), "%.*s", wl, w);
      }
      else
         strlcpy(cur, cand, sizeof(cur));
   }

   if (*cur && n < max_lines)
   {
      strlcpy(lines[n++], cur, 256);
      cur[0] = '\0';
   }

   /* Cut short: say so on the last line. */
   if ((*cur || *p) && n)
   {
      char t[260];
      snprintf(t, sizeof(t), "%s...", lines[n - 1]);
      grid_fit_text(bold, t, max_w, height, scale, lines[n - 1], 256);
   }

   /* No trailing blank lines. */
   while (n && !*lines[n - 1])
      n--;
   return n;
}

/* ---- Control binds ----------------------------------------------------
 * A control (a game button or a shortcut) has a controller button and a
 * keyboard key of its own: binding with the pad sets only the first, with
 * the keyboard only the second (RetroArch's binding works that way). They
 * show as two columns. */

/* The bind a menu row edits, or NULL if it isn't a control. */
static const struct retro_keybind *grid_row_keybind(file_list_t *list,
      size_t i, unsigned *port)
{
   menu_file_list_cbs_t *cbs;
   rarch_setting_t *setting;

   if (!list || i >= list->size)
      return NULL;
   cbs     = (menu_file_list_cbs_t*)list->list[i].actiondata;
   setting = cbs ? cbs->setting : NULL;
   if (!setting || setting->type != ST_BIND)
      return NULL;
   if (port)
      *port = setting->index_offset;
   return setting->value.target.keybind;
}

/* Controller side of a bind: the button's name from the controller's
 * profile where it has one ("A", "D-pad up"), else a generic one. Sets
 * *custom if it was set by hand rather than by the profile. Empty: none. */
static void grid_bind_pad_text(const struct retro_keybind *b, unsigned port,
      char *s, size_t len, bool *custom)
{
   const struct retro_keybind *e = b;
   const struct retro_keybind *profile;
   unsigned id;

   s[0]    = '\0';
   *custom = b->joykey != NO_BTN || b->joyaxis != AXIS_NONE;
   if (!*custom)
      e = input_config_get_bind_auto(port, b->id);
   if (!e)
      return;

   if (e->joykey != NO_BTN)
   {
      /* A label for this button anywhere in the controller's profile. */
      for (id = 0; id < RARCH_BIND_LIST_END; id++)
      {
         profile = input_config_get_bind_auto(port, id);
         if (     profile && profile->joykey == e->joykey
               && !string_is_empty(profile->joykey_label))
         {
            strlcpy(s, profile->joykey_label, len);
            return;
         }
      }
      if (GET_HAT_DIR(e->joykey))
      {
         unsigned dir = GET_HAT_DIR(e->joykey);
         strlcpy(s, dir == HAT_UP_MASK ? "D-pad up"
               : dir == HAT_DOWN_MASK  ? "D-pad down"
               : dir == HAT_LEFT_MASK  ? "D-pad left" : "D-pad right", len);
      }
      else
         snprintf(s, len, "Button %u", (unsigned)e->joykey);
   }
   else if (e->joyaxis != AXIS_NONE)
   {
      for (id = 0; id < RARCH_BIND_LIST_END; id++)
      {
         profile = input_config_get_bind_auto(port, id);
         if (     profile && profile->joyaxis == e->joyaxis
               && !string_is_empty(profile->joyaxis_label))
         {
            strlcpy(s, profile->joyaxis_label, len);
            return;
         }
      }
      if (AXIS_POS_GET(e->joyaxis) != AXIS_DIR_NONE)
         snprintf(s, len, "Stick %u+", (unsigned)AXIS_POS_GET(e->joyaxis));
      else
         snprintf(s, len, "Stick %u-", (unsigned)AXIS_NEG_GET(e->joyaxis));
   }
}

/* Keyboard side of a bind ("X", "Enter"). Empty: none. */
static void grid_bind_key_text(const struct retro_keybind *b,
      char *s, size_t len)
{
   s[0] = '\0';
   if (b->key == RETROK_UNKNOWN)
      return;
   input_keymaps_translate_rk_to_str(b->key, s, len);
   if (string_is_equal(s, "nul"))
      s[0] = '\0';
   else if (s[0])
      s[0] = (char)toupper((unsigned char)s[0]);
}

/* Whether RetroArch is waiting for a button or key to bind. */
static bool grid_binding(void)
{
   return (menu_state_get_ptr()->flags & MENU_ST_FLAG_IS_BINDING) ? true
      : false;
}

/* A value that left / right cycles: centred in a box of fixed width
 * ending at right, with "<" and ">" at the box's edges on the selected
 * row, so the values and brackets line up row to row. */
static void grid_draw_cycle_value(const char *value, float right, float box_w,
      float ty, bool selected, uint32_t color, unsigned width,
      unsigned height, float ts)
{
   float bx = right - box_w;

   grid_draw_text(value, bx + box_w / 2.0f, ty, width, height, color,
         TEXT_ALIGN_CENTER, ts);
   if (selected)
   {
      grid_draw_text("<", bx, ty, width, height, COL_TEXT, TEXT_ALIGN_LEFT,
            ts);
      grid_draw_text(">", right, ty, width, height, COL_TEXT,
            TEXT_ALIGN_RIGHT, ts);
   }
}

/* A category's Consoles page: the consoles; one console's settings of the
 * category; or its Bindings. Cycled values sit centred between "<" ">"
 * (dimmed when default), in the Settings list's row style. */
static void grid_draw_cc_screen(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height)
{
   static unsigned first = 0;
   static grid_cc_row_t rows[GRID_CC_MAX_ROWS];
   const char *consoles[GRID_MAX_TABS];
   const char *parts[4];
   enum grid_cc_bucket bucket = grid_cc_bucket();
   unsigned n_consoles = grid_cc_consoles(consoles);
   const char *console = grid_cc_console();
   bool listing        = grid_st.cc_level == 1 || !console;
   unsigned n          = listing ? n_consoles
                       : grid_cc_rows(rows, GRID_CC_MAX_ROWS);
   unsigned sel        = grid_st.cc_sel;
   unsigned rows_fit, i, k, np = 0;
   float left          = REF(35);
   float top           = REF(88);
   float area_h        = REF(358) - top;
   float row_h         = REF(28);
   float inset         = REF(3);
   float pad           = REF(12);
   float list_w        = width - 2.0f * left;
   float ts            = CAP(GRID_TEXT_CAP);
   float box_w         = 0.0f;
   /* Bindings: gamepad and keyboard columns (left edges). */
   float col_w         = REF(84);
   float key_x         = left + list_w - pad - col_w;
   float pad_x         = key_x - REF(12) - col_w;

   /* "Graphics | Consoles | SNES", or a game's "Super Metroid | SNES". */
   if (grid_st.game_settings)
   {
      parts[np++] = grid_st.game_title;
      if (console)
         parts[np++] = console;
   }
   else
   {
      parts[np++] = grid_cc_bucket_name[bucket];
      parts[np++] = "Consoles";
      if (!listing)
         parts[np++] = console;
   }
   if (!listing && grid_st.cc_level == 3)
      parts[np++] = "Bindings";
   grid_draw_screen_heading(p_disp, userdata, width, height, parts, np);

   if (!n)
   {
      grid_draw_text(listing
            ? "Consoles show here once you have games for them"
            : "Nothing here", width / 2.0f,
            grid_text_y(top + area_h / 2.0f, height, ts),
            width, height, COL_TEXT_SOFT, TEXT_ALIGN_CENTER, ts);
      return;
   }

   /* The value box: as wide as the widest value it can show here. */
   for (i = 0; !listing && i < n; i++)
   {
      float w = 0.0f;
      switch (rows[i].kind)
      {
         case CC_ROW_EMULATOR:
            for (k = 0; k < grid_cc_cores.count; k++)
            {
               w = grid_text_width(grid_cc_core_name(grid_cc_cores.paths[k]),
                     height, ts);
               if (w > box_w)
                  box_w = w;
            }
            break;
         case CC_ROW_OPT:
            for (k = 0; k < grid_coreopts_value_count(rows[i].arg); k++)
            {
               w = grid_text_width(grid_coreopts_value_label(rows[i].arg, k),
                     height, ts);
               if (w > box_w)
                  box_w = w;
            }
            break;
         default:
            break;
      }
   }
   box_w += 2.0f * REF(22); /* room for the brackets */
   if (box_w > list_w * 0.5f)
      box_w = list_w * 0.5f;

   rows_fit = (unsigned)(area_h / row_h);
   if (rows_fit < 1)
      rows_fit = 1;
   if (sel >= n)
      sel = n - 1;
   if (sel < first + 1)
      first = sel ? sel - 1 : 0;
   else if (sel + 2 > first + rows_fit)
      first = sel + 2 - rows_fit;
   if (first + rows_fit > n)
      first = n > rows_fit ? n - rows_fit : 0;
   grid_draw_scrollbar(p_disp, userdata, width, height, top,
         rows_fit * row_h, first, rows_fit, n);

   for (i = first; i < n && i < first + rows_fit; i++)
   {
      float ry           = floorf(top + (i - first) * row_h + inset + 0.5f);
      float rh           = floorf(row_h - 2.0f * inset + 0.5f);
      float ty           = grid_text_y(ry + rh / 2.0f, height, ts);
      const char *label  = "";
      const char *value  = NULL;
      bool cycle         = false;   /* value cycled with left / right */
      uint32_t label_col = COL_TEXT;
      uint32_t value_col = COL_TEXT_SOFT;
      char note[256], fit[256];
      float label_w;
      int bind_id        = -1;      /* a Bindings row: its control */

      grid_hit_add(left, ry, list_w, rh, HIT_CC, i, MENU_ACTION_OK);
      if (i == sel && !(  !listing && rows[i].kind == CC_ROW_NOTE))
      {
         grid_draw_cursor(p_disp, userdata, width, height,
               left, ry, list_w, rh, REF(7));
         grid_draw_shape(p_disp, userdata, width, height,
               left, ry, list_w, rh, REF(7), COL_DIVIDER);
      }

      if (listing)
      {
         /* Where the console has its own choices here, whether it uses
          * them; else a page to open. */
         bool changed = false, status = true;
         if (bucket == CC_SYSTEM)
            changed = !string_is_empty(grid_controls_core(consoles[i]));
         else if (bucket == CC_CONTROLS)
            changed = grid_controls_changed(consoles[i]);
         else
            status  = false;
         label     = consoles[i];
         value     = !status ? "..." : changed ? "Changed" : "Default";
         value_col = status && !changed ? COL_TEXT_DIM : COL_TEXT_SOFT;
         if (status && changed)
            value_col = COL_TEXT;
      }
      else
         switch (rows[i].kind)
         {
            case CC_ROW_EMULATOR:
               label     = "Emulator";
               value     = grid_cc_core_name(grid_cc_cores.current);
               value_col = string_is_empty(grid_controls_core(console))
                  ? COL_TEXT_DIM : COL_TEXT;
               cycle     = grid_cc_cores.count > 1;
               break;
            case CC_ROW_BINDINGS:
               label     = "Bindings";
               value     = "...";
               break;
            case CC_ROW_OPT:
            {
               unsigned opt = (unsigned)rows[i].arg;
               unsigned cur = grid_coreopts_value(opt);
               label     = grid_coreopts_name(opt);
               value     = grid_coreopts_value_label(opt, cur);
               value_col = cur == grid_coreopts_default(opt)
                  ? COL_TEXT_DIM : COL_TEXT;
               cycle     = true;
               break;
            }
            case CC_ROW_RESET:
               label     = "Reset all to default";
               break;
            case CC_ROW_BUTTON:
               /* Its two columns are drawn below. */
               label     = grid_cc_control_name((unsigned)rows[i].arg);
               bind_id   = rows[i].arg;
               break;
            case CC_ROW_NOTE:
               strlcpy(note, rows[i].arg   /* known, none here */
                     ? "Nothing to set here"
                     : "Play a game once to see more settings", sizeof(note));
               label     = note;
               label_col = COL_TEXT_DIM;
               break;
         }

      label_w = bind_id >= 0 ? pad_x - REF(12) - (left + pad)
         : list_w - 2.0f * pad - (value ? (cycle ? box_w
               : grid_text_width(value, height, ts)) + REF(16) : 0.0f);
      grid_fit_text(false, label, label_w, height, ts, fit, sizeof(fit));
      grid_draw_text(fit, left + pad, ty, width, height, label_col,
            TEXT_ALIGN_LEFT, ts);
      if (value && cycle)
      {
         grid_fit_text(false, value, box_w - 2.0f * REF(22), height, ts,
               fit, sizeof(fit));
         grid_draw_cycle_value(fit, left + list_w - pad, box_w, ty,
               i == sel, value_col, width, height, ts);
         /* Its "<" and ">": back and on (as left / right). */
         grid_hit_add(left + list_w - pad - box_w, ry, REF(22), rh, HIT_CC,
               i, MENU_ACTION_LEFT);
         grid_hit_add(left + list_w - pad - REF(22), ry, REF(22) + pad, rh,
               HIT_CC, i, MENU_ACTION_RIGHT);
      }
      else if (value)
         grid_draw_text(value, left + list_w - pad, ty, width, height,
               value_col, TEXT_ALIGN_RIGHT, ts);

      /* A control: its gamepad button and keyboard key for this console
       * (its own bright, the RetroPad setup's soft), or "Press..." while
       * one is being bound. */
      if (bind_id >= 0)
      {
         const struct retro_keybind *rp = &input_config_binds[0][bind_id];
         struct retro_keybind tmp       = *rp;
         uint16_t joykey;
         uint32_t joyaxis;
         unsigned key;
         bool custom = false;
         char pad_s[64], key_s[64];

         grid_controls_get_bind(console, (unsigned)bind_id, &joykey,
               &joyaxis, &key);

         if (     grid_cc_binding.active
               && grid_cc_binding.id == (unsigned)bind_id)
         {
            int64_t left_us = menu_state_get_ptr()
               ->input_binds.timer_timeout.timeout_us;
            unsigned secs   = left_us > 0
               ? (unsigned)((left_us + 999999) / 1000000) : 0;
            float pulse     = 0.5f + 0.5f * sinf(
                  cpu_features_get_time_usec() / 1000000.0f * 4.0f);
            char prompt[64];
            snprintf(prompt, sizeof(prompt), "Press a button or key  %u",
                  secs);
            grid_draw_text(prompt, pad_x, ty, width, height,
                  grid_mix(COL_SELECT, COL_SELECT_HI, pulse),
                  TEXT_ALIGN_LEFT, ts);
            continue;
         }

         grid_draw_divider(p_disp, userdata, width, height,
               key_x - REF(6), ry + rh / 2.0f);

         /* Gamepad. */
         if (joykey != NO_BTN || joyaxis != AXIS_NONE)
         {
            tmp.joykey  = joykey;
            tmp.joyaxis = joyaxis;
         }
         grid_bind_pad_text(&tmp, 0, pad_s, sizeof(pad_s), &custom);
         grid_fit_text(false, string_is_empty(pad_s) ? "-" : pad_s, col_w,
               height, ts, fit, sizeof(fit));
         grid_draw_text(fit, pad_x, ty, width, height,
               string_is_empty(pad_s) ? COL_TEXT_DIM
               : (joykey != NO_BTN || joyaxis != AXIS_NONE) ? COL_TEXT
               : COL_TEXT_SOFT, TEXT_ALIGN_LEFT, ts);

         /* Keyboard. */
         tmp.key = key != RETROK_UNKNOWN ? (enum retro_key)key : rp->key;
         grid_bind_key_text(&tmp, key_s, sizeof(key_s));
         grid_fit_text(false, string_is_empty(key_s) ? "-" : key_s, col_w,
               height, ts, fit, sizeof(fit));
         grid_draw_text(fit, key_x, ty, width, height,
               string_is_empty(key_s) ? COL_TEXT_DIM
               : key != RETROK_UNKNOWN ? COL_TEXT : COL_TEXT_SOFT,
               TEXT_ALIGN_LEFT, ts);
      }
   }

   /* Bindings: column captions over the two. */
   if (grid_st.cc_level == 3 && !listing)
   {
      float cs = CAP(GRID_TEXT_CAP - 1.5f);
      float cy = grid_text_y(REF(70), height, cs);
      grid_draw_text("Gamepad", pad_x, cy, width, height, COL_TEXT_DIM,
            TEXT_ALIGN_LEFT, cs);
      grid_draw_divider(p_disp, userdata, width, height, key_x - REF(6),
            REF(70));
      grid_draw_text("Keyboard", key_x, cy, width, height, COL_TEXT_DIM,
            TEXT_ALIGN_LEFT, cs);
   }

   /* A console's settings: its emulator's are the emulator's, so other
    * consoles it plays share them. Say which. */
   if (     grid_st.cc_level == 2 && console && grid_cc_cores.current[0]
         && n && (rows[0].kind == CC_ROW_OPT
            || (n > 1 && rows[1].kind == CC_ROW_OPT)))
   {
      const char *others[4];
      char note[256];
      unsigned no = 0;
      size_t g;

      for (g = 0; g < grid_st.count && no < 4; g++)
      {
         const grid_game_t *game = &grid_st.games[g];
         unsigned j;
         bool seen = false;

         if (     string_is_equal(game->system, console)
               || !string_is_equal(game->core_path, grid_cc_cores.current))
            continue;
         for (j = 0; j < no; j++)
            if (string_is_equal(others[j], game->system))
               seen = true;
         if (!seen)
            others[no++] = game->system;
      }

      /* Bottom left, on the hints' row. */
      if (no)
      {
         size_t len = strlcpy(note, "These settings also apply to ",
               sizeof(note));
         for (k = 0; k < no && len < sizeof(note); k++)
            len += (size_t)snprintf(note + len, sizeof(note) - len, "%s%s",
                  k ? (k + 1 == no ? " and " : ", ") : "", others[k]);
         grid_draw_text(note, REF(20), grid_text_y(REF(390), height, ts),
               width, height, COL_TEXT_DIM, TEXT_ALIGN_LEFT, ts);
      }
   }
}

/* Settings screen: the current RetroArch menu list (any depth) as rows
 * straight on the background, label on the left and value on the right;
 * switches as toggles, the chosen entry of a choice list with a lime dot.
 * The selected row also shows what it does underneath. RetroArch moves
 * the selection and changes values; this only draws. */
static void grid_draw_settings_screen(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height)
{
   static size_t first = 0;
   struct menu_state *menu_st = menu_state_get_ptr();
   file_list_t *list = MENU_LIST_GET_SELECTION(menu_st->entries.list, 0);
   size_t i, n       = list ? list->size : 0;
   size_t sel        = menu_st->selection_ptr;
   float left        = REF(35);
   float top         = REF(88);
   float area_h      = REF(358) - top;
   float row_h       = REF(28);
   float inset       = REF(3);  /* row box inside its slot, top and bottom */
   float pad         = REF(12); /* text inside a row */
   float list_w      = width - 2.0f * left;
   float ts          = CAP(GRID_TEXT_CAP);
   float ds          = CAP(GRID_TEXT_CAP - 1.5f); /* description text */
   float desc_lh     = REF(13);                   /* its line height */
   const char *parts[4];
   char title[NAME_MAX_LENGTH];
   char desc[GRID_WRAP_LINES][256];
   unsigned np = 0, desc_n = 0;
   size_t rows;
   float sel_h, y;
   /* Controls: controller and keyboard columns (left edges), and the bind
    * RetroArch is waiting for, if any. */
   float col_w       = REF(84);
   float key_x       = left + list_w - pad - col_w;
   float pad_x       = key_x - REF(12) - col_w;
   bool binding      = grid_binding();
   const struct retro_keybind *bind_out = binding
      ? menu_st->input_binds.output : NULL;
   bool has_binds    = false;

   if (grid_st.cc_level)
   {
      grid_draw_cc_screen(p_disp, userdata, width, height);
      return;
   }

   /* While binding, the row being bound is the one shown selected (with
    * "Set all controls" that moves down the list). */
   for (i = 0; i < n; i++)
   {
      const struct retro_keybind *kb = grid_row_keybind(list, i, NULL);
      if (!kb)
         continue;
      has_binds = true;
      if (bind_out && kb == bind_out)
         sel = i;
   }

   /* Heading: the category (or Quick settings and the game), then the
    * page open below it (not RetroArch's binding step). */
   title[0] = '\0';
   if (grid_menu_depth() > grid_settings_base() && !binding)
      menu_entries_get_title(title, sizeof(title));
   if (grid_st.settings_quick)
   {
      parts[np++] = "Quick settings";
      parts[np++] = grid_st.menu_game < grid_st.count
         ? grid_st.games[grid_st.menu_game].title : "";
   }
   else
      parts[np++] = grid_settings_cats[grid_st.settings_cat
         < GRID_SETTINGS_CAT_COUNT ? grid_st.settings_cat : 0].name;

   /* A player's controls: "RetroPad" (and "Bindings"), not "Port 1
    * Controls". */
   {
      file_list_t *stack = grid_menu_stack();
      const char *top_lbl = stack && stack->size
         ? stack->list[stack->size - 1].label : NULL;
      if (     !grid_st.advanced && top_lbl
            && string_is_equal(top_lbl,
               msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_USER_BINDS_LIST)))
      {
         strlcpy(title, "RetroPad", sizeof(title));
         if (grid_user_binds_depth() >= 2)
         {
            parts[np++] = "RetroPad";
            strlcpy(title, "Bindings", sizeof(title));
         }
      }
   }
   parts[np++] = title;
   grid_draw_screen_heading(p_disp, userdata, width, height, parts, np);

   /* Column captions over the controls' two binds. */
   if (has_binds)
   {
      float cs = CAP(GRID_TEXT_CAP - 1.5f);
      float cy = grid_text_y(REF(70), height, cs);
      grid_draw_text("Gamepad", pad_x, cy, width, height, COL_TEXT_DIM,
            TEXT_ALIGN_LEFT, cs);
      grid_draw_divider(p_disp, userdata, width, height, key_x - REF(6),
            REF(70));
      grid_draw_text("Keyboard", key_x, cy, width, height, COL_TEXT_DIM,
            TEXT_ALIGN_LEFT, cs);
   }

   if (!n)
   {
      grid_draw_text("Nothing here", width / 2.0f,
            grid_text_y(top + area_h / 2.0f, height, ts), width, height,
            COL_TEXT_SOFT, TEXT_ALIGN_CENTER, ts);
      return;
   }

   if (sel >= n)
      sel = n - 1;

   /* What the selected entry does, wrapped to the row's width: shown under
    * its name, the row growing to fit (the rows after it move down). */
   {
      menu_entry_t entry;
      char text[MENU_LABEL_MAX_LENGTH];

      MENU_ENTRY_INITIALIZE(entry);
      entry.flags |= MENU_ENTRY_FLAG_SUBLABEL_ENABLED;
      menu_entry_get(&entry, 0, sel, NULL, true);
      strlcpy(text, entry.sublabel, sizeof(text));

      /* Controls need no description: the row says it all (and shows
       * the "press" prompt itself while binding). */
      if (grid_row_keybind(list, sel, NULL))
         text[0] = '\0';

      desc_n = grid_wrap_text(false, text, list_w - 2.0f * pad,
            height, ds, desc, GRID_WRAP_LINES);
   }
   sel_h = row_h + (desc_n ? desc_n * desc_lh + REF(6) : 0.0f);

   /* Keep the selection a row away from the edges where there is one:
    * the row before it, it (grown) and the row after it on screen. */
   rows = (size_t)(area_h / row_h);
   if (rows < 1)
      rows = 1;
   if (sel < first + 1)
      first = sel ? sel - 1 : 0;
   while (     first < sel
         && (sel - first) * row_h + sel_h + (sel + 1 < n ? row_h : 0.0f)
            > area_h)
      first++;
   /* No gap at the end when scrolled down. */
   while (first > 0)
   {
      float used = (n - first) * row_h + (sel_h - row_h);
      if (used + row_h > area_h)
         break;
      first--;
   }
   /* (About as many rows show as fit, less the selection's growth.) */
   {
      size_t shown = (size_t)((area_h - (sel_h - row_h)) / row_h);
      grid_draw_scrollbar(p_disp, userdata, width, height, top, area_h,
            first, shown ? shown : 1, n);
   }

   y = top;
   for (i = first; i < n; i++)
   {
      menu_entry_t entry;
      char line[MENU_LABEL_MAX_LENGTH];
      char named[64];
      float slot_h  = i == sel ? sel_h : row_h;
      float ry      = floorf(y + inset + 0.5f);
      float rh      = floorf(slot_h - 2.0f * inset + 0.5f);
      float mid     = ry + (row_h - 2.0f * inset) / 2.0f; /* name line */
      float ty      = grid_text_y(mid, height, ts);
      float vx      = left + list_w - pad; /* value's right edge */
      float value_w = 0.0f;
      float label_w;
      const char *label;
      const struct retro_keybind *kb;
      unsigned kb_port = 0;
      int sw;

      if (y + slot_h > top + area_h + 0.5f)
         break;
      y += slot_h;
      kb = grid_row_keybind(list, i, &kb_port);
      grid_hit_add(left, ry, list_w, rh, HIT_MENU, (unsigned)i,
            MENU_ACTION_OK);

      MENU_ENTRY_INITIALIZE(entry);
      entry.flags |= MENU_ENTRY_FLAG_PATH_ENABLED
                   | MENU_ENTRY_FLAG_RICH_LABEL_ENABLED
                   | MENU_ENTRY_FLAG_VALUE_ENABLED;
      menu_entry_get(&entry, 0, i, NULL, true);
      if (!(label = grid_settings_label(entry.enum_idx, named, sizeof(named))))
         label = !string_is_empty(entry.rich_label)
            ? entry.rich_label : entry.path;

      /* The grid's own submenus show "..." like RetroArch's. */
      if (     list->list[i].label
            && (  string_is_equal(list->list[i].label, GRID_BINDS_LABEL)
               || string_is_equal(list->list[i].label, GRID_CC_LABEL)))
         strlcpy(entry.value, "...", sizeof(entry.value));

      if (i == sel)
      {
         unsigned k;

         grid_draw_cursor(p_disp, userdata, width, height,
               left, ry, list_w, rh, REF(7));
         grid_draw_shape(p_disp, userdata, width, height,
               left, ry, list_w, rh, REF(7), COL_DIVIDER);

         for (k = 0; k < desc_n; k++)
            grid_draw_text(desc[k], left + pad,
                  grid_text_y(ry + (row_h - 2.0f * inset) + REF(1)
                     + (k + 0.5f) * desc_lh, height, ds),
                  width, height, COL_TEXT_SOFT, TEXT_ALIGN_LEFT, ds);
      }

      if (kb)
      {
         /* A control: its controller button and keyboard key in their
          * columns (set by hand bright, from the controller's profile
          * soft, none a dash); "Press..." in both while it is being
          * bound. */
         char pad_s[64], key_s[64];
         bool custom = false;

         if (bind_out && kb == bind_out)
         {
            /* Waiting: "Press a button or key  3" across both columns. */
            int64_t left_us = menu_st->input_binds.timer_timeout.timeout_us;
            unsigned secs   = left_us > 0
               ? (unsigned)((left_us + 999999) / 1000000) : 0;
            float pulse     = 0.5f + 0.5f * sinf(
                  cpu_features_get_time_usec() / 1000000.0f * 4.0f);
            char prompt[64];

            snprintf(prompt, sizeof(prompt), "Press a button or key  %u",
                  secs);
            grid_draw_text(prompt, pad_x, ty, width, height,
                  grid_mix(COL_SELECT, COL_SELECT_HI, pulse),
                  TEXT_ALIGN_LEFT, ts);
         }
         else
         {
            grid_draw_divider(p_disp, userdata, width, height,
                  key_x - REF(6), mid);

            grid_bind_pad_text(kb, kb_port, pad_s, sizeof(pad_s), &custom);
            grid_bind_key_text(kb, key_s, sizeof(key_s));

            grid_fit_text(false, string_is_empty(pad_s) ? "-" : pad_s,
                  col_w, height, ts, line, sizeof(line));
            grid_draw_text(line, pad_x, ty, width, height,
                  string_is_empty(pad_s) ? COL_TEXT_DIM
                  : custom ? COL_TEXT : COL_TEXT_SOFT, TEXT_ALIGN_LEFT, ts);

            grid_fit_text(false, string_is_empty(key_s) ? "-" : key_s,
                  col_w, height, ts, line, sizeof(line));
            grid_draw_text(line, key_x, ty, width, height,
                  string_is_empty(key_s) ? COL_TEXT_DIM : COL_TEXT,
                  TEXT_ALIGN_LEFT, ts);
         }
         value_w = left + list_w - pad - pad_x;
      }
      else if ((sw = grid_switch_state(&entry)) >= 0)
      {
         /* Toggle: a capsule with a white knob, lime with the knob right
          * (on), or translucent white with it left (off; lightens what is
          * behind, like the dividers, so it shows on any background). */
         float th = floorf(REF(14) + 0.5f);
         float tw = floorf(REF(26) + 0.5f);
         float kd = th - 2.0f * floorf(REF(2) + 0.5f);
         float tx = floorf(vx - tw + 0.5f);
         float tyo = floorf(mid - th / 2.0f + 0.5f);
         float m  = (th - kd) / 2.0f;

         if (sw)
            grid_draw_rounded(p_disp, userdata, width, height,
                  tx, tyo, tw, th, th / 2.0f, COL_LIME);
         else
            grid_draw_shape(p_disp, userdata, width, height,
                  tx, tyo, tw, th, th / 2.0f, 0xffffff4c);
         grid_draw_rounded(p_disp, userdata, width, height,
               sw ? tx + tw - m - kd : tx + m, tyo + m, kd, kd, kd / 2.0f,
               0xffffffff);
         value_w = tw;
      }
      else if (entry.flags & MENU_ENTRY_FLAG_CHECKED)
      {
         float d = floorf(REF(8) + 0.5f);
         grid_draw_rounded(p_disp, userdata, width, height,
               floorf(vx - d + 0.5f), floorf(mid - d / 2.0f + 0.5f), d, d,
               d / 2.0f, COL_LIME);
         value_w = d;
      }
      else if (!string_is_empty(entry.value))
      {
         const char *value = entry.enum_idx == MENU_ENUM_LABEL_CHEEVOS_PASSWORD
            ? entry.password_value : entry.value;
         grid_fit_text(false, value, list_w * 0.45f, height, ts,
               line, sizeof(line));
         value_w = grid_text_width(line, height, ts);
         grid_draw_text(line, vx, ty, width, height, COL_TEXT_SOFT,
               TEXT_ALIGN_RIGHT, ts);
      }

      label_w = list_w - 2.0f * pad - (value_w > 0.0f ? value_w + REF(16) : 0.0f);
      if (label_w <= 0.0f)
         continue;
      if (i == sel)
         grid_draw_marquee(p_disp, userdata, false, label, left + pad, ty,
               label_w, width, height, COL_TEXT, ts, TEXT_ALIGN_LEFT,
               (const void*)(uintptr_t)(i + 1));
      else
      {
         grid_fit_text(false, label, label_w, height, ts, line, sizeof(line));
         grid_draw_text(line, left + pad, ty, width, height, COL_TEXT,
               TEXT_ALIGN_LEFT, ts);
      }
   }
}

/* RetroArch's message box (an entry's help, confirmations) or the prompt
 * of its text entry, centred in a dark rounded box. */
static void grid_draw_message_box(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, const char *msg, float cy)
{
   struct string_list lines = {0};
   char wrapped[sizeof(grid_st.box_msg) + 256];
   float ts     = CAP(GRID_TEXT_CAP);
   float line_h = REF(17);
   float pad    = REF(16);
   float max_w  = width * 0.7f;
   float char_w = grid_text_width("n", height, ts);
   float w      = 0.0f, h, y;
   unsigned k;

   if (string_is_empty(msg) || char_w <= 0.0f)
      return;

   word_wrap(wrapped, sizeof(wrapped), msg, strlen(msg),
         (int)(max_w / char_w), 0, 0);

   string_list_initialize(&lines);
   if (!string_split_noalloc(&lines, wrapped, "\n") || !lines.size)
   {
      string_list_deinitialize(&lines);
      return;
   }

   for (k = 0; k < lines.size; k++)
   {
      float lw = grid_text_width(lines.elems[k].data, height, ts);
      if (lw > w)
         w = lw;
   }
   if (w > max_w)
      w = max_w;
   h = lines.size * line_h;
   y = cy - h / 2.0f;

   grid_draw_shape(p_disp, userdata, width, height,
         (width - w) / 2.0f - pad, y - pad, w + 2.0f * pad, h + 2.0f * pad,
         REF(12), 0x1c1c1ef0);

   for (k = 0; k < lines.size; k++)
      grid_draw_text(lines.elems[k].data, width / 2.0f,
            grid_text_y(y + (k + 0.5f) * line_h, height, ts), width, height,
            COL_TEXT, TEXT_ALIGN_CENTER, ts);

   string_list_deinitialize(&lines);
}

bool grid_home_frame(video_frame_info_t *video_info)
{
   size_t i, k, sel_col, cols_visible;
   size_t gm_i = (size_t)-1; /* game menu's tile in the view */
   float gm_e = 0.0f, gm_ca = 0.0f, gm_push = 0.0f;
   bool gm_show;
   float tile_w, tile_h, col_gap, row_gap, left, radius, grid_top, col_w;
   float target;
   float zoom_t = 1.0f, old_tile_h = 0.0f, old_tile_w = 0.0f, old_col_w = 0.0f;
   float new_tile_w, new_tile_h;
   unsigned old_rows = 0;
   void *userdata                 = video_info->userdata;
   unsigned width                 = video_info->width;
   unsigned height                = video_info->height;
   gfx_display_t *p_disp          = (gfx_display_t*)video_info->disp_userdata;
   video_driver_state_t *video_st = video_state_get_ptr();
   settings_t *settings           = config_get_ptr();
   bool in_settings               = grid_settings_shown();
   /* The Settings, achievements, art, picture and Installs screens take
    * the games grid's place. */
   bool show_games                = !in_settings
                                 && grid_st.panel != PANEL_CHEEVOS
                                 && grid_st.panel != PANEL_ART
                                 && grid_st.panel != PANEL_ICONS
                                 && grid_st.panel != PANEL_UPDATES
                                 && grid_st.panel != PANEL_SETUP;

   /* What the mouse can point at is recorded afresh as it's drawn. */
   grid_hits_next.count = 0;
   grid_hits_next.edit  = false;

   /* Just opened over a game: everything fades in; just closed: out
    * (smoothstep both ways). */
   grid_alpha = 1.0f;
   if (grid_st.open_start)
   {
      float p = (float)(cpu_features_get_time_usec() - grid_st.open_start)
            / GRID_OPEN_USEC;
      if (p >= 1.0f || p < 0.0f)
         grid_st.open_start = 0;
      else
         grid_alpha = p * p * (3.0f - 2.0f * p);
   }
   if (grid_st.close_start)
   {
      float p = grid_clamp01((float)(cpu_features_get_time_usec()
            - grid_st.close_start) / grid_close_usec());
      grid_alpha = 1.0f - p * p * (3.0f - 2.0f * p);
   }

   /* A game started: only its loading screen, fading out over it. */
   if (grid_st.loading == LOADING_OUT)
   {
      if (video_st->current_video && video_st->current_video->set_viewport)
         video_st->current_video->set_viewport(
               video_st->data, width, height, true, false);
      grid_draw_loading(p_disp, userdata, width, height, 1.0f);
      grid_hits  = grid_hits_next;
      grid_alpha = 1.0f;
      return true;
   }

   /* Point saves at the current profile (once). */
   grid_profile_init();
   /* SteamGridDB key and RetroAchievements login, until both are set. */
   grid_setup_poll();

   /* A console control's bind wait just ended: show what was pressed. */
   grid_cc_bind_poll();

   if (     !grid_st.scanned
         || !string_is_equal(grid_st.scanned_dir,
               settings->paths.directory_menu_content))
      grid_scan();

   /* RetroAchievements / SteamGridDB lookups finishing in the background.
    * (Both polled every frame; | not || so neither is skipped.) */
   if (grid_meta_poll() | grid_cover_poll())
      for (i = 0; i < grid_st.count; i++)
         grid_apply_meta(&grid_st.games[i]);
   grid_cover_hero_poll();
   grid_cover_icons_poll();

   /* Blur downloaded banners ahead of time, on the worker thread. */
   grid_blur_poll();

   /* Installs: looked for at start (and again now and then), so the
    * download button can show a dot. Nothing installs unless picked; when
    * something has, reload core info so its games show (or play). */
   grid_updates_check(false, grid_running_core());
   if (grid_updates_poll())
   {
      command_event(CMD_EVENT_CORE_INFO_INIT, NULL);
      grid_scan();
   }

   /* Achievement badges / art previews arrived: load the new ones. */
   if (grid_cheevos_poll())
      for (i = 0; i < GRID_MAX_CHEEVOS; i++)
         if (!grid_st.badge_tex[i])
            grid_st.badge_tried[i] = false;
   if (grid_cover_choices_poll() && grid_st.art_tried)
      for (i = 0; i < grid_st.art_count; i++)
         if (!grid_st.art_tex[i])
            grid_st.art_tried[i] = false;

   /* New cover art or wallpaper chosen and downloaded: back to the grid,
    * showing it. (A new cover reloads through the cover poll above; a
    * new wallpaper gets blurred and shown by the background code.) */
   if (     grid_st.panel == PANEL_ART
         && grid_cover_choose_state() == GRID_CHOICE_APPLY_DONE)
   {
      if (grid_st.menu_game < grid_st.count)
      {
         grid_game_t *game = &grid_st.games[grid_st.menu_game];
         game->bg_ready    = false;
         grid_apply_meta(game);
      }
      grid_free_art();
      grid_st.panel = PANEL_NONE;
   }

   /* A missing core or its system files finished installing: its games
    * can show (or play) now. */
   if (grid_cores_poll())
      grid_scan();

   grid_build_view();

   if (video_st->current_video && video_st->current_video->set_viewport)
      video_st->current_video->set_viewport(
            video_st->data, width, height, true, false);

   /* Layout (reference pixels): portrait covers (2:3) filling a 302px
    * tall area from y 62, in 1-4 rows (Zoom); 8px row / 8px column gaps,
    * 35px left margin. Columns scroll sideways; the next one peeks in at
    * the right edge. */
   if (grid_st.rows < 1 || grid_st.rows > 4)
      grid_st.rows = GRID_ROWS_DEFAULT;
   grid_top = REF(62);
   row_gap  = REF(8);
   tile_h   = (REF(302) - (grid_st.rows - 1) * row_gap) / grid_st.rows;
   tile_w   = tile_h * GRID_TILE_ASPECT;
   col_gap  = REF(8);
   left     = REF(35);
   radius   = tile_w * GRID_COVER_RADIUS;
   col_w    = tile_w + col_gap;

   /* Keep the selected column fully on screen, scrolling as little as
    * possible. */
   cols_visible = (size_t)((width - left + col_gap) / col_w);
   if (cols_visible < 1)
      cols_visible = 1;
   sel_col = grid_st.selection / grid_st.rows;
   if (sel_col < grid_st.first_col)
      grid_st.first_col = sel_col;
   else if (sel_col >= grid_st.first_col + cols_visible)
      grid_st.first_col = sel_col - cols_visible + 1;

   target = grid_st.first_col * col_w;

   /* The game menu's tile, if its drawer shows: the rest of its row moves
    * aside by as much of the drawer as is out, and the grid scrolls (if it
    * must) to fit the open drawer on screen. */
   gm_show = grid_game_menu_anim(in_settings, &gm_e, &gm_ca) && show_games;
   for (i = 0; gm_show && i < grid_st.view_count; i++)
      if (grid_st.view[i] == grid_st.menu_game)
         gm_i = i;
   if (gm_i < grid_st.view_count)
   {
      const char *labels[GAME_MENU_COUNT];
      unsigned n;
      int danger;
      float item_h, mcol_w;
      float gm_w   = grid_game_drawer_layout(height, tile_h, labels, &n,
            &danger, &item_h, &mcol_w);
      float gm_end = left + (gm_i / grid_st.rows) * col_w + tile_w
         + (grid_st.gm_open ? gm_w : 0.0f);

      gm_push = gm_e > 0.0f ? gm_w * gm_e : 0.0f;
      if (gm_end - target > width - REF(10))
         target = gm_end - (width - REF(10));
   }
   /* Just zoomed: jump straight there (sliding from the old column
    * widths would look odd). */
   if (grid_st.zoomed)
   {
      grid_st.scroll_x = target;
      grid_st.zoomed   = false;
   }
   grid_st.scroll_x += (target - grid_st.scroll_x) * 0.25f;
   if (fabsf(target - grid_st.scroll_x) < 0.5f)
      grid_st.scroll_x = target;

   /* Zoom animation: each tile glides (ease-out) from where it sat in the
    * old layout to its place in the new one, resizing on the way. */
   if (grid_st.zoom_from >= 1 && grid_st.zoom_from <= 4)
   {
      float p = (float)(cpu_features_get_time_usec() - grid_st.zoom_start)
            / GRID_ZOOM_USEC;
      if (p >= 1.0f || p < 0.0f)
         grid_st.zoom_from = 0;
      else
      {
         zoom_t     = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
         old_rows   = grid_st.zoom_from;
         old_tile_h = (REF(302) - (old_rows - 1) * row_gap) / old_rows;
         old_tile_w = old_tile_h * GRID_TILE_ASPECT;
         old_col_w  = old_tile_w + col_gap;
      }
   }

   /* Upload the backgrounds of nearby games before they're selected. */
   grid_bg_prefetch(grid_st.first_col, cols_visible);

   grid_draw_background(p_disp, userdata, width, height,
         grid_st.selection < grid_st.view_count
         ? &grid_st.games[grid_st.view[grid_st.selection]] : NULL);

   if (!show_games)
   {
      if (in_settings)
         grid_draw_settings_screen(p_disp, userdata, width, height);
      else if (grid_st.panel == PANEL_CHEEVOS)
         grid_draw_cheevos_screen(p_disp, userdata, width, height);
      else if (grid_st.panel == PANEL_ART)
         grid_draw_art_screen(p_disp, userdata, width, height);
      else if (grid_st.panel == PANEL_ICONS)
         grid_draw_picker_screen(p_disp, userdata, width, height);
      else if (grid_st.panel == PANEL_SETUP)
         grid_draw_setup_screen(p_disp, userdata, width, height);
      else
         grid_draw_downloads_screen(p_disp, userdata, width, height);
   }
   else if (grid_st.view_count == 0)
   {
      char msg[PATH_MAX_LENGTH + 64];
      if (!string_is_empty(grid_search_text()))
         snprintf(msg, sizeof(msg), "No games match \"%s\"",
               grid_search_text());
      else
         snprintf(msg, sizeof(msg), "No games found in %s",
               string_is_empty(settings->paths.directory_menu_content)
               ? "(File Browser directory not set)"
               : settings->paths.directory_menu_content);
      grid_draw_text(msg, width / 2.0f,
            grid_top + (grid_st.rows * tile_h + (grid_st.rows - 1) * row_gap) / 2.0f,
            width, height, COL_TEXT_DIM, TEXT_ALIGN_CENTER, CAP(8));
   }

   new_tile_w = tile_w;
   new_tile_h = tile_h;

   for (k = 0; show_games && k <= grid_st.view_count; k++)
   {
      /* The game menu's tile is drawn last, over its drawer. */
      i = k < grid_st.view_count ? k : gm_i;
      if (i >= grid_st.view_count || (k < grid_st.view_count && i == gm_i))
         continue;
      {
         grid_game_t *game = &grid_st.games[grid_st.view[i]];
         size_t col        = i / grid_st.rows;
         size_t row        = i % grid_st.rows;
         float x           = left + col * col_w - grid_st.scroll_x
            + (gm_i < grid_st.view_count && col > gm_i / grid_st.rows
               && row == gm_i % grid_st.rows ? gm_push : 0.0f);
         float y           = grid_top + row * (new_tile_h + row_gap);
         bool selected     = i == grid_st.selection && !grid_st.bar_focus
                             && (grid_st.panel == PANEL_NONE
                              || grid_st.panel == PANEL_GAME_MENU);

         /* Mid-zoom: blend from the tile's spot in the old layout. */
         tile_w = new_tile_w;
         tile_h = new_tile_h;
         if (old_rows)
         {
            float ox = left + (i / old_rows) * old_col_w
                  - grid_st.zoom_from_scroll;
            float oy = grid_top + (i % old_rows) * (old_tile_h + row_gap);
            x      = ox + (x - ox) * zoom_t;
            y      = oy + (y - oy) * zoom_t;
            tile_w = old_tile_w + (new_tile_w - old_tile_w) * zoom_t;
            tile_h = old_tile_h + (new_tile_h - old_tile_h) * zoom_t;
         }
         radius = tile_w * GRID_COVER_RADIUS;

         /* Where the selected tile is: the game menu opens beside it. */
         if (i == grid_st.selection)
         {
            grid_st.sel_x = x;
            grid_st.sel_y = y;
            grid_st.sel_w = tile_w;
            grid_st.sel_h = tile_h;
         }

         /* Columns off screen. */
         if (x + tile_w < 0.0f || x > width)
            continue;

         if (grid_st.panel == PANEL_NONE)
            grid_hit_add(x, y, tile_w, tile_h, HIT_TILE, (unsigned)i,
                  MENU_ACTION_OK);

         if (!game->art_tried)
         {
            game->art_tried = true;
            /* Covers are cropped to the tile and share its corner radius;
             * icons keep their shape with slightly rounder corners. */
            if (!string_is_empty(game->art_path))
               grid_load_rounded_texture(game->art_path, &game->art,
                     &game->art_w, &game->art_h,
                     game->art_cover ? GRID_COVER_RADIUS : 0.08f,
                     game->art_cover ? GRID_TILE_ASPECT : 0.0f);
         }

         if (i == gm_i)
            grid_draw_game_drawer(p_disp, userdata, width, height,
                  x, y, tile_w, tile_h, radius, selected, gm_e, gm_ca);
         else if (selected)
            grid_draw_cursor(p_disp, userdata, width, height,
                  x, y, tile_w, tile_h, radius);

         grid_draw_rounded(p_disp, userdata, width, height,
               x, y, tile_w, tile_h, radius, COL_TILE);

         /* Covers fill the tile; other games show icon + title. Games whose
          * emulator is still downloading are dimmed. */
         if (game->art_cover && game->art && game->art_w && game->art_h)
            grid_draw_image(p_disp, userdata, width, height,
                  x, y, tile_w, tile_h, &game->art, game->art_w, game->art_h,
                  game->pending ? 0x5a5a5aff : 0xffffffff);
         else
            grid_draw_plain_tile(p_disp, userdata, width, height, game,
                  x, y, tile_w, tile_h);

         /* Emulator downloading: a loading bar along the bottom of the tile,
          * filled to the download progress (or sliding while unknown). */
         if (game->pending)
         {
            float bh       = floorf(REF(3) + 0.5f);
            float bpad     = floorf(tile_w * 0.1f);
            float bx       = x + bpad;
            float bw       = tile_w - 2.0f * bpad;
            float by       = y + tile_h - bpad - bh;
            int   progress = grid_cores_progress(game->path, game->core_path);

            grid_draw_rounded(p_disp, userdata, width, height,
                  bx, by, bw, bh, bh / 2.0f, 0x3a3a3cff);

            if (progress >= 0)
            {
               if (progress > 0)
                  grid_draw_rounded(p_disp, userdata, width, height,
                        bx, by, bw * (progress > 100 ? 100 : progress) / 100.0f,
                        bh, bh / 2.0f, COL_CURSOR);
            }
            else
            {
               /* Unknown progress: a segment sliding back and forth. */
               float t   = cpu_features_get_time_usec() / 1000000.0f;
               float seg = bw * 0.3f;
               float pos = (0.5f + 0.5f * sinf(t * 2.5f)) * (bw - seg);
               grid_draw_rounded(p_disp, userdata, width, height,
                     bx + pos, by, seg, bh, bh / 2.0f, COL_CURSOR);
            }
         }

         /* Running game: lime badge in the bottom-right corner. */
         if (grid_is_running(game))
         {
            float d  = tile_w * 0.13f;
            float b  = height * 0.003f;
            float bx = x + tile_w - d - tile_w * 0.06f;
            float by = y + tile_h - d - tile_w * 0.06f;
            grid_draw_rounded(p_disp, userdata, width, height,
                  bx - b, by - b, d + 2.0f * b, d + 2.0f * b, d / 2.0f + b,
                  0xffffffff);
            grid_draw_rounded(p_disp, userdata, width, height,
                  bx, by, d, d, d / 2.0f, COL_LIME);
         }
      }
   }

   grid_flush_text(width, height);

   /* (The profiles menu is the strip drawn in the top bar; the game menu
    * is drawn with its tile.) */
   grid_draw_top_bar(p_disp, userdata, width, height);
   grid_flush_text(width, height);

   grid_draw_bottom_bar(p_disp, userdata, width, height);

   /* Version, faint in the bottom left corner, on every screen: light text
    * over a dark copy one pixel down-right, so it reads on any background. */
   {
      static const char ver[] = "Bomba GUI v" BOMBA_VERSION;
      float vs = CAP(5);
      float vx = REF(6);
      float vy = grid_text_y(REF(415), height, vs);
      grid_draw_text(ver, vx + 1.0f, vy + 1.0f, width, height,
            0x0000001c, TEXT_ALIGN_LEFT, vs);
      grid_draw_text(ver, vx, vy, width, height,
            (COL_TEXT & 0xffffff00) | 0x1c, TEXT_ALIGN_LEFT, vs);
   }
   grid_flush_text(width, height);

   /* RetroArch's message box: shown for the frame it was set for. */
   if (!string_is_empty(grid_st.box_msg))
   {
      grid_draw_rect(p_disp, userdata, width, height,
            0, 0, width, height, 0x00000090);
      grid_draw_message_box(p_disp, userdata, width, height,
            grid_st.box_msg, height / 2.0f);
      grid_st.box_msg[0] = '\0';
      grid_flush_text(width, height);
   }

   /* Mouse and keyboard mode: a setting's text entry or a profile's name
    * asks in the middle of the screen (the grid's other fields are typed
    * into in place). Gamepad mode: the on-screen keyboard, with its own
    * box for the text, sliding in and out. */
   if (menu_input_dialog_get_display_kb() && grid_st.kbm)
   {
      if (grid_st.kb == KB_MENU || grid_st.kb == KB_PROFILE_NAME)
      {
         const char *buf = menu_input_dialog_get_buffer();
         grid_draw_rect(p_disp, userdata, width, height,
               0, 0, width, height, 0x00000090);
         grid_draw_text_prompt(p_disp, userdata, width, height,
               menu_state_get_ptr()->input_dialog_kb_label,
               buf ? buf : "", height * 0.5f);
      }
   }
   {
      bool osk_on = menu_input_dialog_get_display_kb() && !grid_st.kbm;
      float shown = grid_osk_anim(osk_on);
      if (shown > 0.0f)
         grid_draw_keyboard(p_disp, userdata, width, height, shown, osk_on);
   }

   grid_flush_text(width, height);

   /* A game about to start: its loading screen fades in over all this;
    * once drawn whole, the game starts (grid_home_iterate). */
   if (grid_st.loading == LOADING_IN || grid_st.loading == LOADING_START)
   {
      float p = (float)(cpu_features_get_time_usec() - grid_st.loading_start)
            / GRID_LOADING_USEC;
      if (p >= 1.0f || p < 0.0f)
      {
         p               = 1.0f;
         grid_st.loading = LOADING_START;
      }
      grid_draw_loading(p_disp, userdata, width, height, p);
   }

   grid_hits = grid_hits_next;
   grid_alpha = 1.0f;
   return true;
}

void grid_home_closing_frame(void *video_info)
{
   if (!grid_st.close_start)
      return;
   if (     !grid_content_running()
         || cpu_features_get_time_usec() - grid_st.close_start >= grid_close_usec())
   {
      grid_st.close_start = 0;
      grid_st.loading     = LOADING_NONE;
      return;
   }
   grid_home_frame((video_frame_info_t*)video_info);
}

/* ---------------------------------------------------------------------- */
/* Context                                                                */
/* ---------------------------------------------------------------------- */

void grid_home_context_reset(bool is_threaded)
{
   char app_dir[PATH_MAX_LENGTH];

   grid_home_context_destroy();

   /* Fonts load lazily per size (grid_font); just record where they are.
    * Baloo 2 (rounded) ships next to the exe (see build.sh); fall back to
    * XMB's. */
   grid_st.font_threaded = is_threaded;
   fill_pathname_application_dir(app_dir, sizeof(app_dir));

   fill_pathname_join_special(grid_st.font_path[0], app_dir,
         "bomba/fonts/Baloo2-Medium.ttf", sizeof(grid_st.font_path[0]));
   if (!path_is_valid(grid_st.font_path[0]))
      fill_pathname_application_special(grid_st.font_path[0],
            sizeof(grid_st.font_path[0]),
            APPLICATION_SPECIAL_DIRECTORY_ASSETS_XMB_FONT);

   /* Bold variant; without it bold text uses the regular font. */
   fill_pathname_join_special(grid_st.font_path[1], app_dir,
         "bomba/fonts/Baloo2-Bold.ttf", sizeof(grid_st.font_path[1]));
   if (!path_is_valid(grid_st.font_path[1]))
      grid_st.font_path[1][0] = '\0';

   grid_create_disc();

   /* Top-left bar icons, shipped next to the exe (see build.sh). */
   {
      char icons[PATH_MAX_LENGTH], path[PATH_MAX_LENGTH];

      fill_pathname_join_special(icons, app_dir, "bomba/icons",
            sizeof(icons));
      fill_pathname_join_special(path, icons, "power.png", sizeof(path));
      grid_icon_load(&grid_st.icon_exit, path);
      fill_pathname_join_special(path, icons, "settings.png", sizeof(path));
      grid_icon_load(&grid_st.icon_settings, path);
      fill_pathname_join_special(path, icons, "search.png", sizeof(path));
      grid_icon_load(&grid_st.icon_search, path);
      fill_pathname_join_special(path, icons, "trophy.png", sizeof(path));
      grid_icon_load(&grid_st.icon_trophy, path);
      fill_pathname_join_special(path, icons, "download.png", sizeof(path));
      grid_icon_load(&grid_st.icon_download, path);
      fill_pathname_join_special(path, icons, "backspace.png", sizeof(path));
      grid_icon_load(&grid_st.icon_backspace, path);
      fill_pathname_join_special(path, icons, "shift.png", sizeof(path));
      grid_icon_load(&grid_st.icon_shift, path);
   }
}

void grid_home_context_destroy(void)
{
   grid_free_textures();
   grid_bg_free_all();
   grid_bg_tex_free_all();
   grid_free_profile_textures();
   grid_free_rings();
   grid_free_badges();
   grid_free_art(); /* rebuilt by the art screen when it next draws */
   /* Picker textures die with the context: reopen it to reload them. */
   if (grid_st.panel == PANEL_ICONS)
   {
      grid_st.panel     = PANEL_PROFILES;
      grid_st.panel_sel = grid_st.pick_target + GRID_STRIP_FIRST;
   }
   grid_close_picker();

   grid_icon_free(&grid_st.icon_exit);
   grid_icon_free(&grid_st.icon_settings);
   grid_icon_free(&grid_st.icon_search);
   grid_icon_free(&grid_st.icon_trophy);
   grid_icon_free(&grid_st.icon_download);
   grid_icon_free(&grid_st.icon_backspace);
   grid_icon_free(&grid_st.icon_shift);

   if (grid_st.disc)
      video_driver_texture_unload(&grid_st.disc);
   grid_st.disc = 0;

   grid_free_fonts();
   grid_st.font_screen_h = 0;
}
