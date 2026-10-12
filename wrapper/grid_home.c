/* RetroArchWrapper: Switch-style home screen.
 *
 * Replaces XMB's main menu and Quick Menu with one screen: a grid of the
 * games in the File Browser start directory (settings:
 * rgui_browser_directory) above a white bottom bar. While a game runs the
 * bar shows its icon and title plus Settings / Continue / Quit, and the
 * grid can switch to another game.
 *
 * Art and titles come from RetroAchievements (grid_meta.c), falling back to
 * libretro boxart in the thumbnails directory and the file name.
 *
 * Controls: D-pad moves (down from the last row reaches the bar), A
 * activates, B resumes the running game, Start opens Settings (XMB).
 * Back at XMB's top level returns here.
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
#include "../menu/menu_driver.h"
#include "../gfx/gfx_display.h"
#include "../gfx/video_driver.h"
#include "../gfx/font_driver.h"

#define GRID_MAX_GAMES 1024

/* Colours (RGBA) - gradient background, white bar, Joy-Con accents */
#define COL_BG          0x0e0f16ff /* base under the moving glows */
#define COL_TILE        0xffffffff
#define COL_TEXT        0x2d2d2dff /* on the white bar */
#define COL_TEXT_DIM    0x7a7a7aff
#define COL_TEXT_BG     0xecececff /* on the carbon background */
#define COL_TEXT_BG_DIM 0x9a9a9aff
#define COL_TEXT_LIGHT  0xffffffff
#define COL_BAR         0xffffffff
#define COL_DIVIDER     0xd2d2d2ff
#define COL_CURSOR      0x00c3e3ff /* selection outline */
#define COL_BTN_WHITE   0xffffffff /* Settings */
#define COL_BTN_BORDER  0xc4c4c4ff /* outline so white reads as a button */
#define COL_LIME        0x7ed321ff /* Continue */
#define COL_RED         0xff3c28ff /* Quit */
#define COL_ICON_BG     0xebebebff /* bar icon placeholder */

typedef struct
{
   char path[PATH_MAX_LENGTH];
   char core_path[PATH_MAX_LENGTH];
   char title[NAME_MAX_LENGTH];
   char system[16];
   char boxart_path[PATH_MAX_LENGTH]; /* libretro-thumbnails fallback */
   char art_path[PATH_MAX_LENGTH];    /* what is shown */
   uintptr_t art;
   unsigned art_w;
   unsigned art_h;
   bool art_tried;
   bool art_is_cover;                 /* portrait cover: fill the tile */
} grid_game_t;

enum grid_button
{
   BTN_SETTINGS = 0,
   BTN_CONTINUE,
   BTN_QUIT,
   BTN_COUNT
};

static const char *button_labels[BTN_COUNT] = {
   "Settings", "Continue", "Quit"
};

static const uint32_t button_colors[BTN_COUNT] = {
   COL_BTN_WHITE, COL_LIME, COL_RED
};

static const uint32_t button_text_colors[BTN_COUNT] = {
   COL_TEXT, COL_TEXT_LIGHT, COL_TEXT_LIGHT
};

enum grid_focus
{
   FOCUS_GRID = 0,
   FOCUS_BAR
};

static struct
{
   grid_game_t *games;
   size_t count;
   size_t selection;
   size_t scroll_row;
   unsigned cols;
   enum grid_focus focus;
   unsigned button;     /* selected bar button */
   bool xmb;            /* Settings chosen: show XMB until Back/close */
   bool scanned;
   font_data_t *font;
   float font_height;   /* video height the font was sized for */
   uintptr_t disc;      /* white anti-aliased circle, for rounded shapes */
   uintptr_t glow;      /* soft white blob, for the moving gradient */
   uintptr_t game_bg;   /* blurred snapshot of the paused game */
   unsigned game_bg_w;
   unsigned game_bg_h;
   bool game_bg_tried;
   char scanned_dir[PATH_MAX_LENGTH];

