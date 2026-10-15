#ifndef __BOMBA_GRID_COVER_H
#define __BOMBA_GRID_COVER_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

#include "grid_meta.h"

RETRO_BEGIN_DECLS

/* Puts a trailing article back in front, as No-Intro and RetroAchievements
 * move it: "Legend of Zelda, The: A Link to the Past" -> "The Legend of
 * Zelda: A Link to the Past". In place (the length stays the same). */
void grid_title_fix_article(char *s);

/* Portrait cover (600x900) from SteamGridDB for a game title, queueing a
 * lookup if it isn't cached. On GRID_META_FOUND, path is the cached image.
 * GRID_META_UNKNOWN when there is no cover or no API key. */
enum grid_meta_state grid_cover_get(const char *title,
      char *path, size_t path_len);

/* Runs queued lookups. Returns true when any entry changed. */
bool grid_cover_poll(void);

/* The API key was just entered: use it. */
void grid_cover_reload_key(void);

/* Hero banner (1920x620 or 1600x650) from SteamGridDB for a game title,
 * for the background. Queues a lookup if it isn't cached and moves it to
 * the front of the queue; on GRID_META_FOUND, path is the cached image. */
enum grid_meta_state grid_cover_hero_get(const char *title,
      char *path, size_t path_len);

/* The downloaded hero image for a title, if there is one. Unlike
 * grid_cover_hero_get, never queues or reorders anything. */
bool grid_cover_hero_peek(const char *title, char *path, size_t path_len);

/* Queues a hero lookup for a title (no-op if known), so heroes download
 * along with the covers. */
void grid_cover_hero_queue(const char *title);

/* Called with each hero image's path right after it is downloaded. */
void grid_cover_hero_set_download_cb(void (*cb)(const char *path));

/* Runs queued hero lookups (the most recently asked-for first). */
void grid_cover_hero_poll(void);

/* Queues icon downloads for a title (no-op if known): its 3 best rated
 * static PNG icons that are at least 256x256 and fully opaque, saved
 * scaled to 256x256, for profile pictures. */
void grid_cover_icons_queue(const char *title);

/* Runs queued icon downloads, one game at a time. */
void grid_cover_icons_poll(void);

/* All downloaded icons, across games: how many, and the image path of
 * one. */
size_t grid_cover_icon_count(void);
bool grid_cover_icon_get(size_t idx, char *path, size_t len);

/* Choosing a game's art by hand (game menu). */
enum grid_choice_kind
{
   GRID_CHOICE_COVER = 0, /* portrait cover */
   GRID_CHOICE_HERO       /* background banner */
};

enum grid_choice_apply
{
   GRID_CHOICE_APPLY_IDLE = 0,
   GRID_CHOICE_APPLY_WORKING,
   GRID_CHOICE_APPLY_DONE,
   GRID_CHOICE_APPLY_FAILED
};

/* Lists every SteamGridDB cover or hero for a title (best rated first)
 * and downloads their previews. State: PENDING while loading, FOUND with
 * a list, UNKNOWN if there is none (or no key). */
void grid_cover_choices_open(const char *title, enum grid_choice_kind kind);
enum grid_meta_state grid_cover_choices_state(void);
size_t grid_cover_choices_count(void);
/* A choice's cached preview image, or "" while it downloads. */
const char *grid_cover_choice_thumb(size_t idx);

/* Which choice is the title's current cover or hero, or -1 if none of
 * them (or the list isn't loaded). */
int grid_cover_choice_current(void);

/* Downloads a choice and makes it the title's cover or hero. */
void grid_cover_choose(size_t idx);
enum grid_choice_apply grid_cover_choose_state(void);

/* Runs preview downloads. Returns true when anything changed. */
bool grid_cover_choices_poll(void);

RETRO_END_DECLS

#endif
