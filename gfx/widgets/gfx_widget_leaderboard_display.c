/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2014-2017 - Jean-André Santoni
 *  Copyright (C) 2015-2018 - Andre Leiradella
 *  Copyright (C) 2018-2020 - natinusala
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <features/features_cpu.h>

#include "../gfx_display.h"
#include "../gfx_widgets.h"

#include "../../cheevos/cheevos.h"
#include "../../bomba/grid_home.h"

#define CHEEVO_LBOARD_ARRAY_SIZE 4
#define CHEEVO_CHALLENGE_ARRAY_SIZE 8

/* Bomba look, in the grid's reference pixels (height / 423) */
#define TRACKER_REF(px)          ((float)(px) * (float)video_height / 423.0f)
#define TRACKER_MARGIN           8
#define TRACKER_SPACING          4
#define TRACKER_HEIGHT           18
#define TRACKER_PAD              6
#define TRACKER_RADIUS           5
#define TRACKER_BADGE            20
#define TRACKER_TEXT_CAP         6.5f
#define TRACKER_SMALL_CAP        5.0f

#define TRACKER_COL_BACKDROP     0x000000c0
#define TRACKER_COL_TEXT         0xecececff
#define TRACKER_COL_DIM          0xb4b4b9ff
#define TRACKER_COL_WARN         0xff8a65ff

#define CHEEVO_PROGRESS_TRACKER_DURATION 2000

struct leaderboard_display_info
{
   unsigned id;
   unsigned width; /* unused: measured each frame in the Bomba font */
   char display[24]; /* should never exceed 12 bytes, but aligns the structure at 32 bytes */
};

struct challenge_display_info
{
   unsigned id;
   uintptr_t image;
};

struct progress_tracker_info
{
   uintptr_t image;
   unsigned width;
   char display[32];
   retro_time_t show_until;
};

#define CHEEVO_LBOARD_FIRST_FIXED_CHAR 0x2C /* ,-./0123456789: */
#define CHEEVO_LBOARD_LAST_FIXED_CHAR 0x3A

/* TODO: rename; this file handles all achievement tracker information, not just leaderboards */
struct gfx_widget_leaderboard_display_state
{
#ifdef HAVE_THREADS
   slock_t* array_lock;
#endif
   const dispgfx_widget_t *dispwidget_ptr;
   struct leaderboard_display_info tracker_info[CHEEVO_LBOARD_ARRAY_SIZE];
   struct challenge_display_info challenge_info[CHEEVO_CHALLENGE_ARRAY_SIZE];
   struct progress_tracker_info progress_tracker;
   unsigned tracker_count;
   unsigned challenge_count;
   uint16_t char_width[CHEEVO_LBOARD_LAST_FIXED_CHAR - CHEEVO_LBOARD_FIRST_FIXED_CHAR + 1];
   uint16_t fixed_char_width;
   unsigned char_width_height; /* screen height char_width was measured at */
   uint16_t loading;
   bool disconnected;
};

typedef struct gfx_widget_leaderboard_display_state gfx_widget_leaderboard_display_state_t;

static gfx_widget_leaderboard_display_state_t p_w_leaderboard_display_st;

static bool gfx_widget_leaderboard_display_init(
      gfx_display_t *p_disp,
      gfx_animation_t *p_anim,
      bool video_is_threaded, bool fullscreen)
{
   gfx_widget_leaderboard_display_state_t *state =
      &p_w_leaderboard_display_st;
   memset(state, 0, sizeof(*state));
   state->dispwidget_ptr   = (const dispgfx_widget_t*)
      dispwidget_get_ptr();

   return true;
}

static void gfx_widget_leaderboard_display_free(void)
{
   gfx_widget_leaderboard_display_state_t *state = &p_w_leaderboard_display_st;

   state->tracker_count   = 0;
   state->challenge_count = 0;
#ifdef HAVE_THREADS
   slock_free(state->array_lock);
   state->array_lock      = NULL;
#endif
   state->dispwidget_ptr  = NULL;
}