   /* Running game, for the bar */
   uintptr_t icon;
   char title[NAME_MAX_LENGTH];
   char content[PATH_MAX_LENGTH];
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

/* Title and RetroAchievements icon for a ROM, falling back to the file
 * name. Returns the icon path in icon (empty if none) and the lookup
 * state (PENDING while RetroAchievements hasn't answered yet). */
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

   game->art_is_cover = !string_is_empty(cover);
   art = game->art_is_cover        ? cover
       : !string_is_empty(icon)    ? icon
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
   if (!grid_st.games)
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
      const char *base         = path_basename(path);
      const char *ext          = path_get_extension(path);
      grid_game_t *game        = &grid_st.games[grid_st.count];
      size_t j;

      core_info_list_get_supported_cores(cores, path, &infos, &num_infos);
      if (num_infos == 0)
         continue;

      memset(game, 0, sizeof(*game));
      strlcpy(game->path, path, sizeof(game->path));
      /* TODO: let the user pick when several cores fit. */
      strlcpy(game->core_path, infos[0].path, sizeof(game->core_path));

      for (j = 0; ext[j] && j < sizeof(game->system) - 1; j++)
         game->system[j] = toupper((unsigned char)ext[j]);
      game->system[j] = '\0';

      /* infos points into a list that the next lookup re-sorts,
       * so resolve the boxart now while it is valid. */
      grid_find_art(game, &infos[0], base);
      grid_apply_meta(game);

      grid_st.count++;
   }

   string_list_free(files);

   if (grid_st.selection >= grid_st.count)
      grid_st.selection = grid_st.count ? grid_st.count - 1 : 0;
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

/* Title and icon of the running game, for the bar (cached per content). */
static void grid_unload_running(void)
{
   if (grid_st.icon)
      video_driver_texture_unload(&grid_st.icon);
   grid_st.icon       = 0;
   grid_st.content[0] = '\0';
}

static void grid_update_running(void)
{
   char icon[PATH_MAX_LENGTH];
   const char *content = path_get(RARCH_PATH_CONTENT);

   if (string_is_empty(content)
         || string_is_equal(content, grid_st.content))
      return;

   grid_unload_running();
   strlcpy(grid_st.content, content, sizeof(grid_st.content));

   grid_resolve_title(content, grid_st.title, sizeof(grid_st.title),
         icon, sizeof(icon));

   if (!string_is_empty(icon))
   {
      unsigned w, h;
      gfx_display_reset_icon_texture(icon, &grid_st.icon,
            TEXTURE_FILTER_MIPMAP_LINEAR, &w, &h);
   }
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
   {
      grid_st.xmb = false;

      /* In-game: start on Continue, like the Switch's resume prompt. */
      if (grid_content_running())
      {
         grid_st.focus  = FOCUS_BAR;
         grid_st.button = BTN_CONTINUE;
      }
   }
   else
      grid_unload_running();
}

static unsigned grid_button_count(void)
{
   /* Without a running game only Settings applies. */
   return grid_content_running() ? BTN_COUNT : 1;
}

static void grid_activate_button(unsigned button)
{
   switch (button)
   {
      case BTN_SETTINGS:
         grid_st.xmb = true;
         break;
      case BTN_CONTINUE:
         command_event(CMD_EVENT_RESUME, NULL);
         break;
      case BTN_QUIT:
         grid_st.focus = FOCUS_GRID;
         action_ok_close_content(NULL, NULL, MENU_SETTING_ACTION_CLOSE, 0, 0);
         break;
   }
}

static void grid_bar_action(enum menu_action action)
{
   unsigned count = grid_button_count();

   if (grid_st.button >= count)
      grid_st.button = 0;

   switch (action)
   {
      case MENU_ACTION_LEFT:
         if (grid_st.button > 0)
            grid_st.button--;
         break;
      case MENU_ACTION_RIGHT:
         if (grid_st.button + 1 < count)
            grid_st.button++;
         break;
      case MENU_ACTION_UP:
         if (grid_st.count)
            grid_st.focus = FOCUS_GRID;
         break;
      case MENU_ACTION_OK:
         grid_activate_button(grid_st.button);
         break;
      default:
         break;
   }
}

