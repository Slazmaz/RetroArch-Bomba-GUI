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

#include "../gfx_display.h"
#include "../gfx_widgets.h"

#include "../../cheevos/cheevos.h"
#include "../../bomba/grid_home.h"

#define CHEEVO_NOTIFICATION_DURATION      4000

#define CHEEVO_QUEUE_SIZE 8

/* Bomba look: a compact card at the top centre of the screen,
 * on the same translucent black as the menus' overlays. Sizes are in the
 * grid's reference pixels (screen height / 423). */
#define POPUP_REF(px)          ((float)(px) * (float)video_height / 423.0f)
#define POPUP_MARGIN           10   /* from the top of the screen */
#define POPUP_PAD              5    /* around the badge */
#define POPUP_BADGE            22
#define POPUP_GAP              8    /* badge to text */
#define POPUP_PAD_RIGHT        12
#define POPUP_RADIUS           5
#define POPUP_TITLE_CAP        5.5f
#define POPUP_SUBTITLE_CAP     7.0f
#define POPUP_MAX_WIDTH        0.6f /* of the screen width */

#define POPUP_COL_BACKDROP     0x000000c0
#define POPUP_COL_TITLE        0xb4b4b9ff
#define POPUP_COL_SUBTITLE     0xecececff

typedef struct cheevo_popup
{
   char* title;
   char* subtitle;
   char* badge_name;
   uintptr_t badge;
   retro_time_t badge_retry;
   enum gfx_widgets_icon icon; /* shown when there is no badge;
                                  MENU_WIDGETS_ICON_LAST = text only */
} cheevo_popup;

struct gfx_widget_achievement_popup_state
{
#ifdef HAVE_THREADS
   slock_t* queue_lock;
#endif
   cheevo_popup queue[CHEEVO_QUEUE_SIZE]; /* ptr alignment */
   const dispgfx_widget_t* dispwidget_ptr;
   int queue_read_index;
   int queue_write_index;
   float timer;            /* For delay of CHEEVO_NOTIFICATION_DURATION before dismissing */
   float slide_v;          /* 0 = hidden above the screen, 1 = shown; also the fade */
};

typedef struct gfx_widget_achievement_popup_state gfx_widget_achievement_popup_state_t;

static gfx_widget_achievement_popup_state_t p_w_achievement_popup_st;

/* Space the card took at the top of the screen last frame, so the message
 * queue can stack below it. */
static float p_w_achievement_popup_reserved;

/* Forward declarations */
static void gfx_widget_achievement_popup_start(
   gfx_widget_achievement_popup_state_t* state);
static void gfx_widget_achievement_popup_free_current(
   gfx_widget_achievement_popup_state_t* state);

static bool gfx_widget_achievement_popup_init(gfx_display_t* p_disp,
   gfx_animation_t* p_anim, bool video_is_threaded, bool fullscreen)
{
   gfx_widget_achievement_popup_state_t* state = &p_w_achievement_popup_st;
   memset(state, 0, sizeof(*state));
   state->dispwidget_ptr = (const dispgfx_widget_t*)
      dispwidget_get_ptr();

   state->queue_read_index = -1;

   return true;
}

static void gfx_widget_achievement_popup_free_all(gfx_widget_achievement_popup_state_t* state)
{
   if (state->queue_read_index >= 0)
   {
#ifdef HAVE_THREADS
      slock_lock(state->queue_lock);
#endif
      while (state->queue[state->queue_read_index].title)
         gfx_widget_achievement_popup_free_current(state);
#ifdef HAVE_THREADS
      slock_unlock(state->queue_lock);
#endif
   }
}

static void gfx_widget_achievement_popup_free(void)
{
   gfx_widget_achievement_popup_state_t* state = &p_w_achievement_popup_st;

   gfx_widget_achievement_popup_free_all(state);

#ifdef HAVE_THREADS
   slock_free(state->queue_lock);
   state->queue_lock = NULL;
#endif
   state->dispwidget_ptr = NULL;
}

static void gfx_widget_achievement_popup_context_destroy(void)
{
   gfx_widget_achievement_popup_state_t* state = &p_w_achievement_popup_st;

   gfx_widget_achievement_popup_free_all(state);
}

