/* Bomba: an emulator's own settings (core options) outside the
 * game.
 *
 * While a core runs, RetroArch's option manager has its options (keys,
 * names, values, categories) and the file their values live in. Each time
 * the menu opens over a game, they are written to a cache file per core
 * (config/Bomba/<core file>.opts). The Settings screen reads
 * that back to list and change the options any time: a change goes to the
 * running core if it is that one, otherwise straight into RetroArch's
 * options file, which the core reads when it next starts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/config_file.h>
#include <file/file_path.h>
#include <lists/string_list.h>
#include <string/stdstring.h>

#include "grid_coreopts.h"

#include "../configuration.h"
#include "../core_option_manager.h"
#include "../file_path_special.h"
#include "../paths.h"
#include "../runloop.h"

#define GRID_COREOPTS_SEP "\t" /* between values in the cache */

typedef struct
{
   char *key;
   char *name;
   char *info;
   int cat;
   unsigned def;
   struct string_list *vals;
   struct string_list *labels;
} grid_coreopt_t;

static struct
{
   char core[PATH_MAX_LENGTH];     /* core file selected ("" none) */
   char conf_path[PATH_MAX_LENGTH];/* RetroArch's options file for it */
   char **cats;
   unsigned cat_count;
   grid_coreopt_t *opts;
   unsigned count;
   bool have_buttons;              /* button names known */
   char buttons[16][64];           /* the emulator's names for them */
   char captured[PATH_MAX_LENGTH]; /* core file last captured */
} grid_coreopts_st;

/* config/Bomba/<core file name>.opts */
static bool grid_coreopts_cache_path(const char *core_path, char *s,
      size_t len)
{
   char dir[PATH_MAX_LENGTH], base[PATH_MAX_LENGTH];

   if (string_is_empty(core_path))
      return false;
   fill_pathname_application_special(dir, sizeof(dir),
         APPLICATION_SPECIAL_DIRECTORY_CONFIG);
   if (string_is_empty(dir))
      return false;
   fill_pathname_join_special(base, dir, "Bomba", sizeof(base));
   if (!path_is_directory(base))
      path_mkdir(base);
   fill_pathname_join_special(s, base, path_basename(core_path), len);
   strlcat(s, ".opts", len);
   return true;
}

/* Text for a config value: no quotes or line breaks. */
static void grid_coreopts_clean(const char *in, char *out, size_t len)
{
   size_t i;
   for (i = 0; in && in[i] && i + 1 < len; i++)
      out[i] = (in[i] == '"') ? '\''
             : (in[i] == '\n' || in[i] == '\r') ? ' ' : in[i];
   out[i] = '\0';
}

static void grid_coreopts_set_str(config_file_t *conf, const char *key,
      const char *val)
{
   char clean[2048];
   grid_coreopts_clean(val ? val : "", clean, sizeof(clean));
   config_set_string(conf, key, clean);
}