static void grid_grid_action(enum menu_action action)
{
   size_t sel  = grid_st.selection;
   size_t cols = grid_st.cols ? grid_st.cols : 1;

   switch (action)
   {
      case MENU_ACTION_LEFT:
         if (sel > 0)
            sel--;
         break;
      case MENU_ACTION_RIGHT:
         if (sel + 1 < grid_st.count)
            sel++;
         break;
      case MENU_ACTION_UP:
         if (sel >= cols)
            sel -= cols;
         break;
      case MENU_ACTION_DOWN:
         if (sel + cols < grid_st.count)
            sel += cols;
         else if (sel / cols < (grid_st.count - 1) / cols)
            sel = grid_st.count - 1;   /* partial last row */
         else
         {
            grid_st.focus  = FOCUS_BAR;
            grid_st.button = grid_content_running() ? BTN_CONTINUE : BTN_SETTINGS;
         }
         break;
      case MENU_ACTION_SCROLL_UP:
         sel = 0;
         break;
      case MENU_ACTION_SCROLL_DOWN:
         if (grid_st.count)
            sel = grid_st.count - 1;
         break;
      case MENU_ACTION_OK:
         if (sel < grid_st.count)
         {
            if (grid_is_running(&grid_st.games[sel]))
               command_event(CMD_EVENT_RESUME, NULL);
            else
               grid_launch(&grid_st.games[sel]);
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
      case MENU_ACTION_SCROLL_UP:
      case MENU_ACTION_SCROLL_DOWN:
      case MENU_ACTION_OK:
         if (grid_st.focus == FOCUS_BAR || !grid_st.count)
         {
            /* Empty grid: down/up have nowhere to go. */
            grid_st.focus = FOCUS_BAR;
            grid_bar_action(action);
         }
         else
            grid_grid_action(action);
         break;
      case MENU_ACTION_START:
      case MENU_ACTION_SELECT:
         grid_activate_button(BTN_SETTINGS);
         break;
      case MENU_ACTION_CANCEL:
         if (grid_content_running())
            command_event(CMD_EVENT_RESUME, NULL);
         break;
      default:
         /* Leave menu toggle, pointer etc. to XMB. */
         return false;
   }

   return true;
}

/* ---------------------------------------------------------------------- */
/* Drawing                                                                */
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

   video_driver_texture_load(&ti, TEXTURE_FILTER_LINEAR, &grid_st.disc);
   free(ti.pixels);
}

/* Soft white blob: alpha fades smoothly from the centre to the edge.
 * Drawn large and tinted, overlapping blobs read as a blurred gradient. */
static void grid_create_glow(void)
{
   struct texture_image ti;
   const unsigned size = 128;
   const float r       = size / 2.0f;
   unsigned x, y;

   ti.width         = size;
   ti.height        = size;
   ti.supports_rgba = video_driver_supports_rgba();
   ti.pixels        = (uint32_t*)malloc(size * size * sizeof(uint32_t));
   if (!ti.pixels)
      return;

   for (y = 0; y < size; y++)
   {
      for (x = 0; x < size; x++)
      {
         float dx = x + 0.5f - r;
         float dy = y + 0.5f - r;
         float a  = 1.0f - sqrtf(dx * dx + dy * dy) / r;
         if (a < 0.0f) a = 0.0f;
         a = a * a * (3.0f - 2.0f * a);           /* smoothstep */
         ti.pixels[y * size + x] = ((uint32_t)(a * 255.0f) << 24) | 0x00ffffff;
      }
   }

   video_driver_texture_load(&ti, TEXTURE_FILTER_LINEAR, &grid_st.glow);
   free(ti.pixels);
}

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
         uint32_t r = (uint32_t)(img[i * 3 + 0] * 0.55f);
         uint32_t g = (uint32_t)(img[i * 3 + 1] * 0.55f);
         uint32_t b = (uint32_t)(img[i * 3 + 2] * 0.55f);

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

/* Background: the blurred paused game if there is one, otherwise soft
 * coloured glows drifting over a dark base. */