/* rgba with its alpha scaled by a (0..1). */
static uint32_t gfx_widget_achievement_popup_fade(uint32_t rgba, float a)
{
   return (rgba & 0xffffff00) | (uint32_t)((rgba & 0xff) * a + 0.5f);
}

static void gfx_widget_achievement_popup_frame(void* data, void* userdata)
{
   gfx_widget_achievement_popup_state_t* state = &p_w_achievement_popup_st;

   /* if there's nothing in the queue, just bail */
   if (state->queue_read_index < 0
      || !state->queue[state->queue_read_index].title)
   {
      p_w_achievement_popup_reserved = 0.0f;
      return;
   }

#ifdef HAVE_THREADS
   slock_lock(state->queue_lock);
#endif

   {
      float white[16] = {
         1.00, 1.00, 1.00, 1.00,
         1.00, 1.00, 1.00, 1.00,
         1.00, 1.00, 1.00, 1.00,
         1.00, 1.00, 1.00, 1.00,
      };
      const video_frame_info_t* video_info = (const video_frame_info_t*)data;
      const unsigned video_width           = video_info->width;
      const unsigned video_height          = video_info->height;
      gfx_display_t* p_disp                = (gfx_display_t*)video_info->disp_userdata;
      gfx_display_ctx_driver_t* dispctx    = p_disp->dispctx;
      dispgfx_widget_t* p_dispwidget       = (dispgfx_widget_t*)userdata;
      cheevo_popup* popup                  = &state->queue[state->queue_read_index];
      const char* subtitle                 = popup->subtitle ? popup->subtitle : "";

      /* Ease the fade in a little faster than the slide. */
      float fade      = state->slide_v < 0.5f ? state->slide_v * 2.0f : 1.0f;
      float pad       = POPUP_REF(POPUP_PAD);
      float badge     = POPUP_REF(POPUP_BADGE);
      float h         = badge + 2.0f * pad;
      bool has_image  = popup->badge || popup->badge_name
         || (popup->icon < MENU_WIDGETS_ICON_LAST
            && p_dispwidget->gfx_widgets_icons_textures[popup->icon]);
      float text_x    = has_image
         ? pad + badge + POPUP_REF(POPUP_GAP)
         : POPUP_REF(POPUP_PAD_RIGHT);
      float text_w    = MAX(
            grid_overlay_text_width(false, popup->title,
               video_height, POPUP_TITLE_CAP),
            grid_overlay_text_width(true, subtitle,
               video_height, POPUP_SUBTITLE_CAP));
      float max_w     = video_width * POPUP_MAX_WIDTH;
      float w         = MIN(text_x + text_w + POPUP_REF(POPUP_PAD_RIGHT), max_w);
      float x         = floorf((video_width - w) / 2.0f);
      float y         = -h + (h + POPUP_REF(POPUP_MARGIN)) * state->slide_v;
      bool clip       = text_x + text_w + POPUP_REF(POPUP_PAD_RIGHT) > max_w;

      p_w_achievement_popup_reserved = (h + POPUP_REF(POPUP_MARGIN)) * state->slide_v;

      /* badge wasn't ready, periodically see if it's become available */
      if (!popup->badge && popup->badge_name)
      {
         const retro_time_t next_try = popup->badge_retry;
         const retro_time_t now      = cpu_features_get_time_usec();
         if (next_try == 0 || now > next_try)
         {
            /* try again in 250ms */
            popup->badge_retry = now + 250000;
            popup->badge       = rcheevos_get_badge_texture(popup->badge_name, false, false);
         }
      }

      if (dispctx && dispctx->blend_begin)
         dispctx->blend_begin(video_info->userdata);

      /* Backdrop */
      grid_overlay_shape(p_disp, video_info->userdata,
            video_width, video_height, x, y, w, h,
            POPUP_REF(POPUP_RADIUS),
            gfx_widget_achievement_popup_fade(POPUP_COL_BACKDROP, fade));

      /* Badge, or the popup's icon */
      gfx_display_set_alpha(white, fade);
      if (popup->badge)
         gfx_widgets_draw_icon(video_info->userdata, p_disp,
               video_width, video_height,
               (unsigned)badge, (unsigned)badge, popup->badge,
               x + pad, y + pad,
               0.0f, 1.0f, 0.0f, white);
      else if (popup->icon < MENU_WIDGETS_ICON_LAST
            && p_dispwidget->gfx_widgets_icons_textures[popup->icon])
      {
         /* Icons are drawn smaller than badges, centred in their slot. */
         float icon = badge * 0.8f;
         gfx_widgets_draw_icon(video_info->userdata, p_disp,
               video_width, video_height,
               (unsigned)icon, (unsigned)icon,
               p_dispwidget->gfx_widgets_icons_textures[popup->icon],
               x + pad + (badge - icon) / 2.0f,
               y + pad + (badge - icon) / 2.0f,
               0.0f, 1.0f, 0.0f, white);
      }

      if (clip)
         gfx_display_scissor_begin(p_disp, video_info->userdata,
               video_width, video_height,
               (int)(x + text_x), (int)y,
               (unsigned)(w - text_x - pad), (unsigned)h);

      /* Title (what happened), then subtitle (which one) */
      grid_overlay_text(false, popup->title,
            x + text_x, y + pad + badge * 0.27f,
            video_width, video_height,
            gfx_widget_achievement_popup_fade(POPUP_COL_TITLE, fade),
            TEXT_ALIGN_LEFT, POPUP_TITLE_CAP);
      grid_overlay_text(true, subtitle,
            x + text_x, y + pad + badge * 0.72f,
            video_width, video_height,
            gfx_widget_achievement_popup_fade(POPUP_COL_SUBTITLE, fade),
            TEXT_ALIGN_LEFT, POPUP_SUBTITLE_CAP);
      grid_overlay_flush(video_width, video_height);

      if (clip && dispctx && dispctx->scissor_end)
         dispctx->scissor_end(video_info->userdata,
               video_width, video_height);

      if (dispctx && dispctx->blend_end)
         dispctx->blend_end(video_info->userdata);
   }

#ifdef HAVE_THREADS
   slock_unlock(state->queue_lock);
#endif
}