void grid_coreopts_capture(void)
{
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   core_option_manager_t *mgr  = runloop_st->core_options;
   const char *core_path       = path_get(RARCH_PATH_CORE);
   char path[PATH_MAX_LENGTH], key[64];
   config_file_t *conf;
   size_t i, j, n = 0;

   if (!mgr || string_is_empty(core_path))
      return;
   /* Once per core per session: options don't change while it runs
    * (their visibility might, but that is only a nicety). */
   if (string_is_equal(grid_coreopts_st.captured, core_path))
      return;
   if (!grid_coreopts_cache_path(core_path, path, sizeof(path)))
      return;
   if (!(conf = config_file_new_alloc()))
      return;

   grid_coreopts_set_str(conf, "conf_path", mgr->conf_path);

   /* What the emulator calls player 1's RetroPad controls. */
   for (i = 0; i < 16; i++)
   {
      snprintf(key, sizeof(key), "btn%u", (unsigned)i);
      grid_coreopts_set_str(conf, key,
            runloop_st->system.input_desc_btn[0][i]);
   }
   config_set_string(conf, "buttons", "true");

   config_set_int(conf, "cats", (int)mgr->cats_size);
   for (i = 0; i < mgr->cats_size; i++)
   {
      snprintf(key, sizeof(key), "cat%u_key", (unsigned)i);
      grid_coreopts_set_str(conf, key, mgr->cats[i].key);
      snprintf(key, sizeof(key), "cat%u_name", (unsigned)i);
      grid_coreopts_set_str(conf, key, mgr->cats[i].desc);
   }

   for (i = 0; i < mgr->size; i++)
   {
      struct core_option *o = &mgr->opts[i];
      char vals[4096], labels[4096];
      const char *name;

      if (!o->visible || !o->vals || !o->vals->size)
         continue;

      vals[0] = labels[0] = '\0';
      for (j = 0; j < o->vals->size; j++)
      {
         char v[256];
         const char *label = (o->val_labels && j < o->val_labels->size
               && !string_is_empty(o->val_labels->elems[j].data))
            ? o->val_labels->elems[j].data : o->vals->elems[j].data;
         if (j)
         {
            strlcat(vals,   GRID_COREOPTS_SEP, sizeof(vals));
            strlcat(labels, GRID_COREOPTS_SEP, sizeof(labels));
         }
         grid_coreopts_clean(o->vals->elems[j].data, v, sizeof(v));
         strlcat(vals, v, sizeof(vals));
         grid_coreopts_clean(label, v, sizeof(v));
         strlcat(labels, v, sizeof(labels));
      }

      /* In a category the short name reads right ("Aspect Ratio" under
       * Video); otherwise the full one. */
      name = (!string_is_empty(o->category_key)
            && !string_is_empty(o->desc_categorized))
         ? o->desc_categorized : o->desc;

      snprintf(key, sizeof(key), "opt%u_key", (unsigned)n);
      grid_coreopts_set_str(conf, key, o->key);
      snprintf(key, sizeof(key), "opt%u_name", (unsigned)n);
      grid_coreopts_set_str(conf, key, name);
      snprintf(key, sizeof(key), "opt%u_info", (unsigned)n);
      grid_coreopts_set_str(conf, key,
            !string_is_empty(o->category_key) && o->info_categorized
            ? o->info_categorized : o->info);
      snprintf(key, sizeof(key), "opt%u_cat", (unsigned)n);
      grid_coreopts_set_str(conf, key, o->category_key);
      snprintf(key, sizeof(key), "opt%u_default", (unsigned)n);
      config_set_int(conf, key, (int)o->default_index);
      snprintf(key, sizeof(key), "opt%u_vals", (unsigned)n);
      config_set_string(conf, key, vals);
      snprintf(key, sizeof(key), "opt%u_labels", (unsigned)n);
      config_set_string(conf, key, labels);
      n++;
   }
   config_set_int(conf, "opts", (int)n);

   config_file_write(conf, path, true);
   config_file_free(conf);
   strlcpy(grid_coreopts_st.captured, core_path,
         sizeof(grid_coreopts_st.captured));

   /* The cached copy (if this core is selected) is stale now. */
   if (string_is_equal(grid_coreopts_st.core, core_path))
      grid_coreopts_st.core[0] = '\0';
}

static void grid_coreopts_free(void)
{
   unsigned i;
   for (i = 0; i < grid_coreopts_st.cat_count; i++)
      free(grid_coreopts_st.cats[i]);
   free(grid_coreopts_st.cats);
   for (i = 0; i < grid_coreopts_st.count; i++)
   {
      grid_coreopt_t *o = &grid_coreopts_st.opts[i];
      free(o->key);
      free(o->name);
      free(o->info);
      string_list_free(o->vals);
      string_list_free(o->labels);
   }
   free(grid_coreopts_st.opts);
   grid_coreopts_st.cats      = NULL;
   grid_coreopts_st.cat_count = 0;
   grid_coreopts_st.opts         = NULL;
   grid_coreopts_st.count        = 0;
   grid_coreopts_st.core[0]      = '\0';
   grid_coreopts_st.have_buttons = false;
}

const char *grid_coreopts_button_name(unsigned id)
{
   if (!grid_coreopts_st.have_buttons || id >= 16)
      return NULL;
   return grid_coreopts_st.buttons[id];
}

static char *grid_coreopts_get_str(config_file_t *conf, const char *key)
{
   char buf[2048];
   buf[0] = '\0';
   config_get_array(conf, key, buf, sizeof(buf));
   return strdup(buf);
}

