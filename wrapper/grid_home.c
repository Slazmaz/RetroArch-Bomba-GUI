/* RetroArchWrapper: game grid home screen.
 *
 * Shown in place of XMB's main menu while no content is running. Lists
 * the games in the File Browser start directory (settings:
 * rgui_browser_directory) that at least one installed core supports, as a
 * grid of boxart tiles. Boxart comes from the thumbnails directory using
 * the libretro-thumbnails layout:
 *    <thumbnails>/<database>/Named_Boxarts/<rom name>.png
 *
 * Controls: D-pad moves, A launches, Start opens the regular XMB menu,
 * Back at XMB's top level returns to the grid.
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include <features/features_cpu.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <string/stdstring.h>

#include "grid_home.h"
#include "grid_meta.h"

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
} grid_game_t;

typedef struct
{
   grid_game_t *games;
   size_t count;
   size_t selection;
   size_t scroll_row;
   unsigned cols;
   bool active;         /* grid shown (vs. regular XMB) */
   bool scanned;
   font_data_t *font;
   float font_height;   /* video height the font was sized for */
   char scanned_dir[PATH_MAX_LENGTH];
} grid_home_t;

static grid_home_t grid_st = { NULL, 0, 0, 0, 1, true, false, NULL, 0.0f };

/* In-game bar: shown instead of XMB's Quick Menu while a game runs. */
enum ingame_button
{
   INGAME_SETTINGS = 0,
   INGAME_CONTINUE,
   INGAME_QUIT,
   INGAME_BUTTON_COUNT
};

static const char *ingame_labels[INGAME_BUTTON_COUNT] = {
   "Settings", "Continue", "Quit"
};

static struct
{
   bool xmb;            /* user chose Settings: show XMB until menu closes */
   unsigned selection;
   uintptr_t icon;
   char icon_path[PATH_MAX_LENGTH];
   char title[NAME_MAX_LENGTH];
   char content[PATH_MAX_LENGTH]; /* content the title/icon belong to */
} ingame_st = { false, INGAME_CONTINUE, 0 };

/* From menu_cbs_ok.c; also handles "quit on close" and the menu stack. */
int action_ok_close_content(const char *path, const char *label,
      unsigned type, size_t idx, size_t entry_idx);

/* Colours (RGBA) */
#define GRID_COL_BG        0x14161cff
#define GRID_COL_TILE      0x262a35ff
#define GRID_COL_SELECT    0x5aa0ffff
#define GRID_COL_TEXT      0xe8ebf2ff
#define GRID_COL_TEXT_DIM  0x8a90a0ff

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

/* libretro-thumbnails replaces these characters with '_' */
static void grid_sanitize_thumb_name(char *s)
{
   for (; *s; s++)
      if (strchr("&*/:`<>?\\|\"", *s))
         *s = '_';
}

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

/* Fills title/art from RetroAchievements when known, otherwise from the
 * file name and libretro boxart. */
