#ifndef __BOMBA_GRID_SOCIAL_H
#define __BOMBA_GRID_SOCIAL_H

#include <stddef.h>
#include <time.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

enum grid_social_state
{
   GRID_SOCIAL_LOGGED_OUT = 0, /* no RetroAchievements login in settings */
   GRID_SOCIAL_LOADING,        /* first fetch in flight */
   GRID_SOCIAL_READY,
   GRID_SOCIAL_ERROR           /* fetch failed; shows the last list, if any */
};

typedef struct
{
   char name[64];
   char game[128];       /* game of the last activity */
   char activity[192];   /* rich presence / what they were doing */
   char avatar[1024];    /* cached avatar image path; empty until downloaded */
   time_t when;
} grid_friend_t;

/* Fetches the users the player follows on RetroAchievements (again if the
 * last fetch is older than a minute), and their avatars. */
void grid_social_refresh(void);

enum grid_social_state grid_social_state(void);
size_t grid_social_count(void);
const grid_friend_t *grid_social_get(size_t idx);

/* Runs queued avatar downloads. Returns true when anything changed (new
 * list or avatar), so the caller can reload images. */
bool grid_social_poll(void);

RETRO_END_DECLS

#endif