static void gfx_widget_leaderboard_display_context_destroy(void)
{
   gfx_widget_leaderboard_display_state_t *state = &p_w_leaderboard_display_st;
   state->tracker_count   = 0;
   state->challenge_count = 0;
}

/* Width of a tracker value in the Bomba font, with digits all as wide
 * as the widest one so a running timer doesn't jitter. */
static float gfx_widget_leaderboard_display_value_width(
      gfx_widget_leaderboard_display_state_t *state, const char *value,
      unsigned video_height)
{
   float width = 0.0f;

   /* Measure the fixed characters once per screen height. */
   if (state->char_width_height != video_height)
   {
      char buffer[2] = "0";
      int j;
      state->fixed_char_width = 0;
      for (j = 0; j < (int)ARRAY_SIZE(state->char_width); ++j)
      {
         buffer[0] = (char)(j + CHEEVO_LBOARD_FIRST_FIXED_CHAR);
         state->char_width[j] = (uint16_t)(grid_overlay_text_width(true,
                  buffer, video_height, TRACKER_TEXT_CAP) + 0.5f);
         if (buffer[0] >= '0' && buffer[0] <= '9'
               && state->char_width[j] > state->fixed_char_width)
            state->fixed_char_width = state->char_width[j];
      }
      state->char_width_height = video_height;
   }

   while (*value)
   {
      const char c = *value++;
      if (c >= '0' && c <= '9')
         width += state->fixed_char_width;
      else if (c >= CHEEVO_LBOARD_FIRST_FIXED_CHAR && c <= CHEEVO_LBOARD_LAST_FIXED_CHAR)
         width += state->char_width[c - CHEEVO_LBOARD_FIRST_FIXED_CHAR];
      else
      {
         char buffer[2] = "0";
         buffer[0] = c;
         width += grid_overlay_text_width(true, buffer, video_height,
               TRACKER_TEXT_CAP);
      }
   }

   return width;
}

/* Icon texture or badge, faded with the rest (default icon if none). */
static void gfx_widget_leaderboard_display_draw_image(
      const video_frame_info_t *video_info, gfx_display_t *p_disp,
      dispgfx_widget_t *p_dispwidget, uintptr_t image,
      enum gfx_widgets_icon icon, float x, float y, float size)
{
   static float pure_white[16] = {
      1.00, 1.00, 1.00, 1.00,
      1.00, 1.00, 1.00, 1.00,
      1.00, 1.00, 1.00, 1.00,
      1.00, 1.00, 1.00, 1.00,
   };
   uintptr_t tex = image ? image
      : p_dispwidget->gfx_widgets_icons_textures[icon];

   if (!tex)
      return;

   gfx_widgets_draw_icon(video_info->userdata, p_disp,
         video_info->width, video_info->height,
         (unsigned)size, (unsigned)size, tex, x, y,
         0.0f, /* rad */
         1.0f, /* cos(rad)   = cos(0)  = 1.0f */
         0.0f, /* sine(rad)  = sine(0) = 0.0f */
         pure_white);
}

/* Everything stacks up from the bottom right corner as small pills on the
 * menus' translucent black: timers (bottom), challenge badges, progress,
 * then the RetroAchievements connection status. */