static void grid_apply_meta(grid_game_t *game)
{
   char title[NAME_MAX_LENGTH];
   char icon[PATH_MAX_LENGTH];
   const char *art;

   title[0] = icon[0] = '\0';

   if (grid_meta_get(game->path, title, sizeof(title),
            icon, sizeof(icon)) == GRID_META_FOUND
         && !string_is_empty(title))
      strlcpy(game->title, grid_skip_ra_tags(title), sizeof(game->title));
   else
   {
      strlcpy(game->title, path_basename(game->path), sizeof(game->title));
      path_remove_extension(game->title);
      grid_clean_title(game->title);
   }

   art = !string_is_empty(icon) ? icon : game->boxart_path;

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

static bool grid_content_running(void)
{
   return (runloop_state_get_ptr()->current_core.flags
         & RETRO_CORE_FLAG_GAME_LOADED) ? true : false;
}

static void ingame_unload_icon(void)
{
   if (ingame_st.icon)
      video_driver_texture_unload(&ingame_st.icon);
   ingame_st.icon         = 0;
   ingame_st.icon_path[0] = '\0';
   ingame_st.content[0]   = '\0';
}

void grid_home_toggle(bool menu_on)
{
   if (menu_on)
   {
      ingame_st.xmb       = false;
      ingame_st.selection = INGAME_CONTINUE;
   }
   else
      ingame_unload_icon();
}

static bool ingame_entry_action(enum menu_action action, unsigned xmb_depth)
{
   if (ingame_st.xmb)
   {
      /* Back at XMB's top level returns to the bar. */
      if (action == MENU_ACTION_CANCEL && xmb_depth <= 1)
      {
         ingame_st.xmb = false;
         return true;
      }
      return false;
   }

   switch (action)
   {
      case MENU_ACTION_LEFT:
         if (ingame_st.selection > 0)
            ingame_st.selection--;
         break;
      case MENU_ACTION_RIGHT:
         if (ingame_st.selection + 1 < INGAME_BUTTON_COUNT)
            ingame_st.selection++;
         break;
      case MENU_ACTION_OK:
         switch (ingame_st.selection)
         {
            case INGAME_SETTINGS:
               ingame_st.xmb = true;
               break;
            case INGAME_CONTINUE:
               command_event(CMD_EVENT_RESUME, NULL);
               break;
            case INGAME_QUIT:
               grid_st.active = true;
               action_ok_close_content(NULL, NULL,
                     MENU_SETTING_ACTION_CLOSE, 0, 0);
               break;
         }
         break;
      case MENU_ACTION_CANCEL:
         command_event(CMD_EVENT_RESUME, NULL);
         break;
      case MENU_ACTION_UP:
      case MENU_ACTION_DOWN:
      case MENU_ACTION_START:
      case MENU_ACTION_SELECT:
      case MENU_ACTION_SCROLL_UP:
      case MENU_ACTION_SCROLL_DOWN:
         break;
      default:
         return false;
   }

   return true;
}

bool grid_home_entry_action(enum menu_action action, unsigned xmb_depth)
{
   size_t sel  = grid_st.selection;
   size_t cols = grid_st.cols ? grid_st.cols : 1;

   if (grid_content_running())
      return ingame_entry_action(action, xmb_depth);

   if (!grid_st.active)
   {
      /* Back at XMB's top level returns to the grid. */
      if (action == MENU_ACTION_CANCEL && xmb_depth <= 1)
      {
         grid_st.active = true;
         grid_scan();
         return true;
      }
      return false;
   }

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
         else if (grid_st.count)
            sel = grid_st.count - 1;
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
            grid_launch(&grid_st.games[sel]);
         break;
      case MENU_ACTION_START:
      case MENU_ACTION_SELECT:
         grid_st.active = false;
         break;
      case MENU_ACTION_CANCEL:
         break;
      default:
         /* Leave menu toggle, pointer etc. to XMB. */
         return false;
   }

   grid_st.selection = sel;
   return true;
}

void grid_home_context_reset(bool is_threaded)
{
   char fontpath[PATH_MAX_LENGTH];
   unsigned width  = 0;
   unsigned height = 0;

   grid_home_context_destroy();

   video_driver_get_size(&width, &height);
   if (height == 0)
      height = 720;

   fill_pathname_application_special(fontpath, sizeof(fontpath),
         APPLICATION_SPECIAL_DIRECTORY_ASSETS_XMB_FONT);
   grid_st.font        = gfx_display_font_file(disp_get_ptr(),
         fontpath, height / 32.0f, is_threaded);
   grid_st.font_height = (float)height;
}

