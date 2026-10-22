/* Bomba: per-console settings (emulator and bindings).
 *
 * Kept in one file next to RetroArch's remaps (Bomba.cfg in the
 * remap directory): "<console>_core = <core file>" for a chosen emulator,
 * and "<console>_<control>_btn / _axis / _key" for the controls given a
 * binding of their own. RetroArch has no per-console settings, and one
 * core can run several consoles (mGBA: GB, GBC and GBA), so before each
 * game the grid starts, its console's bindings are written as RetroArch's
 * override for that game (<config>/<core>/<game>.cfg), which RetroArch
 * loads over the usual binds and undoes when the game closes. Those files
 * carry a marker line; an override without it is the player's own and is
 * never touched.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <compat/strl.h>
#include <file/config_file.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "grid_controls.h"

#include "../configuration.h"
#include "../core_info.h"
#include "../file_path_special.h"
#include "../paths.h"
#include "../runloop.h"
#include "../input/input_defines.h"
#include "../input/input_driver.h"
#include "../input/input_keymaps.h"
#include "../input/input_remapping.h"

#define GRID_CONTROLS_FILE    "Bomba.cfg"
#define GRID_CONTROLS_MARKER  "bomba_console_buttons"
#define GRID_CONTROLS_CONSOLES 48

/* RetroArch's names for the RetroPad controls in remap files, by id. */
static const char *grid_controls_key[GRID_CONTROLS_BUTTONS] = {
   "b", "y", "select", "start", "up", "down", "left", "right",
   "a", "x", "l", "r", "l2", "r2", "l3", "r3"
};

static const char *grid_controls_names[GRID_CONTROLS_BUTTONS] = {
   "Bottom button", "Left button", "Select", "Start",
   "D-pad up", "D-pad down", "D-pad left", "D-pad right",
   "Right button", "Top button", "L", "R",
   "L2", "R2", "Left stick press", "Right stick press"
};

const unsigned grid_controls_order[GRID_CONTROLS_BUTTONS] = {
   0, 8, 1, 9,       /* bottom, right, left, top */
   4, 5, 6, 7,       /* d-pad */
   10, 11, 12, 13,   /* L, R, L2, R2 */
   14, 15, 3, 2      /* stick presses, Start, Select */
};

typedef struct
{
   uint16_t joykey;   /* NO_BTN: the RetroPad setup's */
   uint32_t joyaxis;  /* AXIS_NONE: the RetroPad setup's */
   unsigned key;      /* RETROK_UNKNOWN: the RetroPad setup's */
} grid_controls_bind_t;

static struct
{
   unsigned count;
   struct
   {
      char console[40];
      char core[128];
      grid_controls_bind_t binds[GRID_CONTROLS_BUTTONS];
   } consoles[GRID_CONTROLS_CONSOLES];
} grid_controls_st;

/* The running game's console, when its bindings came from our override,
 * and the usual binds from before it: the menu keeps those (it reads the
 * same binds as the game). */
static struct
{
   char console[40];
   char path[PATH_MAX_LENGTH];   /* the override */
   grid_controls_bind_t binds[MAX_USERS][GRID_CONTROLS_BUTTONS];
} grid_controls_game;

const char *grid_controls_button_name(unsigned id)
{
   return id < GRID_CONTROLS_BUTTONS ? grid_controls_names[id] : "";
}

static bool grid_controls_file(char *s, size_t len)
{
   const char *dir = config_get_ptr()->paths.directory_input_remapping;
   if (string_is_empty(dir))
      return false;
   fill_pathname_join_special(s, dir, GRID_CONTROLS_FILE, len);
   return true;
}

/* Console label as a config key part: letters and digits, others '_'. */
static void grid_controls_key_prefix(const char *console, char *s, size_t len)
{
   size_t i;
   for (i = 0; console[i] && i + 1 < len; i++)
      s[i] = isalnum((unsigned char)console[i]) ? console[i] : '_';
   s[i] = '\0';
}

static void grid_controls_bind_clear(grid_controls_bind_t *b)
{
   b->joykey  = NO_BTN;
   b->joyaxis = AXIS_NONE;
   b->key     = RETROK_UNKNOWN;
}

