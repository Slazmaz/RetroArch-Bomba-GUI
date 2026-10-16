#ifndef __BOMBA_CLIPBOARD_H
#define __BOMBA_CLIPBOARD_H

#include <stddef.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Puts len bytes of UTF-8 text on the system clipboard. */
bool bomba_clipboard_set(const char *text, size_t len);

/* The clipboard's text as UTF-8 (free it), or NULL if it holds none. */
char *bomba_clipboard_get(void);

RETRO_END_DECLS

#endif
