#ifndef __BOMBA_TEXT_INPUT_H
#define __BOMBA_TEXT_INPUT_H

/* A core asking the player for text (a launcher core passing on a game's
 * keyboard): RetroArch opens the menu with only the grid's on-screen
 * keyboard over the game. Called from retro_run; returns true if the
 * keyboard will show. done is called once, on RetroArch's main thread,
 * with the text typed (UTF-8), or NULL if cancelled; the menu closes
 * after. The strings are copied. */
#define RETRO_ENVIRONMENT_BOMBA_TEXT_INPUT (0x0B0A | 0x20000) /* PRIVATE */

struct bomba_text_input
{
   const char *label;    /* what is asked, UTF-8; NULL none */
   const char *initial;  /* text to start from, UTF-8; NULL none */
   void (*done)(const char *text);
};

#endif