static int grid_controls_find(const char *console, bool add)
{
   unsigned i, j;

   for (i = 0; i < grid_controls_st.count; i++)
      if (string_is_equal(grid_controls_st.consoles[i].console, console))
         return (int)i;

   if (!add || grid_controls_st.count >= GRID_CONTROLS_CONSOLES)
      return -1;

   i = grid_controls_st.count++;
   strlcpy(grid_controls_st.consoles[i].console, console,
         sizeof(grid_controls_st.consoles[i].console));
   grid_controls_st.consoles[i].core[0] = '\0';
   for (j = 0; j < GRID_CONTROLS_BUTTONS; j++)
      grid_controls_bind_clear(&grid_controls_st.consoles[i].binds[j]);
   return (int)i;
}

/* A console's entry, read from the file the first time it is asked for. */
static int grid_controls_load(const char *console)
{
   char path[PATH_MAX_LENGTH], prefix[40], key[96];
   config_file_t *conf;
   int c;
   unsigned j;

   if (string_is_empty(console))
      return -1;
   if ((c = grid_controls_find(console, false)) >= 0)
      return c;
   if ((c = grid_controls_find(console, true)) < 0)
      return -1;
   if (     !grid_controls_file(path, sizeof(path))
         || !(conf = config_file_new_from_path_to_string(path)))
      return c;

   grid_controls_key_prefix(console, prefix, sizeof(prefix));
   for (j = 0; j < GRID_CONTROLS_BUTTONS; j++)
   {
      grid_controls_bind_t *b = &grid_controls_st.consoles[c].binds[j];
      uint64_t v;

      snprintf(key, sizeof(key), "%s_%s_btn", prefix, grid_controls_key[j]);
      if (config_get_uint64(conf, key, &v))
         b->joykey = (uint16_t)v;
      snprintf(key, sizeof(key), "%s_%s_axis", prefix, grid_controls_key[j]);
      if (config_get_uint64(conf, key, &v))
         b->joyaxis = (uint32_t)v;
      snprintf(key, sizeof(key), "%s_%s_key", prefix, grid_controls_key[j]);
      if (config_get_uint64(conf, key, &v))
         b->key = (unsigned)v;
   }
   snprintf(key, sizeof(key), "%s_core", prefix);
   config_get_array(conf, key, grid_controls_st.consoles[c].core,
         sizeof(grid_controls_st.consoles[c].core));
   config_file_free(conf);
   return c;
}

/* Rewrites this console's lines in the file, keeping the others. */
static void grid_controls_save(int c)
{
   char path[PATH_MAX_LENGTH], dir[PATH_MAX_LENGTH], prefix[40], key[96];
   config_file_t *conf;
   unsigned j;

   if (c < 0 || !grid_controls_file(path, sizeof(path)))
      return;

   if (!(conf = config_file_new_from_path_to_string(path)))
      conf = config_file_new_alloc();
   if (!conf)
      return;

   grid_controls_key_prefix(grid_controls_st.consoles[c].console,
         prefix, sizeof(prefix));
   for (j = 0; j < GRID_CONTROLS_BUTTONS; j++)
   {
      const grid_controls_bind_t *b = &grid_controls_st.consoles[c].binds[j];

      /* Bindings by RetroPad remap from earlier builds: gone. */
      snprintf(key, sizeof(key), "%s_%s", prefix, grid_controls_key[j]);
      config_unset(conf, key);

      snprintf(key, sizeof(key), "%s_%s_btn", prefix, grid_controls_key[j]);
      if (b->joykey == NO_BTN)
         config_unset(conf, key);
      else
         config_set_uint64(conf, key, b->joykey);
      snprintf(key, sizeof(key), "%s_%s_axis", prefix, grid_controls_key[j]);
      if (b->joyaxis == AXIS_NONE)
         config_unset(conf, key);
      else
         config_set_uint64(conf, key, b->joyaxis);
      snprintf(key, sizeof(key), "%s_%s_key", prefix, grid_controls_key[j]);
      if (b->key == RETROK_UNKNOWN)
         config_unset(conf, key);
      else
         config_set_uint64(conf, key, b->key);
   }
   snprintf(key, sizeof(key), "%s_core", prefix);
   if (string_is_empty(grid_controls_st.consoles[c].core))
      config_unset(conf, key);
   else
      config_set_string(conf, key, grid_controls_st.consoles[c].core);

   fill_pathname_parent_dir(dir, path, sizeof(dir));
   if (!string_is_empty(dir) && !path_is_directory(dir))
      path_mkdir(dir);
   config_file_write(conf, path, true);
   config_file_free(conf);
}

