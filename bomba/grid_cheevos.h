#ifndef __BOMBA_GRID_CHEEVOS_H
#define __BOMBA_GRID_CHEEVOS_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

enum grid_cheevos_state
{
   GRID_CHEEVOS_IDLE = 0,
   GRID_CHEEVOS_LOGGED_OUT,  /* no RetroAchievements login in settings */
   GRID_CHEEVOS_NO_GAME,     /* game not on RetroAchievements */
   GRID_CHEEVOS_LOADING,
   GRID_CHEEVOS_READY,
   GRID_CHEEVOS_ERROR
};

typedef struct
{
   char title[128];
   char description[256];
   char badge[1024];     /* cached badge image (locked or unlocked look);
                            empty until downloaded */
   unsigned points;
   float rarity;         /* % of players who have it (lower = rarer) */
   bool unlocked;
} grid_cheevo_t;

/* Loads a game's achievements (core set) and which the player has
 * unlocked, from RetroAchievements with the login in settings. game_id 0
 * means the game isn't on RetroAchievements. */
void grid_cheevos_open(unsigned game_id);

enum grid_cheevos_state grid_cheevos_state(void);
size_t grid_cheevos_count(void);
const grid_cheevo_t *grid_cheevos_get(size_t idx);

/* Runs badge downloads. Returns true when anything changed (list loaded,
 * a badge arrived). */
bool grid_cheevos_poll(void);

/* A game's progress: achievements unlocked of its total (core set).
 * Known progress is kept on disk across launches. Returns true when known
 * (possibly stale); with fetch set, an unknown or stale game is looked up
 * if no lookup is running (one game at a time). Known progress is not
 * looked up again until the game is played. */
bool grid_cheevos_progress(unsigned game_id, bool fetch,
      unsigned *unlocked, unsigned *total);

/* Marks a game's known progress stale (it was played, so unlocks may have
 * changed): it is still returned, and looked up again on the next fetch. */
void grid_cheevos_progress_reset(unsigned game_id);

RETRO_END_DECLS

#endif