static void gfx_widget_achievement_popup_free_current(
   gfx_widget_achievement_popup_state_t* state)
{
   if (state->queue[state->queue_read_index].title)
   {
      free(state->queue[state->queue_read_index].title);
      state->queue[state->queue_read_index].title = NULL;
   }

   if (state->queue[state->queue_read_index].subtitle)
   {
      free(state->queue[state->queue_read_index].subtitle);
      state->queue[state->queue_read_index].subtitle = NULL;
   }

   if (state->queue[state->queue_read_index].badge_name)
   {
      free(state->queue[state->queue_read_index].badge_name);
      state->queue[state->queue_read_index].badge_name = NULL;
   }

   if (state->queue[state->queue_read_index].badge)
   {
      video_driver_texture_unload(&state->queue[state->queue_read_index].badge);
      state->queue[state->queue_read_index].badge = 0;
   }

   state->queue_read_index = (state->queue_read_index + 1) % ARRAY_SIZE(state->queue);
}

static void gfx_widget_achievement_popup_next(void* userdata)
{
   gfx_widget_achievement_popup_state_t* state = &p_w_achievement_popup_st;

#ifdef HAVE_THREADS
   slock_lock(state->queue_lock);
#endif

   if (state->queue_read_index >= 0)
   {
      if (state->queue[state->queue_read_index].title)
         gfx_widget_achievement_popup_free_current(state);

      /* start the next popup (if present) */
      if (state->queue[state->queue_read_index].title)
         gfx_widget_achievement_popup_start(state);
   }

#ifdef HAVE_THREADS
   slock_unlock(state->queue_lock);
#endif
}

static void gfx_widget_achievement_popup_dismiss(void* userdata)
{
   gfx_animation_ctx_entry_t entry;
   gfx_widget_achievement_popup_state_t* state = &p_w_achievement_popup_st;
   const dispgfx_widget_t* p_dispwidget = (const dispgfx_widget_t*)
      state->dispwidget_ptr;

   /* Slide back up and fade out */
   entry.cb             = gfx_widget_achievement_popup_next;
   entry.duration       = MSG_QUEUE_ANIMATION_DURATION;
   entry.easing_enum    = EASING_IN_QUAD;
   entry.subject        = &state->slide_v;
   entry.tag            = p_dispwidget->gfx_widgets_generic_tag;
   entry.target_value   = 0.0f;
   entry.userdata       = NULL;

   gfx_animation_push(&entry);
}