static void grid_draw_background(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, bool game_running)
{
   /* Joy-Con-ish colours; alpha keeps overlaps soft. */
   static const struct
   {
      uint32_t color;
      float sx, sy, phase;
   } glows[] = {
      { 0x0ab9e68c, 0.11f, 0.07f, 0.0f }, /* blue   */
      { 0xff3c2870, 0.08f, 0.10f, 2.1f }, /* red    */
      { 0x7b3cff80, 0.06f, 0.09f, 4.2f }, /* purple */
      { 0x00c89670, 0.09f, 0.05f, 1.3f }, /* teal   */
      { 0xe6a0ff50, 0.05f, 0.08f, 3.4f }  /* pink   */
   };
   float color[16];
   unsigned i;
   float t;

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

   grid_set_color(color, COL_BG);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         0, 0, width, height, width, height, color, NULL);

   if (!grid_st.glow)
      return;

   t = cpu_features_get_time_usec() / 1000000.0f;

   for (i = 0; i < sizeof(glows) / sizeof(glows[0]); i++)
   {
      float size = height * 1.3f;
      float cx   = width  * (0.5f + 0.38f * sinf(t * glows[i].sx * 6.2832f + glows[i].phase));
      float cy   = height * (0.5f + 0.38f * cosf(t * glows[i].sy * 6.2832f + glows[i].phase * 1.7f));

      grid_set_color(color, glows[i].color);
      gfx_display_draw_quad(p_disp, userdata, width, height,
            (int)(cx - size / 2.0f), (int)(cy - size / 2.0f),
            (unsigned)size, (unsigned)size, width, height, color,
            &grid_st.glow);
   }
}

/* Opaque rounded rectangle: three strips plus four corner circles. */
static void grid_draw_rounded(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h, float r, uint32_t rgba)
{
   float color[16];

   if (r * 2.0f > w) r = w / 2.0f;
   if (r * 2.0f > h) r = h / 2.0f;

   grid_set_color(color, rgba);

   if (!grid_st.disc || r < 1.0f)
   {
      gfx_display_draw_quad(p_disp, userdata, width, height,
            (int)x, (int)y, (unsigned)w, (unsigned)h,
            width, height, color, NULL);
      return;
   }

   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)(x + r), (int)y, (unsigned)(w - 2.0f * r), (unsigned)h,
         width, height, color, NULL);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)x, (int)(y + r), (unsigned)r, (unsigned)(h - 2.0f * r),
         width, height, color, NULL);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)(x + w - r), (int)(y + r), (unsigned)r, (unsigned)(h - 2.0f * r),
         width, height, color, NULL);

   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)x, (int)y, (unsigned)(2.0f * r), (unsigned)(2.0f * r),
         width, height, color, &grid_st.disc);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)(x + w - 2.0f * r), (int)y, (unsigned)(2.0f * r), (unsigned)(2.0f * r),
         width, height, color, &grid_st.disc);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)x, (int)(y + h - 2.0f * r), (unsigned)(2.0f * r), (unsigned)(2.0f * r),
         width, height, color, &grid_st.disc);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)(x + w - 2.0f * r), (int)(y + h - 2.0f * r),
         (unsigned)(2.0f * r), (unsigned)(2.0f * r),
         width, height, color, &grid_st.disc);
}

/* Switch-style cursor: pulsing cyan outline with a white gap, drawn
 * behind an item of the given rect and corner radius. */
static void grid_draw_cursor(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height,
      float x, float y, float w, float h, float r)
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
         x - gap, y - gap, w + 2.0f * gap, h + 2.0f * gap, r + gap, 0xffffffff);
}

static void grid_draw_text(const char *text, float x, float y,
      unsigned width, unsigned height, uint32_t color,
      enum text_alignment align, float scale)
{
   if (!grid_st.font)
      return;

   gfx_display_draw_text(grid_st.font, text, x, y, width, height,
         color, align, scale * (height / grid_st.font_height),
         false, 0.0f, false);
}

static void grid_flush_text(unsigned width, unsigned height)
{
   if (grid_st.font && grid_st.font->renderer && grid_st.font->renderer->flush)
      grid_st.font->renderer->flush(width, height,
            grid_st.font->renderer_data);
}