static void gfx_widget_leaderboard_display_frame(void* data, void* userdata)
{
   gfx_widget_leaderboard_display_state_t *state = &p_w_leaderboard_display_st;

   /* if there's nothing to display, just bail */
   if (state->tracker_count == 0 &&
       state->challenge_count == 0 &&
       state->progress_tracker.show_until == 0 &&
       !state->loading &&
       !state->disconnected)
      return;

#ifdef HAVE_THREADS
   slock_lock(state->array_lock);
#endif

   {
      unsigned i;
      dispgfx_widget_t         *p_dispwidget = (dispgfx_widget_t*)userdata;
      const video_frame_info_t *video_info   = (const video_frame_info_t*)data;
      gfx_display_t *p_disp                  = (gfx_display_t*)video_info->disp_userdata;
      gfx_display_ctx_driver_t *dispctx      = p_disp->dispctx;
      const unsigned video_width             = video_info->width;
      const unsigned video_height            = video_info->height;
      const float margin                     = TRACKER_REF(TRACKER_MARGIN);
      const float spacing                    = TRACKER_REF(TRACKER_SPACING);
      const float pill_h                     = TRACKER_REF(TRACKER_HEIGHT);
      const float pad                        = TRACKER_REF(TRACKER_PAD);
      const float radius                     = TRACKER_REF(TRACKER_RADIUS);
      float x;
      float y                                = video_height - margin;
      char buffer[2] = "0";
      const char* ptr;

      if (dispctx && dispctx->blend_begin)
         dispctx->blend_begin(video_info->userdata);

      /* Timers / scores: the value in even-width digits */
      for (i = 0; i < state->tracker_count; ++i)
      {
         float char_x;
         float value_w = gfx_widget_leaderboard_display_value_width(state,
               state->tracker_info[i].display, video_height);
         float w       = value_w + pad * 2.0f;

         x  = video_width - margin - w;
         y -= pill_h;

         grid_overlay_shape(p_disp, video_info->userdata,
               video_width, video_height, x, y, w, pill_h,
               radius, TRACKER_COL_BACKDROP);

         char_x = x + pad;
         ptr    = state->tracker_info[i].display;
         while (*ptr)
         {
            float next_char_x;
            const char c = *ptr++;
            buffer[0]    = c;

            if (c >= '0' && c <= '9')
            {
               /* centre each digit in the widest digit's slot */
               unsigned char_width = state->char_width[c - CHEEVO_LBOARD_FIRST_FIXED_CHAR];
               next_char_x = char_x + state->fixed_char_width;
               char_x     += (float)(state->fixed_char_width - char_width) / 2.0f;
            }
            else if (c >= CHEEVO_LBOARD_FIRST_FIXED_CHAR && c <= CHEEVO_LBOARD_LAST_FIXED_CHAR)
               next_char_x = char_x + state->char_width[c - CHEEVO_LBOARD_FIRST_FIXED_CHAR];
            else
               next_char_x = char_x + grid_overlay_text_width(true, buffer,
                     video_height, TRACKER_TEXT_CAP);

            grid_overlay_text(true, buffer, char_x, y + pill_h / 2.0f,
                  video_width, video_height, TRACKER_COL_TEXT,
                  TEXT_ALIGN_LEFT, TRACKER_TEXT_CAP);

            char_x = next_char_x;
         }

         y -= spacing;
      }

      /* Challenges in progress: their badges in a row */
      if (state->challenge_count)
      {
         const float badge = TRACKER_REF(TRACKER_BADGE);
         const float box   = badge + pad;

         x  = video_width - margin;
         y -= box;

         for (i = 0; i < state->challenge_count; ++i)
         {
            x -= box;

            grid_overlay_shape(p_disp, video_info->userdata,
                  video_width, video_height, x, y, box, box,
                  radius, TRACKER_COL_BACKDROP);

            gfx_widget_leaderboard_display_draw_image(video_info, p_disp,
                  p_dispwidget, state->challenge_info[i].image,
                  MENU_WIDGETS_ICON_ACHIEVEMENT,
                  x + pad / 2.0f, y + pad / 2.0f, badge);

            x -= spacing;
         }

         y -= spacing;
      }

      /* Progress towards an achievement, briefly: badge + "3/10" */
      if (state->progress_tracker.show_until)
      {
         retro_time_t now = cpu_features_get_time_usec();
         if (now >= state->progress_tracker.show_until)
         {
            gfx_widget_set_achievement_progress(NULL, NULL);
         }
         else
         {
            const float badge = pill_h - pad;
            float text_w      = grid_overlay_text_width(true,
                  state->progress_tracker.display, video_height,
                  TRACKER_TEXT_CAP);
            float w           = pad / 2.0f + badge + pad * 0.8f + text_w + pad * 1.4f;

            x  = video_width - margin - w;
            y -= pill_h;

            grid_overlay_shape(p_disp, video_info->userdata,
                  video_width, video_height, x, y, w, pill_h,
                  radius, TRACKER_COL_BACKDROP);

            gfx_widget_leaderboard_display_draw_image(video_info, p_disp,
                  p_dispwidget, state->progress_tracker.image,
                  MENU_WIDGETS_ICON_ACHIEVEMENT,
                  x + pad / 2.0f, y + pad / 2.0f, badge);

            grid_overlay_text(true, state->progress_tracker.display,
                  x + pad / 2.0f + badge + pad * 0.8f, y + pill_h / 2.0f,
                  video_width, video_height, TRACKER_COL_TEXT,
                  TEXT_ALIGN_LEFT, TRACKER_TEXT_CAP);

            y -= spacing;
         }
      }

      /* RetroAchievements: loading dots, or disconnected */
      if (state->disconnected || state->loading)
      {
         char loading_buffer[8] = "RA ...";
         const char *status_text = state->disconnected ? "RA offline" : loading_buffer;
         float w;

         if (state->loading)
         {
            const uint16_t loading_shift = 5;
            loading_buffer[((state->loading - 1) >> loading_shift) + 3] = '\0';
            state->loading &= (1 << (loading_shift + 2)) - 1;
            ++state->loading;
         }

         /* sized for the longest text, so the dots don't resize it */
         w  = grid_overlay_text_width(false,
               state->disconnected ? status_text : "RA ...",
               video_height, TRACKER_SMALL_CAP) + pad * 2.4f;
         x  = video_width - margin - w;
         y -= pill_h * 0.8f;

         grid_overlay_shape(p_disp, video_info->userdata,
               video_width, video_height, x, y, w, pill_h * 0.8f,
               radius, TRACKER_COL_BACKDROP);

         grid_overlay_text(false, status_text, x + pad * 1.2f,
               y + pill_h * 0.4f, video_width, video_height,
               state->disconnected ? TRACKER_COL_WARN : TRACKER_COL_DIM,
               TEXT_ALIGN_LEFT, TRACKER_SMALL_CAP);
      }

      grid_overlay_flush(video_width, video_height);

      if (dispctx && dispctx->blend_end)
         dispctx->blend_end(video_info->userdata);
   }

#ifdef HAVE_THREADS
   slock_unlock(state->array_lock);
#endif
}