void grid_controls_get_bind(const char *console, unsigned id,
      uint16_t *joykey, uint32_t *joyaxis, unsigned *key)
{
   int c = grid_controls_load(console);
   *joykey  = NO_BTN;
   *joyaxis = AXIS_NONE;
   *key     = RETROK_UNKNOWN;
   if (c < 0 || id >= GRID_CONTROLS_BUTTONS)
      return;
   *joykey  = grid_controls_st.consoles[c].binds[id].joykey;
   *joyaxis = grid_controls_st.consoles[c].binds[id].joyaxis;
   *key     = grid_controls_st.consoles[c].binds[id].key;
}

void grid_controls_set_pad(const char *console, unsigned id,
      uint16_t joykey, uint32_t joyaxis)
{
   int c = grid_controls_load(console);
   if (c < 0 || id >= GRID_CONTROLS_BUTTONS)
      return;
   /* A button or a stick direction, not both. */
   grid_controls_st.consoles[c].binds[id].joykey  = joykey;
   grid_controls_st.consoles[c].binds[id].joyaxis =
      joykey != NO_BTN ? AXIS_NONE : joyaxis;
   grid_controls_save(c);
}

void grid_controls_set_key(const char *console, unsigned id, unsigned key)
{
   int c = grid_controls_load(console);
   if (c < 0 || id >= GRID_CONTROLS_BUTTONS)
      return;
   grid_controls_st.consoles[c].binds[id].key = key;
   grid_controls_save(c);
}

void grid_controls_clear(const char *console, unsigned id)
{
   int c = grid_controls_load(console);
   if (c < 0 || id >= GRID_CONTROLS_BUTTONS)
      return;
   grid_controls_bind_clear(&grid_controls_st.consoles[c].binds[id]);
   grid_controls_save(c);
}

void grid_controls_reset(const char *console)
{
   unsigned j;
   int c = grid_controls_load(console);
   if (c < 0)
      return;
   for (j = 0; j < GRID_CONTROLS_BUTTONS; j++)
      grid_controls_bind_clear(&grid_controls_st.consoles[c].binds[j]);
   grid_controls_save(c);
}

bool grid_controls_changed(const char *console)
{
   unsigned j;
   int c = grid_controls_load(console);
   if (c < 0)
      return false;
   for (j = 0; j < GRID_CONTROLS_BUTTONS; j++)
   {
      const grid_controls_bind_t *b = &grid_controls_st.consoles[c].binds[j];
      if (     b->joykey != NO_BTN || b->joyaxis != AXIS_NONE
            || b->key != RETROK_UNKNOWN)
         return true;
   }
   return false;
}

const char *grid_controls_core(const char *console)
{
   int c = grid_controls_load(console);
   return c < 0 ? "" : grid_controls_st.consoles[c].core;
}

void grid_controls_set_core(const char *console, const char *core_file)
{
   int c = grid_controls_load(console);
   if (c < 0)
      return;
   strlcpy(grid_controls_st.consoles[c].core, core_file ? core_file : "",
         sizeof(grid_controls_st.consoles[c].core));
   grid_controls_save(c);
}

/* Whether a file is one this file wrote. */
static bool grid_controls_is_ours(const char *path)
{
   config_file_t *conf = config_file_new_from_path_to_string(path);
   bool ours           = false;
   if (conf)
   {
      config_get_bool(conf, GRID_CONTROLS_MARKER, &ours);
      config_file_free(conf);
   }
   return ours;
}

