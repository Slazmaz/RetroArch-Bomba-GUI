/* RetroArchWrapper: console-style home screen (dark theme).
 *
 * Replaces XMB's main menu and Quick Menu with one screen:
 *
 *   [ Search for games...      ]                         16:24
 *   [L]  ALL GAMES 5        GBA        SNES        NES      [R]
 *   +-----+ +-----+ +-----+ +-----+ +-----+
 *   |cover| |cover| |cover| |cover| |cover|   portrait covers
 *   +-----+ +-----+ +-----+ +-----+ +-----+
 *   Game title        (+) Settings (Y) Quit (X) Search (A) Play (B) Back
 *
 * Games are the files in the File Browser start directory (settings:
 * rgui_browser_directory) that an installed core supports. Titles and
 * icons come from RetroAchievements (grid_meta.c), covers from SteamGridDB
 * (grid_cover.c), falling back to libretro boxart and the file name.
 *
 * Controls: D-pad moves, L/R switch tabs, A plays/continues, B resumes
 * the running game (or clears the search), X searches, Y quits the
 * running game, Start opens Settings (XMB). Back at XMB's top level
 * returns here.
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

#include <features/features_cpu.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <formats/image.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <string/stdstring.h>

#include "grid_home.h"
#include "grid_meta.h"
#include "grid_cover.h"

#include "../configuration.h"
#include "../core_info.h"
#include "../file_path_special.h"
#include "../runloop.h"
#include "../content.h"
#include "../tasks/task_content.h"
#include "../command.h"
#include "../paths.h"
#include "../input/input_driver.h"
#include "../menu/menu_driver.h"
#include "../menu/menu_input.h"
#include "../gfx/gfx_display.h"
#include "../gfx/video_driver.h"
#include "../gfx/font_driver.h"

#define GRID_MAX_GAMES 1024
#define GRID_MAX_TABS  24
#define GRID_COLS      5

/* The layout is measured from a 752x423 reference screenshot. REF(px)
 * converts a reference pixel length to this screen; CAP(px) gives the
 * font scale whose capital letters are px reference pixels tall
 * (scale 1.0 = height/32 em). */
#define GRID_FONT_CAP  0.70f /* Montserrat cap height, in em */
#define REF(px)        ((float)(px) * (float)height / 423.0f)
#define CAP(px)        ((float)(px) * 32.0f / (423.0f * GRID_FONT_CAP))
#define GRID_FONT_CACHE 16
#define GRID_COVER_RADIUS 0.04f /* corner radius / tile width (reference) */

/* Colours (RGBA), dark theme */
#define COL_BG          0x0e0e0eff
#define COL_BAR         0x181818ff /* top and bottom bars */
#define COL_DIVIDER     0x252526ff
#define COL_SEARCH      0x252526ff
#define COL_TILE        0x1f1f20ff /* tile placeholder */
#define COL_TEXT        0xecececff
#define COL_TEXT_DIM    0x8c8c90ff
#define COL_TEXT_DARK   0x181818ff /* on light pills and glyphs */
#define COL_TAB_PILL    0xecececff
#define COL_GLYPH       0xe6e6e6ff /* L/R and button glyphs: light, dark letter */
#define COL_CURSOR      0x00c3e3ff /* selection outline */
#define COL_LIME        0x7ed321ff /* running-game badge */

typedef struct
{
   char path[PATH_MAX_LENGTH];
   char core_path[PATH_MAX_LENGTH];
   char title[NAME_MAX_LENGTH];
   char system[16];                   /* tab label, e.g. "SNES" */
   char boxart_path[PATH_MAX_LENGTH]; /* libretro-thumbnails fallback */
   char art_path[PATH_MAX_LENGTH];    /* what is shown */
   uintptr_t art;
   unsigned art_w;
   unsigned art_h;
   bool art_tried;
} grid_game_t;

enum grid_tab_type
{
   TAB_ALL = 0,
   TAB_SYSTEM
};

typedef struct
{
   enum grid_tab_type type;
   char label[32];
} grid_tab_t;

static struct
{
   grid_game_t *games;
   size_t count;

   /* Current tab + search, as a list of indices into games */
   grid_tab_t tabs[GRID_MAX_TABS];
   unsigned tab_count;
   char tab_label[32];      /* current tab, kept across rebuilds */
   size_t *view;
   size_t view_count;
   size_t selection;        /* index into view */
   float scroll_y;          /* animated grid scroll, pixels */
   char search[128];        /* applied search text */

   bool xmb;                /* Settings chosen: show XMB until Back/close */
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
   char font_path[2][PATH_MAX_LENGTH]; /* [0] Medium, [1] Bold */
   bool font_threaded;
   uintptr_t disc;          /* white anti-aliased circle, for rounded shapes */
   uintptr_t shoulder[2];   /* L / R glyph shapes at their exact size */
   unsigned shoulder_w;
   unsigned shoulder_h;
   uintptr_t game_bg;       /* blurred snapshot of the paused game */
   unsigned game_bg_w;
   unsigned game_bg_h;
   bool game_bg_tried;
} grid_st;

/* From menu_cbs_ok.c; also handles "quit on close" and the menu stack. */
int action_ok_close_content(const char *path, const char *label,
      unsigned type, size_t idx, size_t entry_idx);

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */

