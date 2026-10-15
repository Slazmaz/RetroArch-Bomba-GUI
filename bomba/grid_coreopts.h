#ifndef __BOMBA_GRID_COREOPTS_H
#define __BOMBA_GRID_COREOPTS_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* An emulator's own settings (its core options), shown and changed while
 * it isn't running. A core only says what its options are while it runs,
 * so they are remembered (with the file RetroArch keeps their values in)
 * each time the menu opens over a game; until a core has run once, its
 * settings aren't known. */

/* With a game running: remember its core's options. */
void grid_coreopts_capture(void);

/* Selects the core (by its file path) the calls below are about. Returns
 * false if its settings aren't known yet. */
bool grid_coreopts_select(const char *core_path);

/* Its option categories ("Video", "Audio", ...), possibly none. */
unsigned grid_coreopts_cat_count(void);
const char *grid_coreopts_cat_name(unsigned cat);

/* Its options; cat is the category index, or -1 for none. */
unsigned grid_coreopts_count(void);
const char *grid_coreopts_name(unsigned opt);
const char *grid_coreopts_info(unsigned opt);
int grid_coreopts_cat(unsigned opt);

/* An option's values, by index: how many, the label of one, which is
 * current and which the default. */
unsigned grid_coreopts_value_count(unsigned opt);
const char *grid_coreopts_value_label(unsigned opt, unsigned val);
unsigned grid_coreopts_value(unsigned opt);
unsigned grid_coreopts_default(unsigned opt);

/* Changes an option: in the running core if it's this one, else in the
 * file RetroArch reads it from next time. */
void grid_coreopts_set(unsigned opt, unsigned val);

/* What the emulator calls a RetroPad control (id 0-15: "A", "Jump"...):
 * NULL if not known yet, "" if the emulator doesn't use it. */
const char *grid_coreopts_button_name(unsigned id);

RETRO_END_DECLS

#endif