void gfx_widgets_clear_leaderboard_displays(void)
{
   gfx_widget_leaderboard_display_state_t* state = &p_w_leaderboard_display_st;

#ifdef HAVE_THREADS
   slock_lock(state->array_lock);
#endif

   state->tracker_count = 0;

#ifdef HAVE_THREADS
   slock_unlock(state->array_lock);
#endif
}

void gfx_widgets_set_leaderboard_display(unsigned id, const char* value)
{
   unsigned i;
   gfx_widget_leaderboard_display_state_t *state = &p_w_leaderboard_display_st;

#ifdef HAVE_THREADS
   slock_lock(state->array_lock);
#endif

   for (i = 0; i < state->tracker_count; ++i)
   {
      if (state->tracker_info[i].id == id)
         break;
   }

   if (i < CHEEVO_LBOARD_ARRAY_SIZE)
   {
      if (value == NULL)
      {
         /* hide display */
         if (i < state->tracker_count)
         {
            --state->tracker_count;
            if (i < state->tracker_count)
            {
               memcpy(&state->tracker_info[i], &state->tracker_info[i + 1],
                     (state->tracker_count - i) * sizeof(state->tracker_info[i]));
            }
         }
      }
      else
      {
         /* show or update display */
         if (i == state->tracker_count)
            state->tracker_info[state->tracker_count++].id = id;

         strlcpy(state->tracker_info[i].display, value,
               sizeof(state->tracker_info[i].display));
      }
   }

#ifdef HAVE_THREADS
   slock_unlock(state->array_lock);
#endif
}