/* A gamepad button as RetroArch writes it in a config ("3", "h0up"). */
static void grid_controls_btn_str(uint16_t joykey, char *s, size_t len)
{
   if (joykey == NO_BTN)
      strlcpy(s, "nul", len);
   else if (GET_HAT_DIR(joykey))
   {
      unsigned dir = GET_HAT_DIR(joykey);
      snprintf(s, len, "h%u%s", (unsigned)GET_HAT(joykey),
            dir == HAT_UP_MASK ? "up" : dir == HAT_DOWN_MASK ? "down"
            : dir == HAT_LEFT_MASK ? "left" : "right");
   }
   else
      snprintf(s, len, "%u", (unsigned)joykey);
}

/* A stick direction as RetroArch writes it ("+1", "-0"). */
static void grid_controls_axis_str(uint32_t joyaxis, char *s, size_t len)
{
   if (joyaxis == AXIS_NONE)
      strlcpy(s, "nul", len);
   else if (AXIS_NEG_GET(joyaxis) != AXIS_DIR_NONE)
      snprintf(s, len, "-%u", (unsigned)AXIS_NEG_GET(joyaxis));
   else
      snprintf(s, len, "+%u", (unsigned)AXIS_POS_GET(joyaxis));
}

void grid_controls_apply(const char *console, const char *core_path,
      const char *content_path)
{
   settings_t *settings = config_get_ptr();
   core_info_t *info    = NULL;
   char name[PATH_MAX_LENGTH], dir[PATH_MAX_LENGTH], core_dir[PATH_MAX_LENGTH];
   char path[PATH_MAX_LENGTH];
   config_file_t *conf;
   unsigned p, j;
   int c;

   grid_controls_game.console[0] = '\0';

   if (     string_is_empty(console) || string_is_empty(content_path)
         || !core_info_find(core_path, &info) || !info
         || string_is_empty(info->core_name))
      return;

   strlcpy(name, path_basename_nocompression(content_path), sizeof(name));
   path_remove_extension(name);

   /* Earlier builds remapped the RetroPad in the game's remap file
    * (<remaps>/<core>/<game>.rmp): remove ours. */
   if (!string_is_empty(settings->paths.directory_input_remapping))
   {
      fill_pathname_join_special(core_dir,
            settings->paths.directory_input_remapping, info->core_name,
            sizeof(core_dir));
      fill_pathname_join_special(path, core_dir, name, sizeof(path));
      strlcat(path, ".rmp", sizeof(path));
      if (path_is_valid(path) && grid_controls_is_ours(path))
         filestream_delete(path);
   }

   /* RetroArch's game override: <config>/<core>/<game file name>.cfg */
   fill_pathname_application_special(dir, sizeof(dir),
         APPLICATION_SPECIAL_DIRECTORY_CONFIG);
   if (string_is_empty(dir))
      return;
   fill_pathname_join_special(core_dir, dir, info->core_name,
         sizeof(core_dir));
   fill_pathname_join_special(path, core_dir, name, sizeof(path));
   strlcat(path, ".cfg", sizeof(path));

   /* The player's own override for this game: leave it. */
   if (path_is_valid(path) && !grid_controls_is_ours(path))
      return;

   if (!grid_controls_changed(console) || (c = grid_controls_load(console)) < 0)
   {
      if (path_is_valid(path))
         filestream_delete(path);
      return;
   }

   if (!(conf = config_file_new_alloc()))
      return;
   config_set_string(conf, GRID_CONTROLS_MARKER, "true");

   for (j = 0; j < GRID_CONTROLS_BUTTONS; j++)
   {
      const grid_controls_bind_t *b = &grid_controls_st.consoles[c].binds[j];
      const char *base              = input_config_bind_map_get_base(j);
      char key[96], val[32];

      if (!base)
         continue;

      /* Gamepad: for every player (one controller setup for all). */
      if (b->joykey != NO_BTN || b->joyaxis != AXIS_NONE)
         for (p = 0; p < settings->uints.input_max_users && p < MAX_USERS; p++)
         {
            snprintf(key, sizeof(key), "input_player%u_%s_btn", p + 1, base);
            grid_controls_btn_str(b->joykey, val, sizeof(val));
            config_set_string(conf, key, val);
            snprintf(key, sizeof(key), "input_player%u_%s_axis", p + 1, base);
            grid_controls_axis_str(b->joyaxis, val, sizeof(val));
            config_set_string(conf, key, val);
         }

      /* Keyboard: player 1's only. */
      if (b->key != RETROK_UNKNOWN)
      {
         snprintf(key, sizeof(key), "input_player1_%s", base);
         input_keymaps_translate_rk_to_str((enum retro_key)b->key, val,
               sizeof(val));
         config_set_string(conf, key, val);
      }
   }

   if (!path_is_directory(core_dir))
      path_mkdir(core_dir);
   config_file_write(conf, path, true);
   config_file_free(conf);

   strlcpy(grid_controls_game.console, console,
         sizeof(grid_controls_game.console));
   strlcpy(grid_controls_game.path, path, sizeof(grid_controls_game.path));
   for (p = 0; p < MAX_USERS; p++)
      for (j = 0; j < GRID_CONTROLS_BUTTONS; j++)
      {
         grid_controls_game.binds[p][j].joykey  = input_config_binds[p][j].joykey;
         grid_controls_game.binds[p][j].joyaxis = input_config_binds[p][j].joyaxis;
         grid_controls_game.binds[p][j].key     = input_config_binds[p][j].key;
      }
}