void grid_home_context_destroy(void)
{
   grid_free_textures();
   ingame_unload_icon();

   if (grid_st.font)
      font_driver_free(grid_st.font);
   grid_st.font = NULL;
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

static void grid_flush_text(unsigned width, unsigned height)
{
   if (grid_st.font && grid_st.font->renderer && grid_st.font->renderer->flush)
      grid_st.font->renderer->flush(width, height,
            grid_st.font->renderer_data);
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

/* Resolves title and icon for the running content (cached per content). */
static void ingame_update_game(void)
{
   char title[NAME_MAX_LENGTH];
   char icon[PATH_MAX_LENGTH];
   const char *content = path_get(RARCH_PATH_CONTENT);

   if (string_is_empty(content)
         || string_is_equal(content, ingame_st.content))
      return;

   ingame_unload_icon();
   strlcpy(ingame_st.content, content, sizeof(ingame_st.content));

   title[0] = icon[0] = '\0';

   if (grid_meta_get(content, title, sizeof(title), icon, sizeof(icon))
         == GRID_META_FOUND && !string_is_empty(title))
      strlcpy(ingame_st.title, grid_skip_ra_tags(title),
            sizeof(ingame_st.title));
   else
   {
      strlcpy(ingame_st.title, path_basename(content),
            sizeof(ingame_st.title));
      path_remove_extension(ingame_st.title);
      grid_clean_title(ingame_st.title);
   }

   if (!string_is_empty(icon))
   {
      unsigned w, h;
      strlcpy(ingame_st.icon_path, icon, sizeof(ingame_st.icon_path));
      gfx_display_reset_icon_texture(icon, &ingame_st.icon,
            TEXTURE_FILTER_MIPMAP_LINEAR, &w, &h);
   }
}

/* Bottom bar: [icon] [title] ........ [Settings] [Continue] [Quit] */
static bool ingame_frame(video_frame_info_t *video_info)
{
   float color[16];
   unsigned i;
   void *userdata                 = video_info->userdata;
   unsigned width                 = video_info->width;
   unsigned height                = video_info->height;
   gfx_display_t *p_disp          = (gfx_display_t*)video_info->disp_userdata;
   video_driver_state_t *video_st = video_state_get_ptr();
   float bar_h                    = height * 0.12f;
   float bar_y                    = height - bar_h;
   float pad                      = bar_h * 0.15f;
   float icon_size                = bar_h - 2.0f * pad;
   float btn_w                    = height * 0.17f;
   float btn_h                    = bar_h * 0.5f;
   float btn_y                    = bar_y + (bar_h - btn_h) / 2.0f;
   float btn_x                    = width - 2.0f * pad
         - INGAME_BUTTON_COUNT * btn_w - (INGAME_BUTTON_COUNT - 1) * pad;
   float text_x                   = 2.0f * pad + icon_size + 1.5f * pad;
   /* Rough vertical centring for the font's baseline. */
   float text_dy                  = height * 0.012f;

   if (ingame_st.xmb)
      return false;

   ingame_update_game();

   if (video_st->current_video && video_st->current_video->set_viewport)
      video_st->current_video->set_viewport(
            video_st->data, width, height, true, false);

   /* Light dim over the paused game, then the bar. */
   grid_set_color(color, 0x00000060);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         0, 0, width, (unsigned)bar_y, width, height, color, NULL);

   grid_set_color(color, 0x14161cee);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         0, (int)bar_y, width, (unsigned)bar_h, width, height, color, NULL);

   /* Game icon */
   grid_set_color(color, GRID_COL_TILE);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         (int)(2.0f * pad), (int)(bar_y + pad),
         (unsigned)icon_size, (unsigned)icon_size,
         width, height, color, NULL);

   if (ingame_st.icon)
   {
      grid_set_color(color, 0xffffffff);
      gfx_display_draw_quad(p_disp, userdata, width, height,
            (int)(2.0f * pad), (int)(bar_y + pad),
            (unsigned)icon_size, (unsigned)icon_size,
            width, height, color, &ingame_st.icon);
   }

   /* Game title, scrolling if wider than the space left of the buttons */
   grid_draw_marquee(p_disp, userdata, ingame_st.title,
         text_x, bar_y + bar_h / 2.0f + text_dy,
         btn_x - text_x - 2.0f * pad, width, height,
         GRID_COL_TEXT, 1.0f, TEXT_ALIGN_LEFT, &ingame_st);

   /* Buttons */
   for (i = 0; i < INGAME_BUTTON_COUNT; i++)
   {
      float x       = btn_x + i * (btn_w + pad);
      bool selected = (i == ingame_st.selection);

      grid_set_color(color, selected ? GRID_COL_SELECT : GRID_COL_TILE);
      gfx_display_draw_quad(p_disp, userdata, width, height,
            (int)x, (int)btn_y, (unsigned)btn_w, (unsigned)btn_h,
            width, height, color, NULL);

      grid_draw_text(ingame_labels[i], x + btn_w / 2.0f,
            btn_y + btn_h / 2.0f + text_dy * 0.8f, width, height,
            selected ? GRID_COL_TEXT : GRID_COL_TEXT_DIM,
            TEXT_ALIGN_CENTER, 0.8f);
   }

   if (grid_st.font && grid_st.font->renderer && grid_st.font->renderer->flush)
      grid_st.font->renderer->flush(width, height,
            grid_st.font->renderer_data);

   return true;
}