void gfx_widgets_clear_challenge_displays(void)
{
   gfx_widget_leaderboard_display_state_t* state = &p_w_leaderboard_display_st;

#ifdef HAVE_THREADS
   slock_lock(state->array_lock);
#endif

   state->challenge_count = 0;

#ifdef HAVE_THREADS
   slock_unlock(state->array_lock);
#endif
}

void gfx_widgets_set_challenge_display(unsigned id, const char* badge)
{
   unsigned i;
   gfx_widget_leaderboard_display_state_t* state = &p_w_leaderboard_display_st;

   /* important - this must be done outside the lock because it has the potential to need to
    * lock the video thread, which may be waiting for the popup queue lock to render popups */
   uintptr_t badge_id     = badge ? rcheevos_get_badge_texture(badge, false, true) : 0;
   uintptr_t old_badge_id = 0;

#ifdef HAVE_THREADS
   slock_lock(state->array_lock);
#endif

   for (i = 0; i < state->challenge_count; ++i)
   {
      if (state->challenge_info[i].id == id)
         break;
   }

   if (i < CHEEVO_CHALLENGE_ARRAY_SIZE)
   {
      if (badge == NULL)
      {
         /* hide indicator */
         if (i < state->challenge_count)
         {
            old_badge_id = state->challenge_info[i].image;

            --state->challenge_count;
            if (i < state->challenge_count)
            {
               memcpy(&state->challenge_info[i], &state->challenge_info[i + 1],
                  (state->challenge_count - i) * sizeof(state->challenge_info[i]));
            }

            state->challenge_info[state->challenge_count].image = 0;
         }
      }
      else
      {
         /* show indicator */
         if (i == state->challenge_count)
         {
            /* new indicator, assign id */
            state->challenge_info[state->challenge_count++].id = id;
         }
         else if (state->challenge_info[i].image)
         {
            /* existing indicator, free old image */
            old_badge_id = state->challenge_info[i].image;
         }

         state->challenge_info[i].image = badge_id;
      }
   }

#ifdef HAVE_THREADS
   slock_unlock(state->array_lock);
#endif

   if (old_badge_id)
      video_driver_texture_unload(&old_badge_id);
}

void gfx_widget_set_achievement_progress(const char* badge, const char* progress)
{
   gfx_widget_leaderboard_display_state_t* state = &p_w_leaderboard_display_st;
   uintptr_t old_badge_id = state->progress_tracker.image;

   if (badge == NULL)
   {
      /* hide indicator */
      state->progress_tracker.image = 0;
      state->progress_tracker.show_until = 0;
   }
   else
   {
      /* show indicator */
      state->progress_tracker.show_until = cpu_features_get_time_usec() + CHEEVO_PROGRESS_TRACKER_DURATION * 1000;
      state->progress_tracker.image = rcheevos_get_badge_texture(badge, true, true);

      snprintf(state->progress_tracker.display, sizeof(state->progress_tracker.display), "%s", progress);
      state->progress_tracker.width = (uint16_t)font_driver_get_message_width(
            state->dispwidget_ptr->gfx_widget_fonts.regular.font,
            progress, 0, 1);
   }

   if (old_badge_id)
      video_driver_texture_unload(&old_badge_id);
}

void gfx_widget_set_cheevos_disconnect(bool value)
{
   gfx_widget_leaderboard_display_state_t* state = &p_w_leaderboard_display_st;
   state->disconnected = value;
}

void gfx_widget_set_cheevos_set_loading(bool value)
{
   gfx_widget_leaderboard_display_state_t* state = &p_w_leaderboard_display_st;
   state->loading = value ? 1 : 0;
}


const gfx_widget_t gfx_widget_leaderboard_display = {
   &gfx_widget_leaderboard_display_init,
   &gfx_widget_leaderboard_display_free,
   NULL, /* context_reset*/
   &gfx_widget_leaderboard_display_context_destroy,
   NULL, /* layout */
   NULL, /* iterate */
   &gfx_widget_leaderboard_display_frame
};