/* Copies src into dst, cutting it short with "..." to fit max_w pixels. */
static void grid_fit_text(char *dst, size_t dst_len, const char *src,
      float max_w, unsigned height, float scale)
{
   size_t len;
   float s = scale * (height / grid_st.font_height);

   strlcpy(dst, src, dst_len);

   if (!grid_st.font)
      return;

   len = strlen(dst);
   if (font_driver_get_message_width(grid_st.font, dst, len, s) <= max_w)
      return;

   while (len > 0)
   {
      /* Step back over whole UTF-8 characters. */
      do { len--; } while (len > 0 && ((unsigned char)dst[len] & 0xc0) == 0x80);

      dst[len] = '\0';
      strlcat(dst, "...", dst_len);

      if (font_driver_get_message_width(grid_st.font, dst,
               strlen(dst), s) <= max_w)
         return;

      dst[len] = '\0';
   }
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
   float s, text_w, overflow, scroll_time, t, offset, line_h;
   retro_time_t now;

   if (!grid_st.font)
      return;

   s      = scale * (height / grid_st.font_height);
   text_w = font_driver_get_message_width(grid_st.font, text,
         strlen(text), s);

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
   line_h = (grid_st.font_height / 32.0f) * s;

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

/* White bottom bar: icon and title (in-game) or hints (home) on the
 * left, pill buttons on the right. */
static void grid_draw_bar(gfx_display_t *p_disp, void *userdata,
      unsigned width, unsigned height, float bar_y, float bar_h)
{
   float color[16];
   unsigned i;
   bool running    = grid_content_running();
   unsigned count  = grid_button_count();
   float pad       = bar_h * 0.18f;
   float side      = height * 0.045f;
   float icon_size = bar_h - 2.0f * pad;
   float btn_w     = height * 0.16f;
   float btn_h     = bar_h * 0.52f;
   float btn_y     = bar_y + (bar_h - btn_h) / 2.0f;
   float btn_gap   = height * 0.018f;
   float btn_x     = width - side - count * btn_w - (count - 1) * btn_gap;
   float text_dy   = height * 0.012f; /* rough baseline centring */

   grid_set_color(color, COL_BAR);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         0, (int)bar_y, width, (unsigned)bar_h, width, height, color, NULL);

   grid_set_color(color, COL_DIVIDER);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)side, (int)bar_y, (unsigned)(width - 2.0f * side),
         (unsigned)(height * 0.0015f + 1.0f), width, height, color, NULL);

   if (running)
   {
      float text_x = side + icon_size + pad;

      grid_update_running();

      grid_draw_rounded(p_disp, userdata, width, height,
            side, bar_y + pad, icon_size, icon_size,
            icon_size * 0.08f, COL_ICON_BG);

      if (grid_st.icon)
      {
         grid_set_color(color, 0xffffffff);
         gfx_display_draw_quad(p_disp, userdata, width, height,
               (int)side, (int)(bar_y + pad),
               (unsigned)icon_size, (unsigned)icon_size,
               width, height, color, &grid_st.icon);
      }

      grid_draw_marquee(p_disp, userdata, grid_st.title,
            text_x, bar_y + bar_h / 2.0f + text_dy,
            btn_x - text_x - 2.0f * pad, width, height,
            COL_TEXT, 0.9f, TEXT_ALIGN_LEFT, &grid_st.content);
   }
   else
      grid_draw_text("A  Start        Start  Settings", side,
            bar_y + bar_h / 2.0f + text_dy * 0.8f, width, height,
            COL_TEXT_DIM, TEXT_ALIGN_LEFT, 0.7f);

   for (i = 0; i < count; i++)
   {
      float x  = btn_x + i * (btn_w + btn_gap);
      float r  = 0.0f; /* square corners */

      if (grid_st.focus == FOCUS_BAR && i == grid_st.button)
         grid_draw_cursor(p_disp, userdata, width, height,
               x, btn_y, btn_w, btn_h, r);

      if (button_colors[i] == COL_BTN_WHITE)
      {
         /* White on the white bar: outline it. */
         float b = height * 0.002f + 1.0f;
         grid_draw_rounded(p_disp, userdata, width, height,
               x, btn_y, btn_w, btn_h, r, COL_BTN_BORDER);
         grid_draw_rounded(p_disp, userdata, width, height,
               x + b, btn_y + b, btn_w - 2.0f * b, btn_h - 2.0f * b,
               r, COL_BTN_WHITE);
      }
      else
         grid_draw_rounded(p_disp, userdata, width, height,
               x, btn_y, btn_w, btn_h, r, button_colors[i]);

      grid_draw_text(button_labels[i], x + btn_w / 2.0f,
            btn_y + btn_h / 2.0f + text_dy * 0.75f, width, height,
            button_text_colors[i], TEXT_ALIGN_CENTER, 0.72f);
   }
}