static void grid_set_color(float *dst, uint32_t rgba)
{
   int i;
   float r = ((rgba >> 24) & 0xff) / 255.0f;
   float g = ((rgba >> 16) & 0xff) / 255.0f;
   float b = ((rgba >>  8) & 0xff) / 255.0f;
   float a = ( rgba        & 0xff) / 255.0f;

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

/* Tab label for a file extension. */
static void grid_system_label(const char *ext, char *s, size_t len)
{
   static const struct { const char *ext, *label; } map[] = {
      { "gba", "GBA" },     { "gb",  "GAME BOY" }, { "gbc", "GBC" },
      { "sfc", "SNES" },    { "smc", "SNES" },     { "nes", "NES" },
      { "fds", "NES" },     { "n64", "N64" },      { "z64", "N64" },
      { "v64", "N64" },     { "md",  "GENESIS" },  { "gen", "GENESIS" },
      { "smd", "GENESIS" }, { "sms", "MASTER SYSTEM" },
      { "gg",  "GAME GEAR" },{ "pce", "PC ENGINE" }, { "nds", "DS" }
   };
   size_t i;

   for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
      if (string_is_equal_noncase(ext, map[i].ext))
      {
         strlcpy(s, map[i].label, len);
         return;
      }

   for (i = 0; ext[i] && i + 1 < len; i++)
      s[i] = toupper((unsigned char)ext[i]);
   s[i] = '\0';
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
      grid_cover_get(game->title, cover, sizeof(cover));

   art = !string_is_empty(cover) ? cover
       : !string_is_empty(icon)  ? icon
       : game->boxart_path;

   if (!string_is_equal(art, game->art_path))
   {
      if (game->art)
         video_driver_texture_unload(&game->art);
      game->art       = 0;
      game->art_tried = false;
      strlcpy(game->art_path, art, sizeof(game->art_path));
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
         continue;

      memset(game, 0, sizeof(*game));
      strlcpy(game->path, path, sizeof(game->path));
      /* TODO: let the user pick when several cores fit. */
      strlcpy(game->core_path, infos[0].path, sizeof(game->core_path));
      grid_system_label(path_get_extension(path),
            game->system, sizeof(game->system));

      /* infos points into a list that the next lookup re-sorts,
       * so resolve the boxart now while it is valid. */
      grid_find_art(game, &infos[0], path_basename(path));
      grid_apply_meta(game);

      grid_st.count++;
   }

   string_list_free(files);
}

static bool grid_is_running(const grid_game_t *game)
{
   const char *content = path_get(RARCH_PATH_CONTENT);
   return grid_content_running()
      && !string_is_empty(content)
      && string_is_equal(content, game->path);
}

static void grid_launch(const grid_game_t *game)
{
   content_ctx_info_t content_info;

   content_info.argc        = 0;
   content_info.argv        = NULL;
   content_info.args        = NULL;
   content_info.environ_get = NULL;

   task_push_load_content_with_new_core_from_menu(
         game->core_path, game->path, &content_info,
         CORE_TYPE_PLAIN, NULL, NULL);
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

/* Rebuilds the tab list: ALL GAMES, then one per system present. */
static void grid_build_tabs(void)
{
   size_t i;

   grid_st.tab_count = 0;
   grid_add_tab(TAB_ALL, "ALL GAMES");

   for (i = 0; i < grid_st.count; i++)
      grid_add_tab(TAB_SYSTEM, grid_st.games[i].system);
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
   if (menu_input_dialog_get_display_kb())
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

/* Rebuilds the list of games shown for the current tab and search. */
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

   if (grid_st.selection >= grid_st.view_count)
      grid_st.selection = grid_st.view_count ? grid_st.view_count - 1 : 0;
}

static void grid_switch_tab(int dir)
{
   int idx = (int)grid_current_tab_index() + dir;

   if (idx < 0 || idx >= (int)grid_st.tab_count)
      return;

   strlcpy(grid_st.tab_label, grid_st.tabs[idx].label,
         sizeof(grid_st.tab_label));
   grid_st.selection = 0;
   grid_st.scroll_y  = 0.0f;
}

/* ---------------------------------------------------------------------- */
/* Input                                                                  */
/* ---------------------------------------------------------------------- */

static void grid_free_game_bg(void)
{
   if (grid_st.game_bg)
      video_driver_texture_unload(&grid_st.game_bg);
   grid_st.game_bg       = 0;
   grid_st.game_bg_tried = false;
}

void grid_home_toggle(bool menu_on)
{
   /* Fresh snapshot of the game each time the menu opens. */
   grid_free_game_bg();

   if (menu_on)
      grid_st.xmb = false;
}

static void grid_search_done(void *userdata, const char *line)
{
   strlcpy(grid_st.search, line ? line : "", sizeof(grid_st.search));
   grid_st.selection = 0;
   grid_st.scroll_y  = 0.0f;
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

   menu_input_dialog_start(&line);
}

static void grid_grid_action(enum menu_action action)
{
   size_t sel  = grid_st.selection;
   size_t cols = GRID_COLS;
   size_t n    = grid_st.view_count;

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
      case MENU_ACTION_UP:
         if (sel >= cols)
            sel -= cols;
         break;
      case MENU_ACTION_DOWN:
         if (sel + cols < n)
            sel += cols;
         else if (sel / cols < (n - 1) / cols)
            sel = n - 1;   /* partial last row */
         break;
      case MENU_ACTION_OK:
         if (sel < n)
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

bool grid_home_entry_action(enum menu_action action, unsigned xmb_depth)
{
   if (grid_st.xmb)
   {
      /* Back at XMB's top level returns here. */
      if (action == MENU_ACTION_CANCEL && xmb_depth <= 1)
      {
         grid_st.xmb = false;
         grid_scan();
         return true;
      }
      return false;
   }

   switch (action)
   {
      case MENU_ACTION_LEFT:
      case MENU_ACTION_RIGHT:
      case MENU_ACTION_UP:
      case MENU_ACTION_DOWN:
      case MENU_ACTION_OK:
         if (grid_st.view_count)
            grid_grid_action(action);
         break;
      case MENU_ACTION_SCROLL_UP:   /* L */
         grid_switch_tab(-1);
         break;
      case MENU_ACTION_SCROLL_DOWN: /* R */
         grid_switch_tab(1);
         break;
      case MENU_ACTION_SEARCH:      /* X */
         grid_start_search();
         break;
      case MENU_ACTION_START:       /* + : Settings (XMB) */
         grid_st.xmb = true;
         break;
      case MENU_ACTION_SCAN:        /* Y : quit the running game */
         if (grid_content_running())
            action_ok_close_content(NULL, NULL,
                  MENU_SETTING_ACTION_CLOSE, 0, 0);
         break;
      case MENU_ACTION_CANCEL:
         if (!string_is_empty(grid_st.search))
         {
            grid_st.search[0] = '\0';
            grid_st.selection = 0;
            grid_st.scroll_y  = 0.0f;
         }
         else if (grid_content_running())
            command_event(CMD_EVENT_RESUME, NULL);
         break;
      case MENU_ACTION_SELECT:
      case MENU_ACTION_INFO:
         break;
      default:
         /* Leave menu toggle, pointer etc. to XMB. */
         return false;
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

/* Loads an image with its corners rounded in the alpha channel (radius as
 * a fraction of the image width), so covers look rounded over any
 * background. */
static bool grid_load_rounded_texture(const char *path, uintptr_t *tex,
      unsigned *w, unsigned *h, float radius_frac)
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

/* Selection cursor: pulsing cyan outline with a gap, drawn behind an
 * item of the given rect and corner radius. */
static void grid_draw_cursor(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h, float r, uint32_t gap_color)
{
   float gap   = height * 0.003f;
   float line  = height * 0.004f;
   float t     = cpu_features_get_time_usec() / 1000000.0f;
   float pulse = 0.5f + 0.5f * sinf(t * 4.0f);
   uint32_t c  = grid_mix(COL_CURSOR, 0xffffffff, 0.35f * pulse);
   float o     = gap + line;

   grid_draw_rounded(p_disp, userdata, width, height,
         x - o, y - o, w + 2.0f * o, h + 2.0f * o, r + o, c);
   grid_draw_rounded(p_disp, userdata, width, height,
         x - gap, y - gap, w + 2.0f * gap, h + 2.0f * gap, r + gap, gap_color);
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

static float grid_text_width_bold(const char *text, unsigned height, float scale)
{
   return grid_text_width_font(true, text, height, scale);
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

/* Draws a single character centred on (cx, cy) using the glyph's actual
 * bounding box, so letters sit exactly in the middle of a button glyph. */
static void grid_draw_char_centered(char ch, float cx, float cy,
      unsigned width, unsigned height, uint32_t color, float scale)
{
   char str[2];
   const struct font_glyph *g = NULL;
   font_data_t *font          = grid_font(true, scale, height);

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
      grid_draw_text_bold(str, pen, baseline, width, height, color,
            TEXT_ALIGN_LEFT, scale);
   }
   else
      grid_draw_text_bold(str, cx, grid_text_y(cy, height, scale),
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

/* L / R shoulder-button glyphs, rasterised at their exact on-screen size:
 * a rounded rectangle with one large curved top corner (top-left for L,
 * top-right for R), anti-aliased with a signed distance. */
static void grid_create_shoulders(unsigned w, unsigned h)
{
   unsigned side, x, y;

   for (side = 0; side < 2; side++)
   {
      struct texture_image ti;
      /* Reference: top outer corner curved over ~8 of 17px, the other
       * three corners nearly square. */
      float r_big   = h * 0.47f;
      float r_small = h * 0.06f;
      float hw      = w / 2.0f;
      float hh      = h / 2.0f;

      if (grid_st.shoulder[side])
         video_driver_texture_unload(&grid_st.shoulder[side]);
      grid_st.shoulder[side] = 0;

      ti.width         = w;
      ti.height        = h;
      ti.supports_rgba = video_driver_supports_rgba();
      ti.pixels        = (uint32_t*)malloc(w * h * sizeof(uint32_t));
      if (!ti.pixels)
         continue;

      for (y = 0; y < h; y++)
      {
         for (x = 0; x < w; x++)
         {
            float px   = x + 0.5f - hw;
            float py   = y + 0.5f - hh;
            bool top   = py < 0.0f;
            bool left  = px < 0.0f;
            /* Big corner: top-left on L (side 0), top-right on R. */
            float r    = (top && (side == 0 ? left : !left)) ? r_big : r_small;
            float qx   = fabsf(px) - (hw - r);
            float qy   = fabsf(py) - (hh - r);
            float ox   = qx > 0.0f ? qx : 0.0f;
            float oy   = qy > 0.0f ? qy : 0.0f;
            float in   = (qx > qy ? qx : qy);
            float dist = sqrtf(ox * ox + oy * oy) + (in < 0.0f ? in : 0.0f) - r;
            float a    = 0.5f - dist;
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            ti.pixels[y * w + x] = ((uint32_t)(a * 255.0f) << 24) | 0x00ffffff;
         }
      }

      video_driver_texture_load(&ti, TEXTURE_FILTER_NEAREST,
            &grid_st.shoulder[side]);
      free(ti.pixels);
   }

   grid_st.shoulder_w = w;
   grid_st.shoulder_h = h;
}

static void grid_draw_shoulder(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, unsigned side, char letter,
      float x, float y, unsigned w, unsigned h)
{
   float color[16];

   if (grid_st.shoulder_w != w || grid_st.shoulder_h != h)
      grid_create_shoulders(w, h);

   grid_set_color(color, COL_GLYPH);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)x, (int)y, w, h, width, height, color,
         grid_st.shoulder[side] ? &grid_st.shoulder[side] : NULL);

   /* Reference: letter 8px tall in the 17px glyph, shifted ~3px of 23
    * away from the curved corner. */
   grid_draw_char_centered(letter,
         x + w / 2.0f + (side == 0 ? 1.0f : -1.0f) * w * 0.13f,
         y + h / 2.0f,
         width, height, COL_TEXT_DARK, CAP(8));
}

/* Draws text inside [x, x + w]. If it doesn't fit, it scrolls: pause,
 * scroll to the end, pause, jump back. key identifies what is being shown
 * so the scroll restarts when it changes. align applies when it fits. */
static void grid_draw_marquee(gfx_display_t *p_disp, void *userdata,
      const char *text, float x, float y, float w,
      unsigned width, unsigned height, uint32_t color, float scale,
      enum text_alignment align, const void *key)
{
   static const void *last_key  = NULL;
   static retro_time_t start    = 0;
   const float pause            = 1.0f;           /* seconds */
   const float speed            = height * 0.06f; /* pixels per second */
   float text_w, overflow, scroll_time, t, offset, line_h;
   retro_time_t now;

   if (!grid_font(false, scale, height) || w <= 0.0f)
      return;

   text_w = grid_text_width(text, height, scale);

   if (text_w <= w)
   {
      grid_draw_text(text, align == TEXT_ALIGN_CENTER ? x + w / 2.0f : x,
            y, width, height, color, align, scale);
      return;
   }

   now = cpu_features_get_time_usec();
   if (key != last_key)
   {
      last_key = key;
      start    = now;
   }

   overflow    = text_w - w;
   scroll_time = overflow / speed;
   t           = fmodf((now - start) / 1000000.0f,
         pause + scroll_time + pause);

   if (t < pause)
      offset = 0.0f;
   else if (t < pause + scroll_time)
      offset = (t - pause) * speed;
   else
      offset = overflow;

   /* Clip to the text box. Earlier text is flushed first so the
    * clip only affects this string. */
   line_h = (height / 32.0f) * scale;

   grid_flush_text(width, height);

   if (p_disp->dispctx && p_disp->dispctx->scissor_begin)
      gfx_display_scissor_begin(p_disp, userdata, width, height,
            (int)x, (int)(y - line_h * 1.1f),
            (unsigned)w, (unsigned)(line_h * 1.6f));

   grid_draw_text(text, x - offset, y, width, height, color,
         TEXT_ALIGN_LEFT, scale);
   grid_flush_text(width, height);

   if (p_disp->dispctx && p_disp->dispctx->scissor_end)
      p_disp->dispctx->scissor_end(userdata, width, height);
}

static void grid_draw_image(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h,
      uintptr_t *tex, unsigned tex_w, unsigned tex_h)
{
   float color[16];
   float s = w / tex_w;
   float iw, ih;

   /* Fit inside the box, keeping aspect ratio. */
   if (tex_h * s > h)
      s = h / tex_h;
   iw = tex_w * s;
   ih = tex_h * s;

   grid_set_color(color, 0xffffffff);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)(x + (w - iw) / 2.0f), (int)(y + (h - ih) / 2.0f),
         (unsigned)iw, (unsigned)ih, width, height, color, tex);
}

/* Round button glyph (e.g. "A") with its label to the right. Returns the
 * total width drawn. Draws nothing when measure_only. */
static float grid_draw_hint(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, float x, float cy,
      const char *glyph, const char *label, bool measure_only)
{
   /* Reference: circle 13px, label 5px after it, label caps 5.5px.
    * Odd pixel sizes so the circle has a centre pixel row/column and the
    * symbol's strokes can sit exactly on it. */
   float scale  = CAP(5.5);
   float d      = 2.0f * floorf(REF(13) / 2.0f) + 1.0f;
   float gap    = REF(5);
   float lw     = grid_text_width(label, height, scale);

   if (!measure_only)
   {
      float gx = floorf(x + 0.5f);
      float gy = floorf(cy - d / 2.0f + 0.5f);

      grid_draw_rounded(p_disp, userdata, width, height,
            gx, gy, d, d, d / 2.0f, COL_GLYPH);

      if (glyph[0] == '+')
      {
         /* Drawn as two bars: the font's '+' sits on the math axis, not
          * centred. Reference: strokes as thin as the letters' (1px of
          * 13), arms ~7px of 13. Odd sizes keep it on the centre pixel. */
         float t   = 2.0f * floorf(d * 0.045f) + 1.0f;
         float len = 2.0f * floorf(d * 0.27f) + 1.0f;
         grid_draw_rect(p_disp, userdata, width, height,
               gx + (d - len) / 2.0f, gy + (d - t) / 2.0f, len, t,
               COL_TEXT_DARK);
         grid_draw_rect(p_disp, userdata, width, height,
               gx + (d - t) / 2.0f, gy + (d - len) / 2.0f, t, len,
               COL_TEXT_DARK);
      }
      else
         /* Letter cap height ~54% of the circle, as in the reference. */
         grid_draw_char_centered(glyph[0], gx + d / 2.0f, gy + d / 2.0f,
               width, height, COL_TEXT_DARK,
               d * 0.54f / (height / 32.0f) / GRID_FONT_CAP);

      grid_draw_text(label, gx + d + gap, grid_text_y(cy, height, scale),
            width, height, COL_TEXT, TEXT_ALIGN_LEFT, scale);
   }

   return d + gap + lw;
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

/* Snapshot of the paused game's last frame: shrunk, blurred and dimmed,
 * uploaded as a small texture that is stretched over the screen. Only
 * works for software-rendered cores (hardware cores keep no CPU copy). */
static void grid_capture_game_bg(void)
{
   struct texture_image ti;
   video_driver_state_t *video_st = video_state_get_ptr();
   const uint8_t *src             = (const uint8_t*)video_st->frame_cache_data;
   unsigned sw                    = video_st->frame_cache_width;
   unsigned sh                    = video_st->frame_cache_height;
   size_t pitch                   = video_st->frame_cache_pitch;
   enum retro_pixel_format fmt    = video_st->pix_fmt;
   const unsigned dw              = 96;
   unsigned dh, x, y, i;
   float *img, *tmp;

   grid_st.game_bg_tried = true;

   if (!src || src == RETRO_HW_FRAME_BUFFER_VALID || !sw || !sh || !pitch)
      return;

   dh = (unsigned)(dw * (float)sh / sw + 0.5f);
   if (dh < 1)
      dh = 1;

   img = (float*)calloc(dw * dh * 3, sizeof(float));
   tmp = (float*)calloc(dw * dh * 3, sizeof(float));
   ti.pixels = (uint32_t*)malloc(dw * dh * sizeof(uint32_t));

   if (img && tmp && ti.pixels)
   {
      /* Shrink: average each block of source pixels. */
      for (y = 0; y < dh; y++)
      {
         unsigned y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
         for (x = 0; x < dw; x++)
         {
            unsigned x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            unsigned sx, sy, n = 0;
            float acc[3] = { 0.0f, 0.0f, 0.0f };

            if (y1 <= y0) y1 = y0 + 1;
            if (x1 <= x0) x1 = x0 + 1;

            for (sy = y0; sy < y1 && sy < sh; sy++)
               for (sx = x0; sx < x1 && sx < sw; sx++)
               {
                  float rgb[3];
                  grid_read_pixel(src + sy * pitch, sx, fmt, rgb);
                  acc[0] += rgb[0];
                  acc[1] += rgb[1];
                  acc[2] += rgb[2];
                  n++;
               }

            for (i = 0; i < 3; i++)
               img[(y * dw + x) * 3 + i] = n ? acc[i] / n : 0.0f;
         }
      }

      /* Three box passes approximate a Gaussian blur. */
      for (i = 0; i < 3; i++)
         grid_box_blur(img, tmp, dw, dh, 3);

      /* Dim so the grid and text stay readable on top. */
      for (i = 0; i < dw * dh; i++)
      {
         uint32_t r = (uint32_t)(img[i * 3 + 0] * 0.45f);
         uint32_t g = (uint32_t)(img[i * 3 + 1] * 0.45f);
         uint32_t b = (uint32_t)(img[i * 3 + 2] * 0.45f);

         /* RGBA-capable drivers take R in the low byte, others ARGB. */
         ti.pixels[i] = video_driver_supports_rgba()
            ? (0xff000000 | (b << 16) | (g << 8) | r)
            : (0xff000000 | (r << 16) | (g << 8) | b);
      }

      ti.width         = dw;
      ti.height        = dh;
      ti.supports_rgba = video_driver_supports_rgba();

      video_driver_texture_load(&ti, TEXTURE_FILTER_LINEAR, &grid_st.game_bg);
      grid_st.game_bg_w = dw;
      grid_st.game_bg_h = dh;
   }

   free(img);
   free(tmp);
   free(ti.pixels);
}

/* Solid dark background, or the blurred paused game while one runs. */
static void grid_draw_background(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, bool game_running)
{
   float color[16];

   if (game_running && !grid_st.game_bg && !grid_st.game_bg_tried)
      grid_capture_game_bg();

   if (game_running && grid_st.game_bg)
   {
      /* Cover the screen, keeping the game's aspect ratio. */
      float s  = (float)width / grid_st.game_bg_w;
      float bw, bh;
      if (grid_st.game_bg_h * s < height)
         s = (float)height / grid_st.game_bg_h;
      bw = grid_st.game_bg_w * s;
      bh = grid_st.game_bg_h * s;

      grid_set_color(color, 0xffffffff);
      gfx_display_draw_quad(p_disp, userdata, width, height,
            (int)((width - bw) / 2.0f), (int)((height - bh) / 2.0f),
            (unsigned)bw, (unsigned)bh, width, height, color,
            &grid_st.game_bg);
      return;
   }

   grid_draw_rect(p_disp, userdata, width, height,
         0, 0, width, height, COL_BG);
}

/* ---------------------------------------------------------------------- */
/* Screen parts                                                           */
/* ---------------------------------------------------------------------- */

/* Top bar: search box, clock, then L [tabs] R. */
static void grid_draw_top_bar(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, float bar_h)
{
   char clock[16];
   time_t now_t;
   struct tm *tm_now;
   unsigned i;
   const char *search = grid_search_text();
   bool typing        = menu_input_dialog_get_display_kb();
   unsigned cur       = grid_current_tab_index();
   /* All positions/sizes in reference pixels (see REF/CAP). */
   float side         = REF(20);           /* L glyph, clock, R glyph */
   float row1         = REF(25.5);         /* search row centre */
   float glyph        = floorf(REF(17) + 0.5f);
   float glyph_w      = floorf(REF(23) + 0.5f);
   float tabs_x0      = side + glyph_w + REF(14); /* first pill edge */
   float tabs_x1      = width - side - glyph_w - REF(14);
   float tab_scale    = CAP(6);

   grid_draw_rect(p_disp, userdata, width, height,
         0, 0, width, bar_h, COL_BAR);

   /* Soft drop shadow under the bar, over the grid. */
   grid_draw_vgradient(p_disp, userdata, width, height,
         0, floorf(bar_h), width, floorf(REF(12)), 0x000000b0, 0x00000000);

   /* Search box: 256x31 at (13, 10); magnifier 14px at x 26; text at x 49 */
   {
      float x    = floorf(REF(13));
      float y    = floorf(REF(10));
      float pw   = floorf(REF(256));
      float ph   = floorf(REF(31));
      float icon = floorf(REF(14));
      float ix   = floorf(REF(26));
      float iy   = floorf(row1 - icon / 2.0f + 0.5f);
      float ring = floorf(icon * 0.8f + 0.5f);
      float th   = floorf(REF(2) + 0.5f); /* stroke */
      float tx   = REF(49);
      float ts   = CAP(7);
      char text[160];
      unsigned k;

      grid_draw_rounded(p_disp, userdata, width, height,
            x, y, pw, ph, ph / 2.0f, COL_SEARCH);

      /* Magnifier: ring in the top-left of the icon box, then a short
       * diagonal handle to the bottom-right made of dots (quads can't be
       * rotated). */
      grid_draw_rounded(p_disp, userdata, width, height,
            ix, iy, ring, ring, ring / 2.0f, COL_TEXT);
      grid_draw_rounded(p_disp, userdata, width, height,
            ix + th, iy + th, ring - 2.0f * th, ring - 2.0f * th,
            (ring - 2.0f * th) / 2.0f, COL_SEARCH);
      for (k = 0; k < 4; k++)
      {
         float o = ring * 0.78f + k * (icon - ring * 0.78f - th) / 3.0f;
         grid_draw_rounded(p_disp, userdata, width, height,
               ix + o - th * 0.1f, iy + o - th * 0.1f, th * 1.2f, th * 1.2f,
               th * 0.6f, COL_TEXT);
      }

      if (typing)
      {
         /* Blinking caret while the keyboard is open. */
         bool caret = fmodf(cpu_features_get_time_usec() / 1000000.0f, 1.0f) < 0.5f;
         snprintf(text, sizeof(text), "%s%s", search, caret ? "|" : "");
         grid_draw_text(text, tx, grid_text_y(row1, height, ts),
               width, height, COL_TEXT, TEXT_ALIGN_LEFT, ts);
      }
      else if (!string_is_empty(search))
         grid_draw_text(search, tx, grid_text_y(row1, height, ts),
               width, height, COL_TEXT, TEXT_ALIGN_LEFT, ts);
      else
         grid_draw_text("Search for games...", tx,
               grid_text_y(row1, height, ts),
               width, height, COL_TEXT_DIM, TEXT_ALIGN_LEFT, ts);
   }

   /* Clock: digits 10px tall, centred at y 24.5 */
   now_t  = time(NULL);
   tm_now = localtime(&now_t);
   if (tm_now && strftime(clock, sizeof(clock), "%H:%M", tm_now))
      grid_draw_text(clock, width - side,
            grid_text_y(REF(24.5), height, CAP(10)),
            width, height, COL_TEXT, TEXT_ALIGN_RIGHT, CAP(10));

   /* L / R shoulder-button glyphs: 23x17 at (20, 55) */
   {
      float gy = floorf(REF(55) + 0.5f);
      grid_draw_shoulder(p_disp, userdata, width, height, 0, 'L',
            floorf(side + 0.5f), gy, (unsigned)glyph_w, (unsigned)glyph);
      grid_draw_shoulder(p_disp, userdata, width, height, 1, 'R',
            floorf(width - side - glyph_w + 0.5f), gy,
            (unsigned)glyph_w, (unsigned)glyph);
   }

   /* Tabs packed left to right as in the reference: the pill starts 14px
    * after L, text is padded 16px inside it, and consecutive tab texts
    * are 50px apart. Count follows its label 9.5px later. */
   {
      float ph     = floorf(REF(17) + 0.5f); /* pill height = glyph height */
      float pad    = REF(16);
      float space  = REF(50);
      float cgap   = REF(9.5);
      float xs[GRID_MAX_TABS];
      float ws[GRID_MAX_TABS];
      char  counts[GRID_MAX_TABS][16];
      float x      = tabs_x0 + pad;
      float shift  = 0.0f;

      for (i = 0; i < grid_st.tab_count; i++)
      {
         counts[i][0] = '\0';
         if (grid_st.tabs[i].type == TAB_ALL)
            snprintf(counts[i], sizeof(counts[i]), "%u",
                  (unsigned)grid_st.count);

         ws[i] = grid_text_width_bold(grid_st.tabs[i].label, height, tab_scale);
         if (counts[i][0])
            ws[i] += cgap + grid_text_width_bold(counts[i], height, tab_scale);
      }

      for (i = 0; i < grid_st.tab_count; i++)
      {
         xs[i] = x;
         x    += ws[i] + space;
      }

      /* Too many tabs to fit: slide so the current one is visible. */
      if (grid_st.tab_count && xs[cur] + ws[cur] + pad > tabs_x1)
         shift = xs[cur] + ws[cur] + pad - tabs_x1;

      for (i = 0; i < grid_st.tab_count; i++)
      {
         float tx = xs[i] - shift;

         if (tx - pad < tabs_x0 - 1.0f || tx + ws[i] + pad > tabs_x1 + 1.0f)
            continue;

         if (i == cur)
            grid_draw_rounded(p_disp, userdata, width, height,
                  tx - pad, floorf(REF(55) + 0.5f), ws[i] + 2.0f * pad, ph,
                  ph / 2.0f, COL_TAB_PILL);

         grid_draw_text_bold(grid_st.tabs[i].label, tx,
               grid_text_y(REF(63.5), height, tab_scale), width, height,
               i == cur ? COL_TEXT_DARK : COL_TEXT, TEXT_ALIGN_LEFT, tab_scale);

         if (counts[i][0])
            grid_draw_text_bold(counts[i],
                  tx + grid_text_width_bold(grid_st.tabs[i].label, height,
                     tab_scale) + cgap,
                  grid_text_y(REF(63.5), height, tab_scale), width, height,
                  i == cur ? 0x6a6b72ff : COL_TEXT_DIM, TEXT_ALIGN_LEFT,
                  tab_scale);
      }
   }
}

/* Bottom bar: selected game's title on the left, button hints right. */
static void grid_draw_bottom_bar(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, float bar_y, float bar_h, float side)
{
   const char *glyphs[5], *labels[5];
   unsigned i, n  = 0;
   bool running   = grid_content_running();
   bool has_sel   = grid_st.selection < grid_st.view_count;
   float cy       = REF(397);  /* hint row centre */
   float sp       = REF(20);   /* label end to next circle */
   float right    = REF(28);   /* last label to screen edge */
   float hints_w  = 0.0f, hx;

   /* Soft shadow cast upwards onto the grid, then the bar down to the
    * last pixel row so nothing peeks out underneath. */
   grid_draw_vgradient(p_disp, userdata, width, height,
         0, floorf(bar_y - REF(4)), width, floorf(REF(4)),
         0x00000000, 0x00000060);
   grid_draw_rect(p_disp, userdata, width, height,
         0, floorf(bar_y), width, height - floorf(bar_y), COL_BAR);

   /* Hints, right-aligned, only those that apply right now. */
   glyphs[n] = "+"; labels[n++] = "Settings";
   if (running)
   {
      glyphs[n] = "Y"; labels[n++] = "Quit";
   }
   glyphs[n] = "X"; labels[n++] = "Search";
   if (has_sel)
   {
      glyphs[n] = "A";
      labels[n++] = grid_is_running(&grid_st.games[grid_st.view[grid_st.selection]])
         ? "Continue" : "Play";
   }
   if (running || !string_is_empty(grid_st.search))
   {
      glyphs[n] = "B"; labels[n++] = "Back";
   }

   for (i = 0; i < n; i++)
      hints_w += (i ? sp : 0.0f) + grid_draw_hint(p_disp, userdata,
            width, height, 0, cy, glyphs[i], labels[i], true);

   hx = width - right - hints_w;
   for (i = 0; i < n; i++)
      hx += grid_draw_hint(p_disp, userdata, width, height, hx, cy,
            glyphs[i], labels[i], false) + sp;

   /* Selected game's title on the left (where the reference has its Menu
    * button), in the tab label size. */
   if (has_sel)
   {
      grid_game_t *game = &grid_st.games[grid_st.view[grid_st.selection]];
      float tw          = width - right - hints_w - REF(30) - side;

      grid_draw_marquee(p_disp, userdata, game->title,
            side, grid_text_y(cy, height, CAP(6.5)), tw, width, height,
            COL_TEXT, CAP(6.5), TEXT_ALIGN_LEFT, game);
   }
}

/* On-screen keyboard while searching (physical keyboards type directly). */
static void grid_draw_keyboard(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height)
{
   input_driver_state_t *input_st = input_state_get_ptr();

   /* Dim the screen behind the keyboard. */
   grid_draw_rect(p_disp, userdata, width, height,
         0, height * 0.5f, width, height * 0.5f, 0x000000c0);

   grid_flush_text(width, height);
   gfx_display_draw_keyboard(p_disp, userdata, width, height,
         grid_st.disc, grid_font(false, 1.0f, height), input_st->osk_grid,
         input_st->osk_ptr, 0xffffffff);
}

bool grid_home_frame(video_frame_info_t *video_info)
{
   size_t i;
   float tile, art_h, gap, side, radius, top_h, bot_h, bot_y, grid_top;
   float row_h, target;
   void *userdata                 = video_info->userdata;
   unsigned width                 = video_info->width;
   unsigned height                = video_info->height;
   gfx_display_t *p_disp          = (gfx_display_t*)video_info->disp_userdata;
   video_driver_state_t *video_st = video_state_get_ptr();
   settings_t *settings           = config_get_ptr();

   if (grid_st.xmb)
      return false;

   if (     !grid_st.scanned
         || !string_is_equal(grid_st.scanned_dir,
               settings->paths.directory_menu_content))
      grid_scan();

   /* RetroAchievements / SteamGridDB lookups finishing in the background.
    * (Both polled every frame; | not || so neither is skipped.) */
   if (grid_meta_poll() | grid_cover_poll())
      for (i = 0; i < grid_st.count; i++)
         grid_apply_meta(&grid_st.games[i]);

   grid_build_view();

   if (video_st->current_video && video_st->current_video->set_viewport)
      video_st->current_video->set_viewport(
            video_st->data, width, height, true, false);

   /* Layout from the reference: 5 portrait covers per row, 20px side
    * margins, 19px column / 22px row gaps, covers from y 97; top bar 81px
    * tall; bottom bar from y 370. */
   side     = REF(20);
   gap      = REF(19);
   top_h    = floorf(REF(81));
   bot_y    = floorf(REF(370));
   bot_h    = height - bot_y;
   grid_top = REF(97);
   tile     = (width - 2.0f * side - (GRID_COLS - 1) * gap) / GRID_COLS;
   art_h    = tile * 1.5f;
   radius   = tile * GRID_COVER_RADIUS;
   row_h    = art_h + REF(22);

   /* Selected row slides to the top of the grid area. */
   target = (float)(grid_st.selection / GRID_COLS) * row_h;
   grid_st.scroll_y += (target - grid_st.scroll_y) * 0.25f;
   if (fabsf(target - grid_st.scroll_y) < 0.5f)
      grid_st.scroll_y = target;

   grid_draw_background(p_disp, userdata, width, height,
         video_info->libretro_running);

   if (grid_st.view_count == 0)
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
      grid_draw_text(msg, width / 2.0f, (grid_top + bot_y) / 2.0f,
            width, height, COL_TEXT_DIM, TEXT_ALIGN_CENTER, 0.9f);
   }

   for (i = 0; i < grid_st.view_count; i++)
   {
      grid_game_t *game = &grid_st.games[grid_st.view[i]];
      size_t col        = i % GRID_COLS;
      size_t row        = i / GRID_COLS;
      float x           = side + col * (tile + gap);
      float y           = grid_top + row * row_h - grid_st.scroll_y;
      bool selected     = i == grid_st.selection;

      /* Rows outside the screen (the bars are drawn over the rest). */
      if (y + art_h < 0.0f || y > height)
         continue;

      if (!game->art_tried)
      {
         game->art_tried = true;
         /* Same corner radius as the tile (radius / tile width). */
         if (!string_is_empty(game->art_path))
            grid_load_rounded_texture(game->art_path, &game->art,
                  &game->art_w, &game->art_h, GRID_COVER_RADIUS);
      }

      if (selected)
         grid_draw_cursor(p_disp, userdata, width, height,
               x, y, tile, art_h, radius, COL_BG);

      grid_draw_rounded(p_disp, userdata, width, height,
            x, y, tile, art_h, radius, COL_TILE);

      /* Covers fill the tile; square icons sit centred in it. */
      if (game->art && game->art_w && game->art_h)
         grid_draw_image(p_disp, userdata, width, height,
               x, y, tile, art_h, &game->art, game->art_w, game->art_h);
      else
         grid_draw_text(game->system, x + tile / 2.0f,
               grid_text_y(y + art_h * 0.45f, height, 1.0f), width, height,
               COL_TEXT_DIM, TEXT_ALIGN_CENTER, 1.0f);

      /* Running game: lime badge in the bottom-right corner. */
      if (grid_is_running(game))
      {
         float d = tile * 0.13f;
         float b = height * 0.003f;
         float bx = x + tile - d - tile * 0.05f;
         float by = y + art_h - d - tile * 0.05f;
         grid_draw_rounded(p_disp, userdata, width, height,
               bx - b, by - b, d + 2.0f * b, d + 2.0f * b, d / 2.0f + b,
               0xffffffff);
         grid_draw_rounded(p_disp, userdata, width, height,
               bx, by, d, d, d / 2.0f, COL_LIME);
      }
   }

   grid_flush_text(width, height);

   /* Bars over the grid, so scrolled rows slide under them. */
   grid_draw_top_bar(p_disp, userdata, width, height, top_h);
   grid_draw_bottom_bar(p_disp, userdata, width, height, bot_y, bot_h, side);
   grid_flush_text(width, height);

   if (menu_input_dialog_get_display_kb())
      grid_draw_keyboard(p_disp, userdata, width, height);

   grid_flush_text(width, height);

   return true;
}

/* ---------------------------------------------------------------------- */
/* Context                                                                */
/* ---------------------------------------------------------------------- */

void grid_home_context_reset(bool is_threaded)
{
   char app_dir[PATH_MAX_LENGTH];

   grid_home_context_destroy();

   /* Fonts load lazily per size (grid_font); just record where they are.
    * Montserrat ships next to the exe (see build.sh); fall back to XMB's. */
   grid_st.font_threaded = is_threaded;
   fill_pathname_application_dir(app_dir, sizeof(app_dir));

   fill_pathname_join_special(grid_st.font_path[0], app_dir,
         "wrapper/fonts/Montserrat-Medium.ttf", sizeof(grid_st.font_path[0]));
   if (!path_is_valid(grid_st.font_path[0]))
      fill_pathname_application_special(grid_st.font_path[0],
            sizeof(grid_st.font_path[0]),
            APPLICATION_SPECIAL_DIRECTORY_ASSETS_XMB_FONT);

   /* Bold variant; without it bold text uses the regular font. */
   fill_pathname_join_special(grid_st.font_path[1], app_dir,
         "wrapper/fonts/Montserrat-Bold.ttf", sizeof(grid_st.font_path[1]));
   if (!path_is_valid(grid_st.font_path[1]))
      grid_st.font_path[1][0] = '\0';

   grid_create_disc();
}

void grid_home_context_destroy(void)
{
   unsigned i;

   grid_free_textures();
   grid_free_game_bg();

   if (grid_st.disc)
      video_driver_texture_unload(&grid_st.disc);
   grid_st.disc = 0;

   for (i = 0; i < 2; i++)
   {
      if (grid_st.shoulder[i])
         video_driver_texture_unload(&grid_st.shoulder[i]);
      grid_st.shoulder[i] = 0;
   }
   grid_st.shoulder_w = grid_st.shoulder_h = 0;

   grid_free_fonts();
   grid_st.font_screen_h = 0;
}
