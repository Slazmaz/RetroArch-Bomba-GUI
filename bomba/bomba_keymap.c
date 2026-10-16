/* Bomba: the on-screen keyboard's keys, read from the system's keyboard
 * layouts so it matches the player's own keyboard. Windows only;
 * elsewhere (or for keys a layout leaves empty) a US layout. Kept apart
 * from the grid so windows.h stays out of it. */

#include <string.h>

#include <compat/strl.h>

#include "bomba_keymap.h"

/* US layout: the fallback. */
static const char *bomba_keymap_us[BOMBA_KEYMAP_KEYS][2] = {
   {"1","!"},{"2","@"},{"3","#"},{"4","$"},{"5","%"},{"6","^"},
   {"7","&"},{"8","*"},{"9","("},{"0",")"},{"-","_"},
   {"q","Q"},{"w","W"},{"e","E"},{"r","R"},{"t","T"},{"y","Y"},
   {"u","U"},{"i","I"},{"o","O"},{"p","P"},{"[","{"},
   {"a","A"},{"s","S"},{"d","D"},{"f","F"},{"g","G"},{"h","H"},
   {"j","J"},{"k","K"},{"l","L"},{";",":"},{"'","\""},
   {"z","Z"},{"x","X"},{"c","C"},{"v","V"},{"b","B"},{"n","N"},
   {"m","M"},{",","<"},{".",">"},{"/","?"},{"\\","|"}
};

static void bomba_keymap_fallback(bomba_key_t *keys)
{
   unsigned i;
   for (i = 0; i < BOMBA_KEYMAP_KEYS; i++)
   {
      strlcpy(keys[i].base,  bomba_keymap_us[i][0], sizeof(keys[i].base));
      strlcpy(keys[i].shift, bomba_keymap_us[i][1], sizeof(keys[i].shift));
   }
}

#ifdef _WIN32
#include <windows.h>

#define BOMBA_KEYMAP_MAX_LAYOUTS 16

/* The keys by scan code, row by row: the number row from 1, the letter
 * rows from Q, A and Z (the bottom one ending with the key left of Enter
 * on ISO keyboards, backslash on US ones). */
static const unsigned bomba_keymap_scan[BOMBA_KEYMAP_KEYS] = {
   0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
   0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a,
   0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
   0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x2b
};

static unsigned bomba_keymap_layouts(HKL *list)
{
   int n = GetKeyboardLayoutList(BOMBA_KEYMAP_MAX_LAYOUTS, list);
   if (n <= 0)
   {
      list[0] = GetKeyboardLayout(0);
      n       = 1;
   }
   return (unsigned)n;
}

unsigned bomba_keymap_count(void)
{
   HKL list[BOMBA_KEYMAP_MAX_LAYOUTS];
   return bomba_keymap_layouts(list);
}

unsigned bomba_keymap_current(void)
{
   HKL list[BOMBA_KEYMAP_MAX_LAYOUTS];
   HKL cur    = GetKeyboardLayout(0);
   unsigned i, n = bomba_keymap_layouts(list);

   for (i = 0; i < n; i++)
      if (list[i] == cur)
         return i;
   return 0;
}

/* The key's character with or without shift, as UTF-8 ("" if none). A
 * dead key (´, ¨) gives its accent. */
static void bomba_keymap_char(HKL hkl, UINT scan, bool shift,
      char *s, size_t len)
{
   BYTE state[256];
   WCHAR w[8];
   UINT vk = MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, hkl);
   int n;

   s[0] = '\0';
   if (!vk)
      return;
   memset(state, 0, sizeof(state));
   if (shift)
      state[VK_SHIFT] = 0x80;

   /* Flag 4: leave the keyboard's state (a pending dead key) alone. */
   n = ToUnicodeEx(vk, scan, state, w, 8, 4, hkl);
   if (n < 0)
      n = 1;
   if (n <= 0 || w[0] < 0x20)
      return;
   n = WideCharToMultiByte(CP_UTF8, 0, w, n, s, (int)len - 1, NULL, NULL);
   s[n > 0 ? n : 0] = '\0';
}

void bomba_keymap_get(unsigned idx, bomba_key_t *keys)
{
   HKL list[BOMBA_KEYMAP_MAX_LAYOUTS];
   unsigned i, n = bomba_keymap_layouts(list);
   HKL hkl       = list[idx % n];

   bomba_keymap_fallback(keys);
   for (i = 0; i < BOMBA_KEYMAP_KEYS; i++)
   {
      char base[8], shift[8];
      bomba_keymap_char(hkl, bomba_keymap_scan[i], false, base, sizeof(base));
      bomba_keymap_char(hkl, bomba_keymap_scan[i], true, shift, sizeof(shift));
      if (!base[0])
         continue;
      strlcpy(keys[i].base, base, sizeof(keys[i].base));
      strlcpy(keys[i].shift, shift[0] ? shift : base, sizeof(keys[i].shift));
   }
}

#else

unsigned bomba_keymap_count(void)
{
   return 1;
}

unsigned bomba_keymap_current(void)
{
   return 0;
}

void bomba_keymap_get(unsigned idx, bomba_key_t *keys)
{
   bomba_keymap_fallback(keys);
}

#endif