static void grid_controls_apply_running(const char *console);

void grid_controls_changed_running(const char *console, const char *core_path,
      const char *content_path)
{
   /* Ours is in use (they apply when the menu closes), or the player's. */
   if (     grid_controls_game.console[0]
         || !path_is_empty(RARCH_PATH_CONFIG_OVERRIDE))
      return;

   grid_controls_apply(console, core_path, content_path);
   if (!grid_controls_game.console[0])
      return;
   if (!config_load_override_file(grid_controls_game.path))
   {
      grid_controls_game.console[0] = '\0';
      return;
   }
   /* Loaded over the menu, which keeps the usual binds. */
   grid_controls_menu(true);
}

void grid_controls_menu(bool on)
{
   unsigned p, j;
   int c;

   if (     !grid_controls_game.console[0]
         || path_is_empty(RARCH_PATH_CONFIG_OVERRIDE))
      return;
   if (!on)
   {
      grid_controls_apply_running(grid_controls_game.console);
      return;
   }
   if ((c = grid_controls_load(grid_controls_game.console)) < 0)
      return;

   /* Back to the usual binds, where the console has its own. */
   for (j = 0; j < GRID_CONTROLS_BUTTONS; j++)
   {
      const grid_controls_bind_t *b = &grid_controls_st.consoles[c].binds[j];

      if (b->joykey != NO_BTN || b->joyaxis != AXIS_NONE)
         for (p = 0; p < MAX_USERS; p++)
         {
            input_config_binds[p][j].joykey  = grid_controls_game.binds[p][j].joykey;
            input_config_binds[p][j].joyaxis = grid_controls_game.binds[p][j].joyaxis;
         }
      if (b->key != RETROK_UNKNOWN)
         input_config_binds[0][j].key = (enum retro_key)grid_controls_game.binds[0][j].key;
   }
}

/* The console's bindings onto the running game's binds. */
static void grid_controls_apply_running(const char *console)
{
   settings_t *settings = config_get_ptr();
   unsigned p, j;
   int c;

   /* Only while an override is in use: RetroArch reloads the usual binds
    * when it ends, so these can't leak into them. */
   if (     path_is_empty(RARCH_PATH_CONFIG_OVERRIDE)
         || (c = grid_controls_load(console)) < 0)
      return;

   for (j = 0; j < GRID_CONTROLS_BUTTONS; j++)
   {
      const grid_controls_bind_t *b = &grid_controls_st.consoles[c].binds[j];

      if (b->joykey != NO_BTN || b->joyaxis != AXIS_NONE)
         for (p = 0; p < settings->uints.input_max_users && p < MAX_USERS; p++)
         {
            input_config_binds[p][j].joykey  = b->joykey;
            input_config_binds[p][j].joyaxis = b->joyaxis;
         }
      if (b->key != RETROK_UNKNOWN)
         input_config_binds[0][j].key = (enum retro_key)b->key;
   }
}
