#ifndef __BOMBA_KEYMAP_H
#define __BOMBA_KEYMAP_H

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* The on-screen keyboard's character keys: four rows of eleven, as on
 * the player's own keyboard (its number row, its top letter row and so
 * on, so a Nordic layout has å, ö and ä), each key's character unshifted
 * and shifted as UTF-8 ("" where the layout has none). */
#define BOMBA_KEYMAP_ROWS 4
#define BOMBA_KEYMAP_COLS 11
#define BOMBA_KEYMAP_KEYS (BOMBA_KEYMAP_ROWS * BOMBA_KEYMAP_COLS)

typedef struct
{
   char base[8];
   char shift[8];
} bomba_key_t;

/* How many keyboard layouts the system has (at least 1). */
unsigned bomba_keymap_count(void);

/* The layout in use now, as an index for bomba_keymap_get. */
unsigned bomba_keymap_current(void);

/* Fills keys from layout idx (wrapped to the count). */
void bomba_keymap_get(unsigned idx, bomba_key_t *keys);

RETRO_END_DECLS

#endif