bool grid_home_frame(video_frame_info_t *video_info)
{
   float color[16];
   size_t i;
   size_t rows_visible;
   unsigned cols;
   float tile_w, tile_h, gap, label_h, top, left, art_h;
   void *userdata                 = video_info->userdata;
   unsigned width                 = video_info->width;
   unsigned height                = video_info->height;
   gfx_display_t *p_disp          = (gfx_display_t*)video_info->disp_userdata;
   video_driver_state_t *video_st = video_state_get_ptr();
   settings_t *settings           = config_get_ptr();

   if (video_info->libretro_running)
      return ingame_frame(video_info);

   if (!grid_st.active)
      return false;

   if (     !grid_st.scanned
         || !string_is_equal(grid_st.scanned_dir,
               settings->paths.directory_menu_content))
      grid_scan();

   /* RetroAchievements lookups finishing in the background. */
   if (grid_meta_poll())
      for (i = 0; i < grid_st.count; i++)
         grid_apply_meta(&grid_st.games[i]);

   if (video_st->current_video && video_st->current_video->set_viewport)
      video_st->current_video->set_viewport(
            video_st->data, width, height, true, false);

   /* Layout scales with window height. */
   gap     = height * 0.03f;
   tile_w  = height * 0.1333f;       /* 144px at 1080p */
   art_h   = tile_w;                 /* square art area */
   label_h = height * 0.045f;
   tile_h  = art_h + label_h;
   top     = height * 0.14f;

   cols = (unsigned)((width - gap) / (tile_w + gap));
   if (cols < 1)
      cols = 1;
   grid_st.cols = cols;
   left = (width - (cols * tile_w + (cols - 1) * gap)) / 2.0f;

   rows_visible = (size_t)((height - top - height * 0.08f) / (tile_h + gap));
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

   /* Background */
   grid_set_color(color, GRID_COL_BG);
   gfx_display_draw_quad(p_disp, userdata, width, height,
         0, 0, width, height, width, height, color, NULL);

   /* Header */
   grid_draw_text("Games", left, height * 0.09f, width, height,
         GRID_COL_TEXT, TEXT_ALIGN_LEFT, 1.6f);

   if (grid_st.count == 0)
   {
      char msg[PATH_MAX_LENGTH + 64];
      snprintf(msg, sizeof(msg), "No games found in %s",
            string_is_empty(settings->paths.directory_menu_content)
            ? "(File Browser directory not set)"
            : settings->paths.directory_menu_content);
      grid_draw_text(msg, width / 2.0f, height / 2.0f, width, height,
            GRID_COL_TEXT_DIM, TEXT_ALIGN_CENTER, 1.0f);
   }

   for (i = grid_st.scroll_row * cols;
        i < grid_st.count && i < (grid_st.scroll_row + rows_visible) * cols;
        i++)
   {
      grid_game_t *game = &grid_st.games[i];
      size_t col        = i % cols;
      size_t row        = i / cols - grid_st.scroll_row;
      float x           = left + col * (tile_w + gap);
      float y           = top + row * (tile_h + gap);
      bool selected     = (i == grid_st.selection);

      if (!game->art_tried)
      {
         game->art_tried = true;
         if (!string_is_empty(game->art_path))
            gfx_display_reset_icon_texture(game->art_path, &game->art,
                  TEXTURE_FILTER_MIPMAP_LINEAR, &game->art_w, &game->art_h);
      }

      if (selected)
      {
         float b = height * 0.006f;
         grid_set_color(color, GRID_COL_SELECT);
         gfx_display_draw_quad(p_disp, userdata, width, height,
               (int)(x - b), (int)(y - b),
               (unsigned)(tile_w + 2 * b), (unsigned)(art_h + 2 * b),
               width, height, color, NULL);
      }

      grid_set_color(color, GRID_COL_TILE);
      gfx_display_draw_quad(p_disp, userdata, width, height,
            (int)x, (int)y, (unsigned)tile_w, (unsigned)art_h,
            width, height, color, NULL);

      if (game->art && game->art_w && game->art_h)
      {
         /* Fit inside the tile, keeping aspect ratio. */
         float s  = tile_w / game->art_w;
         float aw, ah;
         if (game->art_h * s > art_h)
            s = art_h / game->art_h;
         aw = game->art_w * s;
         ah = game->art_h * s;

         grid_set_color(color, 0xffffffff);
         gfx_display_draw_quad(p_disp, userdata, width, height,
               (int)(x + (tile_w - aw) / 2), (int)(y + (art_h - ah) / 2),
               (unsigned)aw, (unsigned)ah,
               width, height, color, &game->art);
      }
      else
         grid_draw_text(game->system, x + tile_w / 2,
               y + art_h / 2 + height * 0.012f, width, height,
               GRID_COL_TEXT_DIM, TEXT_ALIGN_CENTER, 1.0f);

      if (selected)
         /* Selected: scroll names that don't fit. */
         grid_draw_marquee(p_disp, userdata, game->title,
               x - gap * 0.25f, y + art_h + label_h * 0.6f,
               tile_w + gap * 0.5f, width, height,
               GRID_COL_TEXT, 0.6f, TEXT_ALIGN_CENTER, game);
      else
      {
         char label[NAME_MAX_LENGTH];
         grid_fit_text(label, sizeof(label), game->title,
               tile_w + gap * 0.5f, height, 0.6f);
         grid_draw_text(label, x + tile_w / 2,
               y + art_h + label_h * 0.6f, width, height,
               GRID_COL_TEXT_DIM, TEXT_ALIGN_CENTER, 0.6f);
      }
   }

   /* Footer hints */
   grid_draw_text("A  Play        Start  Settings", width / 2.0f,
         height - height * 0.035f, width, height,
         GRID_COL_TEXT_DIM, TEXT_ALIGN_CENTER, 0.8f);

   if (grid_st.font && grid_st.font->renderer && grid_st.font->renderer->flush)
      grid_st.font->renderer->flush(width, height,
            grid_st.font->renderer_data);

   return true;
}
