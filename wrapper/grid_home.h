#ifndef __WRAPPER_GRID_HOME_H
#define __WRAPPER_GRID_HOME_H

#include <boolean.h>
#include <retro_common_api.h>

#include "../gfx/video_driver.h"
#include "../menu/menu_defines.h"

RETRO_BEGIN_DECLS

/* Draws the grid if it should be shown. Returns true if it drew,
 * in which case the caller skips its own frame. */
bool grid_home_frame(video_frame_info_t *video_info);

/* Handles a menu action. Returns true if consumed. */
bool grid_home_entry_action(enum menu_action action, unsigned xmb_depth);

void grid_home_context_reset(bool is_threaded);
void grid_home_context_destroy(void);

RETRO_END_DECLS

#endif
