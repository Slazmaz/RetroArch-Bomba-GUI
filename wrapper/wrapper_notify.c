/* RetroArchWrapper: shows on-screen messages handed over by the launcher.
 *
 * The wrapper sets RA_WRAPPER_MSG before starting RetroArch. Multiple
 * messages are separated by '|'. They are pushed once, shortly after
 * startup, so the video driver and notification widgets are ready.
 */

#include <stdlib.h>
#include <string.h>

#include <string/stdstring.h>

#include "wrapper_notify.h"
#include "../runloop.h"

/* Frames to wait before showing messages (~2s at 60 fps). */
#define WRAPPER_NOTIFY_DELAY_FRAMES 120
/* How long each message stays on screen (~8s at 60 fps). */
#define WRAPPER_NOTIFY_DURATION     480

void wrapper_notify_poll(void)
{
   static unsigned frames = 0;
   static bool done       = false;
   const char *env;
   char buf[1024];
   char *tok;
   char *sep;

   if (done || ++frames < WRAPPER_NOTIFY_DELAY_FRAMES)
      return;

   done = true;
   env  = getenv("RA_WRAPPER_MSG");

   if (string_is_empty(env))
      return;

   strlcpy(buf, env, sizeof(buf));

   for (tok = buf; tok; tok = sep)
   {
      if ((sep = strchr(tok, '|')))
         *sep++ = '\0';

      if (!string_is_empty(tok))
         runloop_msg_queue_push(tok, strlen(tok), 1, WRAPPER_NOTIFY_DURATION,
               false, NULL, MESSAGE_QUEUE_ICON_DEFAULT,
               MESSAGE_QUEUE_CATEGORY_INFO);
   }
}