bool grid_home_frame(video_frame_info_t *video_info)
{
   float color[16];
   size_t i;
   size_t rows_visible;
   unsigned cols;
   float tile, art_h, tile_h, gap, label_h, top, left, radius, bar_h, bar_y;
   char clock[16];
   time_t now_t;
   struct tm *tm_now;
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

   if (video_st->current_video && video_st->current_video->set_viewport)
      video_st->current_video->set_viewport(
            video_st->data, width, height, true, false);

   /* Layout scales with window height. */
   bar_h   = height * 0.11f;
   bar_y   = height - bar_h;
   gap     = height * 0.03f;
   tile    = height * 0.18f;         /* tile width: 194px at 1080p */
   art_h   = tile * 1.5f;            /* portrait 2:3, like SteamGridDB covers */
   radius  = tile * 0.05f;
   label_h = height * 0.045f;
   tile_h  = art_h + label_h;
   top     = height * 0.15f;

   cols = (unsigned)((width - gap) / (tile + gap));
   if (cols < 1)
      cols = 1;
   grid_st.cols = cols;
   left = (width - (cols * tile + (cols - 1) * gap)) / 2.0f;

   rows_visible = (size_t)((bar_y - top) / (tile_h + gap));
   if (rows_visible < 1)
      rows_visible = 1;

   /* Keep the selected row on screen. */
   {
      size_t row = grid_st.selection / cols;
      if (row < grid_st.scroll_row)
         grid_st.scroll_row = row;
      else if (row >= grid_st.scroll_row + rows_visible)
         grid_st.scroll_row = row - rows_visible + 1;
   }

   /* Background (opaque, also over a paused game) */
   grid_draw_background(p_disp, userdata, width, height,
         video_info->libretro_running);

   /* Header: title left, clock right */
   grid_draw_text("Games", left, height * 0.09f, width, height,
         COL_TEXT_BG, TEXT_ALIGN_LEFT, 1.4f);

   now_t  = time(NULL);
   tm_now = localtime(&now_t);
   if (tm_now && strftime(clock, sizeof(clock), "%H:%M", tm_now))
      grid_draw_text(clock, width - left, height * 0.09f, width, height,
            COL_TEXT_BG, TEXT_ALIGN_RIGHT, 1.0f);

   if (grid_st.count == 0)
   {
      char msg[PATH_MAX_LENGTH + 64];
      snprintf(msg, sizeof(msg), "No games found in %s",
            string_is_empty(settings->paths.directory_menu_content)
            ? "(File Browser directory not set)"
            : settings->paths.directory_menu_content);
      grid_draw_text(msg, width / 2.0f, height / 2.0f, width, height,
            COL_TEXT_BG_DIM, TEXT_ALIGN_CENTER, 1.0f);
   }

   for (i = grid_st.scroll_row * cols;
        i < grid_st.count && i < (grid_st.scroll_row + rows_visible) * cols;
        i++)
   {
      grid_game_t *game = &grid_st.games[i];
      size_t col        = i % cols;
      size_t row        = i / cols - grid_st.scroll_row;
      float x           = left + col * (tile + gap);
      float y           = top + row * (tile_h + gap);
      bool selected     = grid_st.focus == FOCUS_GRID
                       && i == grid_st.selection;

      if (!game->art_tried)
      {
         game->art_tried = true;
         if (!string_is_empty(game->art_path))
            gfx_display_reset_icon_texture(game->art_path, &game->art,
                  TEXTURE_FILTER_MIPMAP_LINEAR, &game->art_w, &game->art_h);
      }

      if (selected)
         grid_draw_cursor(p_disp, userdata, width, height,
               x, y, tile, art_h, radius);

      grid_draw_rounded(p_disp, userdata, width, height,
            x, y, tile, art_h, radius, COL_TILE);

      /* Covers fill the tile; square icons sit centred in it. */
      if (game->art && game->art_w && game->art_h)
         grid_draw_image(p_disp, userdata, width, height,
               x, y, tile, art_h, &game->art, game->art_w, game->art_h);
      else
         grid_draw_text(game->system, x + tile / 2.0f,
               y + art_h / 2.0f + height * 0.012f, width, height,
               COL_TEXT_DIM, TEXT_ALIGN_CENTER, 1.0f);

      /* Running game: lime badge in the corner. */
      if (grid_is_running(game))
      {
         float d = tile * 0.16f;
         float b = height * 0.003f;
         grid_draw_rounded(p_disp, userdata, width, height,
               x + tile - d * 0.75f - b, y - d * 0.25f - b,
               d + 2.0f * b, d + 2.0f * b, d / 2.0f + b, 0xffffffff);
         grid_draw_rounded(p_disp, userdata, width, height,
               x + tile - d * 0.75f, y - d * 0.25f, d, d, d / 2.0f, COL_LIME);
      }

      if (selected)
         /* Selected: scroll names that don't fit. */
         grid_draw_marquee(p_disp, userdata, game->title,
               x - gap * 0.25f, y + art_h + label_h * 0.65f,
               tile + gap * 0.5f, width, height,
               COL_TEXT_BG, 0.6f, TEXT_ALIGN_CENTER, game);
      else
      {
         char label[NAME_MAX_LENGTH];
         grid_fit_text(label, sizeof(label), game->title,
               tile + gap * 0.5f, height, 0.6f);
         grid_draw_text(label, x + tile / 2.0f,
               y + art_h + label_h * 0.65f, width, height,
               COL_TEXT_BG_DIM, TEXT_ALIGN_CENTER, 0.6f);
      }
   }

   grid_draw_bar(p_disp, userdata, width, height, bar_y, bar_h);

   grid_flush_text(width, height);

   return true;
}