static void gfx_widget_achievement_popup_hold(void* userdata)
{
   gfx_timer_ctx_entry_t timer;
   gfx_widget_achievement_popup_state_t* state = &p_w_achievement_popup_st;

   timer.cb       = gfx_widget_achievement_popup_dismiss;
   timer.duration = CHEEVO_NOTIFICATION_DURATION;
   timer.userdata = NULL;

   gfx_animation_timer_start(&state->timer, &timer);
}

/* Popups always sit at the top centre (see POPUP_*), so the anchor and
 * padding settings are ignored. Kept for the settings menu to call. */
void gfx_widgets_update_cheevos_appearance(void)
{
}

static void gfx_widget_achievement_popup_start(
   gfx_widget_achievement_popup_state_t* state)
{
   gfx_animation_ctx_entry_t anim_slide;
   const dispgfx_widget_t* p_dispwidget = (const dispgfx_widget_t*)
      state->dispwidget_ptr;

   state->slide_v = 0.0f;

   /* Slide down onto the screen, fading in */
   anim_slide.cb = gfx_widget_achievement_popup_hold;
   anim_slide.duration = MSG_QUEUE_ANIMATION_DURATION;
   anim_slide.easing_enum = EASING_OUT_QUAD;
   anim_slide.subject = &state->slide_v;
   anim_slide.tag = p_dispwidget->gfx_widgets_generic_tag;
   anim_slide.target_value = 1.0f;
   anim_slide.userdata = NULL;

   gfx_animation_push(&anim_slide);
}

void gfx_widgets_push_achievement_icon(const char* title, const char* subtitle,
      const char* badge, enum gfx_widgets_icon icon)
{
   gfx_widget_achievement_popup_state_t* state = &p_w_achievement_popup_st;
   int start_notification = 1;

   /* important - this must be done outside the lock because it has the potential to need to
    * lock the video thread, which may be waiting for the popup queue lock to render popups */
   uintptr_t badge_id = badge ? rcheevos_get_badge_texture(badge, false, true) : 0;

   if (state->queue_read_index < 0)
   {
      /* queue uninitialized */
      memset(&state->queue, 0, sizeof(state->queue));
      state->queue_read_index = 0;

#ifdef HAVE_THREADS
      state->queue_lock = slock_new();
#endif
   }

#ifdef HAVE_THREADS
   slock_lock(state->queue_lock);
#endif

   if (state->queue_write_index == state->queue_read_index)
   {
      if (state->queue[state->queue_write_index].title)
      {
         /* queue full */
#ifdef HAVE_THREADS
         slock_unlock(state->queue_lock);
#endif
         if (badge_id)
            video_driver_texture_unload(&badge_id);
         return;
      }

      /* queue empty */
   }
   else
      start_notification = 0; /* notification already being displayed */

   state->queue[state->queue_write_index].badge = badge_id;
   state->queue[state->queue_write_index].title = strdup(title ? title : "");
   state->queue[state->queue_write_index].subtitle = strdup(subtitle ? subtitle : "");
   state->queue[state->queue_write_index].badge_name = (badge_id || !badge) ? NULL : strdup(badge);
   state->queue[state->queue_write_index].badge_retry = 0;
   state->queue[state->queue_write_index].icon = icon;

   state->queue_write_index = (state->queue_write_index + 1) % ARRAY_SIZE(state->queue);

   if (start_notification)
      gfx_widget_achievement_popup_start(state);

#ifdef HAVE_THREADS
   slock_unlock(state->queue_lock);
#endif
}

float gfx_widget_achievement_popup_reserved(void)
{
   return p_w_achievement_popup_reserved;
}

void gfx_widgets_push_achievement(const char* title, const char* subtitle, const char* badge)
{
   gfx_widgets_push_achievement_icon(title, subtitle, badge,
         MENU_WIDGETS_ICON_ACHIEVEMENT);
}

const gfx_widget_t gfx_widget_achievement_popup = {
   &gfx_widget_achievement_popup_init,
   &gfx_widget_achievement_popup_free,
   NULL, /* context_reset*/
   &gfx_widget_achievement_popup_context_destroy,
   NULL, /* layout */
   NULL, /* iterate */
   &gfx_widget_achievement_popup_frame
};