bool grid_coreopts_select(const char *core_path)
{
   char path[PATH_MAX_LENGTH], key[64];
   char **cat_keys = NULL;
   config_file_t *conf;
   int cats = 0, opts = 0;
   unsigned i;

   if (string_is_empty(core_path))
   {
      grid_coreopts_free();
      return false;
   }
   if (string_is_equal(grid_coreopts_st.core, core_path))
      return grid_coreopts_st.count > 0;

   grid_coreopts_free();
   strlcpy(grid_coreopts_st.core, core_path, sizeof(grid_coreopts_st.core));

   if (     !grid_coreopts_cache_path(core_path, path, sizeof(path))
         || !(conf = config_file_new_from_path_to_string(path)))
      return false;

   config_get_array(conf, "conf_path", grid_coreopts_st.conf_path,
         sizeof(grid_coreopts_st.conf_path));

   config_get_bool(conf, "buttons", &grid_coreopts_st.have_buttons);
   for (i = 0; grid_coreopts_st.have_buttons && i < 16; i++)
   {
      snprintf(key, sizeof(key), "btn%u", i);
      grid_coreopts_st.buttons[i][0] = '\0';
      config_get_array(conf, key, grid_coreopts_st.buttons[i],
            sizeof(grid_coreopts_st.buttons[i]));
   }
   config_get_int(conf, "cats", &cats);
   config_get_int(conf, "opts", &opts);

   if (cats > 0)
   {
      grid_coreopts_st.cats = (char**)calloc(cats, sizeof(char*));
      cat_keys              = (char**)calloc(cats, sizeof(char*));
   }
   for (i = 0; grid_coreopts_st.cats && cat_keys && i < (unsigned)cats; i++)
   {
      snprintf(key, sizeof(key), "cat%u_key", i);
      cat_keys[i] = grid_coreopts_get_str(conf, key);
      snprintf(key, sizeof(key), "cat%u_name", i);
      grid_coreopts_st.cats[i] = grid_coreopts_get_str(conf, key);
      grid_coreopts_st.cat_count++;
   }

   if (opts > 0)
      grid_coreopts_st.opts = (grid_coreopt_t*)calloc(opts,
            sizeof(grid_coreopt_t));
   for (i = 0; grid_coreopts_st.opts && i < (unsigned)opts; i++)
   {
      grid_coreopt_t *o = &grid_coreopts_st.opts[grid_coreopts_st.count];
      char *cat, *vals, *labels;
      int def = 0;
      unsigned k;

      snprintf(key, sizeof(key), "opt%u_key", i);
      o->key  = grid_coreopts_get_str(conf, key);
      snprintf(key, sizeof(key), "opt%u_name", i);
      o->name = grid_coreopts_get_str(conf, key);
      snprintf(key, sizeof(key), "opt%u_info", i);
      o->info = grid_coreopts_get_str(conf, key);
      snprintf(key, sizeof(key), "opt%u_default", i);
      config_get_int(conf, key, &def);
      o->def = def > 0 ? (unsigned)def : 0;

      snprintf(key, sizeof(key), "opt%u_cat", i);
      cat    = grid_coreopts_get_str(conf, key);
      o->cat = -1;
      for (k = 0; cat_keys && k < grid_coreopts_st.cat_count; k++)
         if (!string_is_empty(cat) && string_is_equal(cat, cat_keys[k]))
            o->cat = (int)k;
      free(cat);

      snprintf(key, sizeof(key), "opt%u_vals", i);
      vals      = grid_coreopts_get_str(conf, key);
      snprintf(key, sizeof(key), "opt%u_labels", i);
      labels    = grid_coreopts_get_str(conf, key);
      o->vals   = string_split(vals, GRID_COREOPTS_SEP);
      o->labels = string_split(labels, GRID_COREOPTS_SEP);
      free(vals);
      free(labels);

      if (!o->key || !o->vals || !o->vals->size)
      {
         free(o->key);
         free(o->name);
         free(o->info);
         string_list_free(o->vals);
         string_list_free(o->labels);
         memset(o, 0, sizeof(*o));
         continue;
      }
      if (o->def >= o->vals->size)
         o->def = 0;
      grid_coreopts_st.count++;
   }

   for (i = 0; cat_keys && i < grid_coreopts_st.cat_count; i++)
      free(cat_keys[i]);
   free(cat_keys);
   config_file_free(conf);
   return grid_coreopts_st.count > 0;
}