/* ---------------------------------------------------------------------- */
/* Context                                                                */
/* ---------------------------------------------------------------------- */

void grid_home_context_reset(bool is_threaded)
{
   char fontpath[PATH_MAX_LENGTH];
   unsigned width  = 0;
   unsigned height = 0;

   grid_home_context_destroy();

   video_driver_get_size(&width, &height);
   if (height == 0)
      height = 720;

   /* Roboto ships next to the exe (see build.sh); fall back to XMB's. */
   {
      char app_dir[PATH_MAX_LENGTH];
      fill_pathname_application_dir(app_dir, sizeof(app_dir));
      fill_pathname_join_special(fontpath, app_dir,
            "wrapper/fonts/Roboto-Medium.ttf", sizeof(fontpath));
   }
   if (!path_is_valid(fontpath))
      fill_pathname_application_special(fontpath, sizeof(fontpath),
            APPLICATION_SPECIAL_DIRECTORY_ASSETS_XMB_FONT);
   grid_st.font        = gfx_display_font_file(disp_get_ptr(),
         fontpath, height / 32.0f, is_threaded);
   grid_st.font_height = (float)height;

   grid_create_disc();
   grid_create_glow();
}

void grid_home_context_destroy(void)
{
   grid_free_textures();
   grid_unload_running();

   if (grid_st.disc)
      video_driver_texture_unload(&grid_st.disc);
   grid_st.disc = 0;

   if (grid_st.glow)
      video_driver_texture_unload(&grid_st.glow);
   grid_st.glow = 0;

   grid_free_game_bg();

   if (grid_st.font)
      font_driver_free(grid_st.font);
   grid_st.font = NULL;
}
