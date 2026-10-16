/* Bomba: the system clipboard, for copy and paste in the grid's text
 * fields. Windows only; elsewhere the clipboard stays empty. Kept apart
 * from the grid so windows.h stays out of it. */

#include <stdlib.h>
#include <string.h>

#include "bomba_clipboard.h"

#ifdef _WIN32
#include <windows.h>

bool bomba_clipboard_set(const char *text, size_t len)
{
   int wlen;
   HGLOBAL mem;
   wchar_t *w;
   bool ok = false;

   wlen = MultiByteToWideChar(CP_UTF8, 0, text, (int)len, NULL, 0);
   if (wlen < 0 || !(mem = GlobalAlloc(GMEM_MOVEABLE,
               (wlen + 1) * sizeof(wchar_t))))
      return false;

   w = (wchar_t*)GlobalLock(mem);
   MultiByteToWideChar(CP_UTF8, 0, text, (int)len, w, wlen);
   w[wlen] = 0;
   GlobalUnlock(mem);

   if (OpenClipboard(NULL))
   {
      EmptyClipboard();
      ok = SetClipboardData(CF_UNICODETEXT, mem) != NULL;
      CloseClipboard();
   }
   /* The clipboard owns it once set. */
   if (!ok)
      GlobalFree(mem);
   return ok;
}

char *bomba_clipboard_get(void)
{
   HANDLE mem;
   const wchar_t *w;
   char *s = NULL;

   if (!OpenClipboard(NULL))
      return NULL;

   if (     (mem = GetClipboardData(CF_UNICODETEXT))
         && (w = (const wchar_t*)GlobalLock(mem)))
   {
      int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
      if (len > 0 && (s = (char*)malloc(len)))
         WideCharToMultiByte(CP_UTF8, 0, w, -1, s, len, NULL, NULL);
      GlobalUnlock(mem);
   }

   CloseClipboard();
   return s;
}

#else

bool bomba_clipboard_set(const char *text, size_t len)
{
   return false;
}

char *bomba_clipboard_get(void)
{
   return NULL;
}

#endif
