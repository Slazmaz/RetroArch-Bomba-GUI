#ifndef __BOMBA_AUTOSAVE_H
#define __BOMBA_AUTOSAVE_H

#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Call once per main loop iteration: writes retroarch.cfg soon after a
 * setting or key bind changes, so changes survive a crash or a kill. */
void bomba_autosave_poll(void);

RETRO_END_DECLS

#endif
