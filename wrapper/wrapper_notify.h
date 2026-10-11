#ifndef __WRAPPER_NOTIFY_H
#define __WRAPPER_NOTIFY_H

#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Call once per main loop iteration. */
void wrapper_notify_poll(void);

RETRO_END_DECLS

#endif