unsigned grid_coreopts_cat_count(void)
{
   return grid_coreopts_st.cat_count;
}

const char *grid_coreopts_cat_name(unsigned cat)
{
   return cat < grid_coreopts_st.cat_count && grid_coreopts_st.cats[cat]
      ? grid_coreopts_st.cats[cat] : "";
}

unsigned grid_coreopts_count(void)
{
   return grid_coreopts_st.count;
}

const char *grid_coreopts_name(unsigned opt)
{
   return opt < grid_coreopts_st.count ? grid_coreopts_st.opts[opt].name : "";
}

const char *grid_coreopts_info(unsigned opt)
{
   return opt < grid_coreopts_st.count ? grid_coreopts_st.opts[opt].info : "";
}

int grid_coreopts_cat(unsigned opt)
{
   return opt < grid_coreopts_st.count ? grid_coreopts_st.opts[opt].cat : -1;
}

unsigned grid_coreopts_value_count(unsigned opt)
{
   return opt < grid_coreopts_st.count
      ? (unsigned)grid_coreopts_st.opts[opt].vals->size : 0;
}

const char *grid_coreopts_value_label(unsigned opt, unsigned val)
{
   grid_coreopt_t *o;
   if (opt >= grid_coreopts_st.count)
      return "";
   o = &grid_coreopts_st.opts[opt];
   if (o->labels && val < o->labels->size)
      return o->labels->elems[val].data;
   return val < o->vals->size ? o->vals->elems[val].data : "";
}

unsigned grid_coreopts_default(unsigned opt)
{
   return opt < grid_coreopts_st.count ? grid_coreopts_st.opts[opt].def : 0;
}

/* RetroArch's option manager, if the selected core is the one running. */
static core_option_manager_t *grid_coreopts_live(void)
{
   const char *running = path_get(RARCH_PATH_CORE);
   if (     string_is_empty(running)
         || !string_is_equal(running, grid_coreopts_st.core))
      return NULL;
   return runloop_state_get_ptr()->core_options;
}

unsigned grid_coreopts_value(unsigned opt)
{
   core_option_manager_t *mgr = grid_coreopts_live();
   grid_coreopt_t *o;
   char val[256];
   size_t idx, k;

   if (opt >= grid_coreopts_st.count)
      return 0;
   o = &grid_coreopts_st.opts[opt];

   if (mgr && core_option_manager_get_idx(mgr, o->key, &idx))
   {
      const char *cur = core_option_manager_get_val(mgr, idx);
      for (k = 0; cur && k < o->vals->size; k++)
         if (string_is_equal(cur, o->vals->elems[k].data))
            return (unsigned)k;
   }

   /* The value in RetroArch's options file (re-read each time: cheap and
    * never stale). */
   val[0] = '\0';
   if (!string_is_empty(grid_coreopts_st.conf_path))
   {
      config_file_t *conf = config_file_new_from_path_to_string(
            grid_coreopts_st.conf_path);
      if (conf)
      {
         config_get_array(conf, o->key, val, sizeof(val));
         config_file_free(conf);
      }
   }
   for (k = 0; *val && k < o->vals->size; k++)
      if (string_is_equal(val, o->vals->elems[k].data))
         return (unsigned)k;
   return o->def;
}

void grid_coreopts_set(unsigned opt, unsigned val)
{
   core_option_manager_t *mgr = grid_coreopts_live();
   grid_coreopt_t *o;
   config_file_t *conf;
   size_t idx;

   if (opt >= grid_coreopts_st.count)
      return;
   o = &grid_coreopts_st.opts[opt];
   if (val >= o->vals->size)
      return;

   /* Running: RetroArch applies it and saves its file when the core
    * closes. */
   if (mgr && core_option_manager_get_idx(mgr, o->key, &idx))
   {
      core_option_manager_set_val(mgr, idx, val, false);
      return;
   }

   if (string_is_empty(grid_coreopts_st.conf_path))
      return;
   if (!(conf = config_file_new_from_path_to_string(
               grid_coreopts_st.conf_path)))
      conf = config_file_new_alloc();
   if (!conf)
      return;
   config_set_string(conf, o->key, o->vals->elems[val].data);
   config_file_write(conf, grid_coreopts_st.conf_path, true);
   config_file_free(conf);
}
