/* Launcher "core" for Nintendo Switch games, running them in Eden.
 *
 * Does no emulation. On load it starts a separately installed Eden with
 * the game and ends the content once Eden exits, so RetroArch handles
 * playlists, history and playtime.
 *
 * eden.exe is looked up in order:
 *   1. the path written in <system>/eden.txt (first line)
 *   2. <system>/Eden/eden.exe
 *
 * Eden runs portable: its data (keys, firmware, settings) is in the "user"
 * folder next to eden.exe, shared by all of RetroArch's profiles. Only the
 * game saves, user/nand/user/save, are the profile's: each launch links
 * that folder to <saves>/save (<saves>: the profile's Switch folder).
 *
 * Overlay mode (the Bomba fork's GL driver; it sets the
 * BOMBA_OVERLAY environment variable and the "BombaOverlay"
 * property on its window): Eden and RetroArch look and act as one window,
 * like Steam's overlay.
 *  - While Eden boots, RetroArch shows a loading screen (the core's frame)
 *    and stays over Eden's window until the game runs.
 *  - Eden's borderless fullscreen window is kept exactly under
 *    RetroArch's, which is see-through except for its menu and
 *    notifications.
 *  - Eden has no taskbar or Alt-Tab entry, and hides while RetroArch is
 *    minimized. RetroArch keeps focus; Eden reads controllers in the
 *    background.
 *  - While RetroArch doesn't run the core (menu open, window inactive) the
 *    Eden process is suspended, so the game pauses.
 * Otherwise Eden simply opens in its own window.
 *
 * The game runs in eden-cli.exe, which comes with Eden, rather than
 * eden.exe: it can join multiplayer rooms (see "Multiplayer"), and its
 * Switch keyboard is RetroArch's (see "Switch keyboard").
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <winioctl.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include "../../bomba_text_input.h"

/* The loading screen fades in from black, and out to black before Eden
 * shows, black that then fades off Eden (gl2.c, GL2_BOMBA_FADE_MS). */
#define FADE_MS          500

/* Eden draws frames (see emu_presenting): shown this long after. */
#define READY_AFTER_FRAMES_MS 300
/* No frame count seen yet: shown this long after Eden opened its audio
 * (the game is heard by then; eden-cli's frame count can lag seconds). */
#define READY_AFTER_AUDIO_MS 1000
/* Eden's window taken over anyway after loading this long. */
#define LOADING_MAX_MS   60000
/* Eden's window restyled: RetroArch turns see-through this long after,
 * once the window has drawn again and the loading screen faded out. */
#define SHOW_DELAY_MS    (FADE_MS + 150)
/* No retro_run for this long: RetroArch paused the core; pause Eden. */
#define PAUSE_AFTER_MS   250
/* How often the window arrangement is checked. */
#define ARRANGE_MS       250

typedef LONG (NTAPI *nt_process_fn)(HANDLE);

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;

/* The frame: the loading screen, at the screen's own size. */
static unsigned fb_width = 1280, fb_height = 720;
static HANDLE emu_process;
static DWORD emu_pid;

static struct
{
   bool active;        /* overlay mode */
   bool transparent;   /* RetroArch's window is see-through now */
   bool hidden;        /* Eden hidden while RetroArch is minimized */
   bool topmost;       /* RetroArch's window kept over Eden's while loading */
   bool styled;        /* Eden's window taken over, see-through next */
   HWND ra;            /* RetroArch's window */
   HWND emu;           /* Eden's game window, once it appears */
   HWND popup;         /* Eden's dialog over the game (Switch keyboard) */
   HANDLE emu_thread;  /* Eden's main thread, suspended until RetroArch's
                          window is on top (set by the launch thread) */
   DWORD loading_since;/* tick Eden was let run */
   DWORD audio_seen;   /* tick Eden's audio was first seen, 0 not yet */
   DWORD frames_seen;  /* tick Eden was first seen drawing, 0 not yet */
   DWORD styled_at;    /* tick Eden's window was taken over */
   DWORD last_arrange; /* tick the arrangement was last checked */
   RECT placed;        /* where Eden's window was last put */
   RECT emu_rect;      /* ...and its window rectangle then */

   HANDLE thread;      /* manager thread, see manager_main */
   volatile LONG stop;
   CRITICAL_SECTION lock;
   bool lock_init;
   DWORD last_run;     /* tick of the last retro_run */
   bool suspended;
   nt_process_fn suspend, resume;
} ov;

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */

/* RetroArch's save folder for this game, read at load (the launch thread
 * can't ask). */
static char save_dir_cache[MAX_PATH * 4];

/* Step-by-step log, <saves>/launcher.log, rewritten each launch. */
static FILE *log_file;

static void log_open(void)
{
   const char *save_dir = NULL;
   char path[MAX_PATH * 4];

   if (log_file)
      return;
   if (!env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) || !save_dir)
      return;
   snprintf(path, sizeof(path), "%s\\launcher.log", save_dir);
   log_file = fopen(path, "w");
}

static void log_msg(const char *fmt, ...)
{
   va_list ap;
   if (!log_file)
      return;
   fprintf(log_file, "[%10lu] ", (unsigned long)GetTickCount());
   va_start(ap, fmt);
   vfprintf(log_file, fmt, ap);
   va_end(ap);
   fputc('\n', log_file);
   fflush(log_file);
}

static void show_message(const char *msg)
{
   struct retro_message m = { msg, 60 * 6 };
   env_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
}

static int to_wide(const char *in, wchar_t *out, int out_len)
{
   return MultiByteToWideChar(CP_UTF8, 0, in, -1, out, out_len) > 0;
}

static int file_exists(const wchar_t *path)
{
   DWORD attr = GetFileAttributesW(path);
   return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static int find_eden(wchar_t *out, size_t out_len)
{
   const char *system_dir = NULL;
   char buf[MAX_PATH * 4];
   FILE *f;

   if (!env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) || !system_dir)
      return 0;

   snprintf(buf, sizeof(buf), "%s\\eden.txt", system_dir);
   if ((f = fopen(buf, "r")))
   {
      char line[MAX_PATH * 4] = {0};
      if (fgets(line, sizeof(line), f))
      {
         char *start = line;
         char *end;
         /* Skip a UTF-8 BOM (Notepad), whitespace and surrounding quotes. */
         if (!strncmp(start, "\xEF\xBB\xBF", 3))
            start += 3;
         while (*start == ' ' || *start == '\t' || *start == '"')
            start++;
         end = start + strlen(start);
         while (end > start && strchr(" \t\r\n\"", end[-1]))
            *--end = '\0';
         if (*start && to_wide(start, out, (int)out_len) && file_exists(out))
         {
            fclose(f);
            return 1;
         }
      }
      fclose(f);
   }

   snprintf(buf, sizeof(buf), "%s\\Eden\\eden.exe", system_dir);
   return to_wide(buf, out, (int)out_len) && file_exists(out);
}

static int fullscreen_enabled(void)
{
   struct retro_variable var = { "eden_launcher_fullscreen", NULL };
   if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      return strcmp(var.value, "disabled") != 0;
   return 1;
}

/* ---------------------------------------------------------------------- */
/* Eden's qt-config.ini                                                   */
/* ---------------------------------------------------------------------- */

/* Sets key=value in [section] of an INI text, adding the section or key
 * if missing. Eden (Qt) also keeps "key\default=true" while a setting is
 * at its default, and then ignores the value, so that is set to false. */
static bool ini_set_line(char **buf, size_t *len, const char *section,
      const char *key, const char *value)
{
   char header[96], line[256];
   char *sec, *p, *line_start, *line_end, *out, *insert;
   size_t key_len = strlen(key), line_len, new_len;

   snprintf(header, sizeof(header), "[%s]", section);
   snprintf(line, sizeof(line), "%s=%s\r\n", key, value);
   line_len = strlen(line);

   /* The section's header, at the start of a line. */
   for (sec = strstr(*buf, header); sec; sec = strstr(sec + 1, header))
      if (sec == *buf || sec[-1] == '\n')
         break;

   if (!sec)
   {
      /* New section at the end. */
      size_t add = strlen(header) + 4 + line_len;
      if (!(out = (char*)malloc(*len + add + 1)))
         return false;
      memcpy(out, *buf, *len);
      new_len = *len;
      if (new_len && out[new_len - 1] != '\n')
         out[new_len++] = '\n';
      new_len += (size_t)sprintf(out + new_len, "\r\n%s\r\n%s", header, line);
      free(*buf);
      *buf = out;
      *len = new_len;
      return true;
   }

   /* Look for the key within the section (until the next header). */
   p      = strchr(sec, '\n');
   p      = p ? p + 1 : *buf + *len;
   insert = p;
   while (p < *buf + *len && *p != '[')
   {
      line_start = p;
      line_end   = strchr(p, '\n');
      line_end   = line_end ? line_end + 1 : *buf + *len;
      if (     !strncmp(line_start, key, key_len)
            && line_start[key_len] == '=')
      {
         new_len = *len - (size_t)(line_end - line_start) + line_len;
         if (!(out = (char*)malloc(new_len + 1)))
            return false;
         memcpy(out, *buf, (size_t)(line_start - *buf));
         memcpy(out + (line_start - *buf), line, line_len);
         memcpy(out + (line_start - *buf) + line_len, line_end,
               *len - (size_t)(line_end - *buf) + 1);
         free(*buf);
         *buf = out;
         *len = new_len;
         return true;
      }
      /* Insert after the section's last non-empty line. */
      if (line_end - line_start > 2)
         insert = line_end;
      p = line_end;
   }

   new_len = *len + line_len;
   if (!(out = (char*)malloc(new_len + 1)))
      return false;
   memcpy(out, *buf, (size_t)(insert - *buf));
   memcpy(out + (insert - *buf), line, line_len);
   memcpy(out + (insert - *buf) + line_len, insert,
         *len - (size_t)(insert - *buf) + 1);
   free(*buf);
   *buf = out;
   *len = new_len;
   return true;
}

static void ini_set(char **buf, size_t *len, const char *section,
      const char *key, const char *value)
{
   char def_key[128];
   snprintf(def_key, sizeof(def_key), "%.110s\\default", key);
   ini_set_line(buf, len, section, def_key, "false");
   ini_set_line(buf, len, section, key, value);
}

/* ---------------------------------------------------------------------- */
/* Environment for Eden                                                   */
/* ---------------------------------------------------------------------- */

/* RetroArch's environment, minus what Steam sets for SDL games when Steam
 * Input is on: those make SDL identify Steam's virtual controller (from
 * SteamVirtualGamepadInfo) and then skip it as a device to ignore, which
 * leaves Eden, which doesn't talk to Steam, with no controller at all.
 * Without them SDL sees the virtual controller as a plain XInput pad.
 * The multiplayer settings (room password included) aren't passed on.
 * Returns a block for CreateProcessW (CREATE_UNICODE_ENVIRONMENT); free()
 * it. NULL: inherit unchanged. */
static wchar_t *child_environment(void)
{
   static const wchar_t *drop[] = {
      L"BOMBA_MP_",
      L"SDL_GAMECONTROLLER_IGNORE_DEVICES=",
      L"SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=",
      L"SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=",
      L"SDL_GAMECONTROLLER_USE_BUTTON_LABELS=",
      L"SDL_GAMECONTROLLERCONFIG=",
      L"SDL_JOYSTICK_HIDAPI_STEAMXBOX=",
      L"SteamVirtualGamepadInfo=",
      L"SteamGenericControllers=",
   };
   LPWCH env = GetEnvironmentStringsW(), e;
   wchar_t *out, *o;
   size_t total = 0, i;

   if (!env)
      return NULL;
   for (e = env; *e; e += wcslen(e) + 1)
      total += wcslen(e) + 1;
   if (!(out = (wchar_t*)malloc((total + 1) * sizeof(wchar_t))))
   {
      FreeEnvironmentStringsW(env);
      return NULL;
   }

   for (o = out, e = env; *e; e += wcslen(e) + 1)
   {
      bool keep = true;
      for (i = 0; i < sizeof(drop) / sizeof(drop[0]) && keep; i++)
         if (!_wcsnicmp(e, drop[i], wcslen(drop[i])))
            keep = false;
      if (keep)
      {
         wcscpy(o, e);
         o += wcslen(e) + 1;
      }
   }
   *o = L'\0';
   FreeEnvironmentStringsW(env);
   return out;
}

/* CreateProcessW for Eden and its helpers. RetroArch loads Steam's overlay,
 * which detours kernel32's CreateProcessW to inject itself into every child;
 * inside Eden (and eden_padinfo) the overlay hides the controller from SDL.
 * KernelBase's export is the untouched implementation, so the children start
 * clean. The overlay still draws over Eden through RetroArch's window. */
typedef BOOL (WINAPI *create_process_w_t)(LPCWSTR, LPWSTR,
      LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
      LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);

static BOOL spawn_process(LPCWSTR app, LPWSTR cmd, BOOL inherit,
      DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si,
      LPPROCESS_INFORMATION pi)
{
   static create_process_w_t create;
   if (!create)
   {
      HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
      if (kb)
         create = (create_process_w_t)GetProcAddress(kb, "CreateProcessW");
      if (!create)
         create = CreateProcessW;
   }
   return create(app, cmd, NULL, NULL, inherit, flags, env, dir, si, pi);
}

/* ---------------------------------------------------------------------- */
/* Controller: Player 1 on the first game controller                      */
/* ---------------------------------------------------------------------- */

/* A boolean "key=true|false" from the INI text (any section). */
static bool ini_get_bool(const char *buf, const char *key, bool def)
{
   size_t key_len = strlen(key);
   const char *p;

   for (p = strstr(buf, key); p; p = strstr(p + 1, key))
      if ((p == buf || p[-1] == '\n') && p[key_len] == '=')
         return !strncmp(p + key_len + 1, "true", 4);
   return def;
}

/* The folder this core's DLL is in. */
static bool core_dir(wchar_t *out, size_t len)
{
   HMODULE self = NULL;
   wchar_t *slash;

   if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(void*)core_dir, &self)
         || !GetModuleFileNameW(self, out, (DWORD)len)
         || !(slash = wcsrchr(out, L'\\')))
      return false;
   *slash = L'\0';
   return true;
}

/* SDL GameController button numbers (SDL_GameControllerButton). */
enum
{
   SDL_A_BUTTON = 0, SDL_B_BUTTON, SDL_X_BUTTON, SDL_Y_BUTTON, SDL_BACK_BUTTON,
   SDL_GUIDE_BUTTON, SDL_START_BUTTON, SDL_LSTICK_BUTTON, SDL_RSTICK_BUTTON,
   SDL_LSHOULDER_BUTTON, SDL_RSHOULDER_BUTTON,
   SDL_DUP_BUTTON, SDL_DDOWN_BUTTON, SDL_DLEFT_BUTTON, SDL_DRIGHT_BUTTON
};

/* The bindings the player gave Switch in the grid (Settings > Controls >
 * Consoles > Switch > Bindings), kept in Bomba.cfg in
 * RetroArch's remap folder as "Switch_<control>_btn / _axis". They hold
 * RetroArch's xinput joypad driver's button / axis numbers; each becomes
 * the same physical control in SDL GameController terms, as an override
 * for eden_padinfo ("<eden key>=b:<button>" or "=a:<axis>:<+|->"). The
 * Switch's controls sit where the RetroPad's do (A right, B bottom...). */
static void grid_bind_args(wchar_t *out, size_t out_len)
{
   static const struct { const char *ra, *eden; } controls[] = {
      { "b", "player_0_button_b" },      { "y", "player_0_button_y" },
      { "select", "player_0_button_minus" }, { "start", "player_0_button_plus" },
      { "up", "player_0_button_dup" },   { "down", "player_0_button_ddown" },
      { "left", "player_0_button_dleft" }, { "right", "player_0_button_dright" },
      { "a", "player_0_button_a" },      { "x", "player_0_button_x" },
      { "l", "player_0_button_l" },      { "r", "player_0_button_r" },
      { "l2", "player_0_button_zl" },    { "r2", "player_0_button_zr" },
      { "l3", "player_0_button_lstick" }, { "r3", "player_0_button_rstick" },
   };
   /* xinput driver button number -> SDL GameController button. */
   static const int xinput_to_sdl[] = {
      SDL_A_BUTTON, SDL_B_BUTTON, SDL_X_BUTTON, SDL_Y_BUTTON,
      SDL_LSHOULDER_BUTTON, SDL_RSHOULDER_BUTTON, SDL_START_BUTTON, SDL_BACK_BUTTON,
      SDL_LSTICK_BUTTON, SDL_RSTICK_BUTTON, SDL_GUIDE_BUTTON
   };
   wchar_t path[MAX_PATH * 2], *slash;
   char *text = NULL;
   size_t i, used = 0;
   long size;
   FILE *f;

   out[0] = L'\0';
   if (     !GetModuleFileNameW(NULL, path, MAX_PATH * 2)
         || !(slash = wcsrchr(path, L'\\')))
      return;
   *slash = L'\0';
   /* The remap folder sits beside the app folder (the template's
    * ":\..\config\remaps"). */
   wcsncat(path, L"\\..\\config\\remaps\\Bomba.cfg", MAX_PATH * 2 - wcslen(path) - 1);
   if (!(f = _wfopen(path, L"rb")))
   {
      log_msg("controller: no grid bindings (%ls)", path);
      return;
   }
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (size > 0 && (text = (char*)malloc((size_t)size + 1)))
      text[fread(text, 1, (size_t)size, f)] = '\0';
   fclose(f);
   if (!text)
      return;

   for (i = 0; i < sizeof(controls) / sizeof(controls[0]); i++)
   {
      char key[48], *p;
      unsigned long v;
      wchar_t arg[96];
      arg[0] = L'\0';

      /* A button: a number, or a hat direction (RetroArch's HAT masks). */
      snprintf(key, sizeof(key), "Switch_%s_btn", controls[i].ra);
      if ((p = strstr(text, key)) && sscanf(p + strlen(key), " = \"%lu\"", &v) == 1)
      {
         if (v & 0xF000u) /* hat 0 */
         {
            int b = (v & 0x8000u) ? SDL_DUP_BUTTON : (v & 0x4000u) ? SDL_DDOWN_BUTTON
                  : (v & 0x2000u) ? SDL_DLEFT_BUTTON : SDL_DRIGHT_BUTTON;
            _snwprintf(arg, 96, L" %hs=b:%d", controls[i].eden, b);
         }
         else if (v < sizeof(xinput_to_sdl) / sizeof(xinput_to_sdl[0]))
            _snwprintf(arg, 96, L" %hs=b:%d", controls[i].eden, xinput_to_sdl[v]);
      }

      /* An axis direction: RetroArch's AXIS_POS / AXIS_NEG encoding. The
       * xinput driver's axes (LX, LY, RX, RY, LT, RT) are numbered like
       * SDL's, with the same directions. */
      snprintf(key, sizeof(key), "Switch_%s_axis", controls[i].ra);
      if (!arg[0] && (p = strstr(text, key)) && sscanf(p + strlen(key), " = \"%lu\"", &v) == 1
            && v != 0xFFFFFFFFul)
      {
         if ((v & 0xFFFF0000ul) == 0xFFFF0000ul)
            _snwprintf(arg, 96, L" %hs=a:%lu:+", controls[i].eden, v & 0xFFFFu);
         else if ((v & 0xFFFFu) == 0xFFFFu)
            _snwprintf(arg, 96, L" %hs=a:%lu:-", controls[i].eden, v >> 16);
      }

      if (arg[0] && used + wcslen(arg) + 1 < out_len)
      {
         wcscat(out, arg);
         used += wcslen(arg);
         log_msg("controller: grid binding%ls", arg);
      }
   }
   free(text);
}

/* Player 1's mapping, from eden_padinfo.exe (next to this core, see
 * eden_padinfo.c), which asks RetroArch's SDL2.dll in a process of its
 * own, the way Eden's SDL2 will see the controllers. */
static void write_controls(char **buf, size_t *len)
{
   wchar_t dir[MAX_PATH * 2], exe_dir[MAX_PATH * 2], cmd[MAX_PATH * 6 + 1600];
   wchar_t binds[1536];
   wchar_t *slash;
   SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
   STARTUPINFOW si;
   PROCESS_INFORMATION pi;
   HANDLE rd = NULL, wr = NULL;
   char out[16384], *line, *next;
   DWORD got, total = 0;
   int mapped = 0;

   if (!core_dir(dir, MAX_PATH * 2)
         || !GetModuleFileNameW(NULL, exe_dir, MAX_PATH * 2)
         || !(slash = wcsrchr(exe_dir, L'\\')))
      return;
   *slash = L'\0';

   grid_bind_args(binds, sizeof(binds) / sizeof(binds[0]));
   _snwprintf(cmd, MAX_PATH * 6 + 1600, L"\"%ls\\eden_padinfo.exe\" \"%ls\\SDL2.dll\" %d %d %d%ls",
         dir, exe_dir,
         ini_get_bool(*buf, "enable_raw_input", false),
         ini_get_bool(*buf, "enable_joycon_driver", true),
         ini_get_bool(*buf, "enable_procon_driver", false), binds);
   cmd[MAX_PATH * 6 + 1599] = L'\0';

   if (!CreatePipe(&rd, &wr, &sa, 0))
      return;
   SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

   memset(&si, 0, sizeof(si));
   si.cb         = sizeof(si);
   si.dwFlags    = STARTF_USESTDHANDLES;
   si.hStdOutput = wr;
   si.hStdError  = wr;
   si.hStdInput  = NULL;
   /* The environment Eden will get, so it sees what Eden will see. */
   {
      wchar_t *env = child_environment();
      BOOL ok = spawn_process(NULL, cmd, TRUE,
            CREATE_NO_WINDOW | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
            env, NULL, &si, &pi);
      free(env);
      if (!ok)
      {
         log_msg("controller: can't run eden_padinfo.exe (%lu)", (unsigned long)GetLastError());
         CloseHandle(rd);
         CloseHandle(wr);
         return;
      }
   }
   CloseHandle(wr);
   CloseHandle(pi.hThread);

   while (total < sizeof(out) - 1
         && ReadFile(rd, out + total, (DWORD)(sizeof(out) - 1 - total), &got, NULL) && got)
      total += got;
   out[total] = '\0';
   CloseHandle(rd);
   WaitForSingleObject(pi.hProcess, 5000);
   CloseHandle(pi.hProcess);

   for (line = out; line && *line; line = next)
   {
      char *eq, *end;
      if ((next = strchr(line, '\n')))
         *next++ = '\0';
      if ((end = strchr(line, '\r')))
         *end = '\0';
      if (line[0] == '#')
         log_msg("controller:%s", line + 1);
      else if ((eq = strchr(line, '=')))
      {
         *eq = '\0';
         ini_set(buf, len, "Controls", line, eq + 1);
         mapped++;
      }
   }
   if (!mapped)
      log_msg("controller: nothing mapped; Eden keeps its own mapping");
}

/* ---------------------------------------------------------------------- */
/* Eden's settings as core options                                        */
/* ---------------------------------------------------------------------- */

/* Shown in RetroArch's Quick Menu and on the grid's Consoles pages (the
 * categories put them under Graphics and System). Values are Eden's own
 * (its enum indices); defaults are Eden's. Eden reads its config when it
 * starts, so changes apply from the next launch. */
static struct retro_core_option_v2_category option_cats[] = {
   { "video",  "Video",  "Eden's graphics. Changes apply the next time a game starts." },
   { "system", "System", "Eden's console settings. Changes apply the next time a game starts." },
   { NULL, NULL, NULL },
};

static struct retro_core_option_v2_definition option_defs[] = {
   { "eden_resolution", "Video > Resolution", "Resolution",
      "Rendering resolution, as a multiple of the Switch's own (720p handheld, 1080p docked).",
      NULL, "video",
      { { "1", "0.5x" }, { "2", "0.75x" }, { "3", "1x (native)" }, { "5", "1.5x" },
        { "6", "2x" }, { "7", "3x" }, { "8", "4x" }, { NULL, NULL } }, "3" },
   { "eden_aspect", "Video > Aspect Ratio", "Aspect Ratio", NULL, NULL, "video",
      { { "0", "16:9" }, { "1", "4:3" }, { "2", "21:9" }, { "3", "16:10" },
        { "4", "Stretch to window" }, { NULL, NULL } }, "0" },
   { "eden_aa", "Video > Anti-Aliasing", "Anti-Aliasing", NULL, NULL, "video",
      { { "0", "Off" }, { "1", "FXAA" }, { "2", "SMAA" }, { NULL, NULL } }, "0" },
   { "eden_filter", "Video > Scaling Filter", "Scaling Filter",
      "How the picture is scaled to the screen.", NULL, "video",
      { { "1", "Bilinear" }, { "0", "Nearest neighbor" }, { "2", "Bicubic" },
        { "3", "Gaussian" }, { "4", "Lanczos" }, { "5", "ScaleForce" },
        { "6", "AMD FSR" }, { "7", "Area" }, { NULL, NULL } }, "1" },
   { "eden_anisotropy", "Video > Anisotropic Filtering", "Anisotropic Filtering", NULL, NULL, "video",
      { { "0", "Automatic" }, { "1", "Game's default" }, { "2", "2x" }, { "3", "4x" },
        { "4", "8x" }, { "5", "16x" }, { NULL, NULL } }, "0" },
   { "eden_vsync", "Video > VSync", "VSync", NULL, NULL, "video",
      { { "2", "On (FIFO)" }, { "1", "Mailbox" }, { "3", "FIFO relaxed" },
        { "0", "Off (immediate)" }, { NULL, NULL } }, "2" },
   { "eden_docked", "System > Console Mode", "Console Mode",
      "Docked plays as on a TV; handheld as on the Switch's own screen.", NULL, "system",
      { { "1", "Docked" }, { "0", "Handheld" }, { NULL, NULL } }, "1" },
   { "eden_region", "System > Region", "Region", NULL, NULL, "system",
      { { "1", "USA" }, { "2", "Europe" }, { "0", "Japan" }, { "3", "Australia" },
        { "4", "China" }, { "5", "Korea" }, { "6", "Taiwan" }, { NULL, NULL } }, "1" },
   { "eden_launcher_fullscreen", "System > Fullscreen Without Overlay", "Fullscreen Without Overlay",
      "Only when RetroArch can't show Eden under its own window.", NULL, "system",
      { { "enabled", NULL }, { "disabled", NULL }, { NULL, NULL } }, "enabled" },
   { NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

/* Which Eden setting each option is. */
static const struct { const char *option, *section, *key; } option_keys[] = {
   { "eden_resolution", "Renderer", "resolution_setup" },
   { "eden_aspect",     "Renderer", "aspect_ratio" },
   { "eden_aa",         "Renderer", "anti_aliasing" },
   { "eden_filter",     "Renderer", "scaling_filter" },
   { "eden_anisotropy", "Renderer", "max_anisotropy" },
   { "eden_vsync",      "Renderer", "use_vsync" },
   { "eden_docked",     "System",   "use_docked_mode" },
   { "eden_region",     "System",   "region_index" },
};

#define OPTION_COUNT (sizeof(option_keys) / sizeof(option_keys[0]))

/* The options' values as last written, to tell a real change. */
static char options_written[512];
/* The options' values as last read ("" unset), see read_options. */
static char option_values[OPTION_COUNT][16];

/* Reads the options' values from RetroArch, on its thread (the launch
 * thread writes them, see launch_main). */
static void read_options(void)
{
   size_t i;
   for (i = 0; i < OPTION_COUNT; i++)
   {
      struct retro_variable var = { option_keys[i].option, NULL };
      option_values[i][0] = '\0';
      if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
         snprintf(option_values[i], sizeof(option_values[i]), "%s", var.value);
   }
}

static void apply_options(char **buf, size_t *len, void *data)
{
   char now[512] = {0};
   size_t i;

   (void)data;
   for (i = 0; i < OPTION_COUNT; i++)
   {
      if (!option_values[i][0])
         continue;
      ini_set(buf, len, option_keys[i].section, option_keys[i].key, option_values[i]);
      strncat(now, option_values[i], sizeof(now) - strlen(now) - 2);
      strcat(now, ",");
   }
   strcpy(options_written, now);
}

/* ---------------------------------------------------------------------- */
/* Writing qt-config.ini                                                  */
/* ---------------------------------------------------------------------- */

static wchar_t config_path[MAX_PATH * 2];
static CRITICAL_SECTION config_lock; /* the core's thread and RetroArch's */
static bool config_lock_init;

typedef void (*config_fn)(char **buf, size_t *len, void *data);

/* Reads Eden's config, lets fn change it, and writes it back (through a
 * temporary file, so Eden never reads half of it). Creates it if Eden
 * hasn't written one yet. */
static void config_edit(config_fn fn, void *data)
{
   wchar_t tmp[MAX_PATH * 2];
   char *buf;
   size_t len = 0;
   long size = 0;
   FILE *f;

   if (!config_path[0] || !config_lock_init)
      return;
   EnterCriticalSection(&config_lock);

   if ((f = _wfopen(config_path, L"rb")))
   {
      fseek(f, 0, SEEK_END);
      size = ftell(f);
      fseek(f, 0, SEEK_SET);
   }
   if (!(buf = (char*)malloc((size_t)(size > 0 ? size : 0) + 1)))
   {
      if (f)
         fclose(f);
      LeaveCriticalSection(&config_lock);
      return;
   }
   if (f)
   {
      len = fread(buf, 1, (size_t)(size > 0 ? size : 0), f);
      fclose(f);
   }
   buf[len] = '\0';

   fn(&buf, &len, data);

   _snwprintf(tmp, MAX_PATH * 2, L"%ls.tmp", config_path);
   tmp[MAX_PATH * 2 - 1] = L'\0';
   if ((f = _wfopen(tmp, L"wb")))
   {
      bool ok = fwrite(buf, 1, len, f) == len;
      ok = fclose(f) == 0 && ok;
      if (!ok || !MoveFileExW(tmp, config_path, MOVEFILE_REPLACE_EXISTING))
      {
         log_msg("couldn't write %ls", config_path);
         DeleteFileW(tmp);
      }
   }
   free(buf);
   LeaveCriticalSection(&config_lock);
}

/* Settings Eden needs to work as part of RetroArch: no dialogs that would
 * open hidden under RetroArch's window (stop confirmation, update check,
 * first start), no pausing of its own (this core pauses it), borderless
 * fullscreen; then the core options and Player 1's controls. */
static void apply_launch(char **buf, size_t *len, void *data)
{
   ini_set(buf, len, "UI", "confirmStop", "2"); /* Ask_Never */
   ini_set(buf, len, "UI", "check_for_updates", "false");
   ini_set(buf, len, "UI", "firstStart", "false");
   ini_set(buf, len, "UI", "pauseWhenInBackground", "false");
   ini_set(buf, len, "UI", "muteWhenInBackground", "false");
   ini_set(buf, len, "Renderer", "fullscreen_mode", "0"); /* Borderless */
   apply_options(buf, len, data);
   write_controls(buf, len);
}

/* Makes the folder link a junction to the folder target, replacing an
 * older junction or an empty folder there. A folder with files in it is
 * left alone. */
static bool make_junction(const wchar_t *link, const wchar_t *target)
{
   struct
   {
      DWORD tag;
      WORD  len, reserved;
      WORD  sub_off, sub_len, print_off, print_len;
      WCHAR buf[4 * MAX_PATH + 8];
   } rp;
   wchar_t full[2 * MAX_PATH + 1];
   size_t tl;
   DWORD attr, bytes;
   HANDLE h;
   BOOL ok;

   /* A junction keeps its target as written and nothing resolves ".." in
    * it later (RetroArch's save folder is "<app>\..\saves\..."): Eden then
    * can't open anything through it. */
   tl = GetFullPathNameW(target, 2 * MAX_PATH + 1, full, NULL);
   if (!tl || tl > 2 * MAX_PATH)
      return false;
   target = full;

   /* RemoveDirectory takes away only a junction, not what it points to,
    * and fails on a folder that isn't empty. */
   attr = GetFileAttributesW(link);
   if (attr != INVALID_FILE_ATTRIBUTES)
   {
      if (!(attr & FILE_ATTRIBUTE_DIRECTORY) || !RemoveDirectoryW(link))
         return false;
   }
   if (!CreateDirectoryW(link, NULL))
      return false;

   /* "\??\<target>" then the plain path, each NUL-terminated. */
   memset(&rp, 0, sizeof(rp));
   rp.tag       = IO_REPARSE_TAG_MOUNT_POINT;
   rp.sub_len   = (WORD)((tl + 4) * sizeof(WCHAR));
   rp.print_off = (WORD)(rp.sub_len + sizeof(WCHAR));
   rp.print_len = (WORD)(tl * sizeof(WCHAR));
   memcpy(rp.buf, L"\\??\\", 4 * sizeof(WCHAR));
   memcpy(rp.buf + 4, target, (tl + 1) * sizeof(WCHAR));
   memcpy(rp.buf + tl + 5, target, (tl + 1) * sizeof(WCHAR));
   rp.len       = (WORD)(8 + rp.print_off + rp.print_len + sizeof(WCHAR));

   h = CreateFileW(link, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
   if (h == INVALID_HANDLE_VALUE)
   {
      RemoveDirectoryW(link);
      return false;
   }
   ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &rp, 8 + rp.len,
         NULL, 0, &bytes, NULL);
   CloseHandle(h);
   if (!ok)
      RemoveDirectoryW(link);
   return ok ? true : false;
}

/* Points Eden's game saves at the current profile's (see the top). */
static void link_saves(const wchar_t *exe)
{
   wchar_t dir[MAX_PATH * 2], link[MAX_PATH * 2], target[MAX_PATH * 2];
   wchar_t *slash, *p;

   if (!save_dir_cache[0] || !to_wide(save_dir_cache, dir, MAX_PATH * 2))
      return;
   _snwprintf(target, MAX_PATH * 2, L"%ls\\save", dir);
   target[MAX_PATH * 2 - 1] = L'\0';
   for (p = target; *p; p++)
      if (*p == L'/')
         *p = L'\\';
   CreateDirectoryW(target, NULL);

   wcsncpy(dir, exe, MAX_PATH * 2 - 1);
   dir[MAX_PATH * 2 - 1] = L'\0';
   if (!(slash = wcsrchr(dir, L'\\')))
      return;
   *slash = L'\0';
   _snwprintf(link, MAX_PATH * 2, L"%ls\\user", dir);
   CreateDirectoryW(link, NULL);
   _snwprintf(link, MAX_PATH * 2, L"%ls\\user\\nand", dir);
   CreateDirectoryW(link, NULL);
   _snwprintf(link, MAX_PATH * 2, L"%ls\\user\\nand\\user", dir);
   CreateDirectoryW(link, NULL);
   _snwprintf(link, MAX_PATH * 2, L"%ls\\user\\nand\\user\\save", dir);
   link[MAX_PATH * 2 - 1] = L'\0';

   if (make_junction(link, target))
      log_msg("saves: %ls", target);
   else
      log_msg("couldn't link %ls to %ls", link, target);
}

/* eden-cli.exe's settings: the same as eden.exe's, minus its UI. */
static void apply_launch_cli(char **buf, size_t *len, void *data)
{
   ini_set(buf, len, "Renderer", "fullscreen_mode", "0"); /* Borderless */
   apply_options(buf, len, data);
   write_controls(buf, len);
}

/* Writes the config of the Eden that's started: eden.exe reads
 * qt-config.ini, eden-cli.exe (multiplayer) sdl2-config.ini. Options
 * changed while it runs go to the same file. */
static void write_config(const wchar_t *exe, bool cli)
{
   wchar_t dir[MAX_PATH * 2];
   wchar_t *slash;

   wcsncpy(dir, exe, MAX_PATH * 2 - 1);
   dir[MAX_PATH * 2 - 1] = L'\0';
   if (!(slash = wcsrchr(dir, L'\\')))
      return;
   *slash = L'\0';
   _snwprintf(config_path, MAX_PATH * 2, L"%ls\\user\\config", dir);
   config_path[MAX_PATH * 2 - 1] = L'\0';
   CreateDirectoryW(config_path, NULL);
   _snwprintf(config_path, MAX_PATH * 2, L"%ls\\user\\config\\%ls", dir,
         cli ? L"sdl2-config.ini" : L"qt-config.ini");
   config_path[MAX_PATH * 2 - 1] = L'\0';

   config_edit(cli ? apply_launch_cli : apply_launch, NULL);
}

/* ---------------------------------------------------------------------- */
/* Multiplayer                                                            */
/* ---------------------------------------------------------------------- */

/* Eden plays the Switch's local wireless and LAN modes over a "room", a
 * UDP relay every player joins. The grid's Multiplayer settings (one for
 * all launcher cores, on until set to Off) arrive as RetroArch's
 * environment variables BOMBA_MP_MODE (host / join), _ADDRESS, _PORT,
 * _PASSWORD, _MAX and _NAME (the player: the grid profile's name).
 * eden-cli.exe joins from its command line. Hosting also starts eden.exe's
 * dedicated room server, hidden, for as long as the game runs, and joins
 * it. */
#define EDEN_ROOM_PORT 24872

static struct
{
   bool active;
   bool host;
   char address[256];
   unsigned port;
   char password[128];
   unsigned max_members;
   char name[64];
   char nick[32];      /* name as Eden takes it, see player_name */
} mp;

static void player_name(char *out, size_t out_len);

static HANDLE room_job; /* the room server, killed when this closes */

static void mp_getenv(const char *var, char *out, DWORD len)
{
   DWORD n = GetEnvironmentVariableA(var, out, len);
   if (!n || n >= len)
      out[0] = '\0';
}

static void read_multiplayer_settings(void)
{
   char mode[16], num[16];

   memset(&mp, 0, sizeof(mp));
   mp_getenv("BOMBA_MP_MODE", mode, sizeof(mode));
   mp.active = !strcmp(mode, "join") || !strcmp(mode, "host");
   mp.host   = !strcmp(mode, "host");
   if (!mp.active)
      return;

   mp_getenv("BOMBA_MP_ADDRESS",  mp.address,  sizeof(mp.address));
   mp_getenv("BOMBA_MP_PASSWORD", mp.password, sizeof(mp.password));
   mp_getenv("BOMBA_MP_NAME",     mp.name,     sizeof(mp.name));
   mp_getenv("BOMBA_MP_PORT", num, sizeof(num));
   mp.port = num[0] ? (unsigned)strtoul(num, NULL, 10) : EDEN_ROOM_PORT;
   mp_getenv("BOMBA_MP_MAX", num, sizeof(num));
   mp.max_members = (unsigned)strtoul(num, NULL, 10);
   if (mp.max_members < 2 || mp.max_members > 16)
      mp.max_members = 8;

   if (mp.host)
      strcpy(mp.address, "127.0.0.1");
   player_name(mp.nick, sizeof(mp.nick));
   /* eden-cli splits "nick:password@address:port" at those characters. */
   if (!mp.address[0])
   {
      log_msg("multiplayer: no room address; playing alone");
      show_message("Multiplayer: set the room's address in Settings > Multiplayer. Playing alone.");
      mp.active = false;
   }
   else if (     !mp.port || mp.port > 65535
         || strpbrk(mp.address, ":@\" ") || strpbrk(mp.password, ":@\""))
   {
      log_msg("multiplayer: settings not usable (address \"%s\", port %u); playing alone",
            mp.address, mp.port);
      show_message("Multiplayer: the room's address or password can't be used "
                   "(no spaces, quotes, ':' or '@'). Playing alone.");
      mp.active = false;
   }
}

/* The player's name as Eden takes nicknames and room names: 4 to 20 of
 * letters, digits, space, '.', '_' and '-'. The grid profile's name, else
 * RetroArch's user name. */
static void player_name(char *out, size_t out_len)
{
   const char *name = mp.name;
   size_t n = 0;

   if (!name[0] && !(env_cb(RETRO_ENVIRONMENT_GET_USERNAME, &name) && name))
      name = "";
   for (; *name && n < 20 && n + 1 < out_len; name++)
      if (     (*name >= 'a' && *name <= 'z') || (*name >= 'A' && *name <= 'Z')
            || (*name >= '0' && *name <= '9') || strchr(" ._-", *name))
         out[n++] = *name;
   out[n] = '\0';
   while (n && out[n - 1] == ' ')
      out[--n] = '\0';
   if (n < 4)
      snprintf(out, out_len, "Player");
}

/* Starts eden.exe's room server, hidden, in a job that ends it when the
 * core lets go of it (or RetroArch goes away). */
static bool start_room(const wchar_t *exe, const wchar_t *workdir, const wchar_t *rom)
{
   JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
   wchar_t cmd[MAX_PATH * 8], game[MAX_PATH * 2], log_path[MAX_PATH * 2];
   wchar_t ban_path[MAX_PATH * 2], password[96], name[32];
   const wchar_t *base = wcsrchr(rom, L'\\');
   wchar_t *dot, *p;
   STARTUPINFOW si;
   PROCESS_INFORMATION pi;

   /* The room's preferred game: the game's file name. */
   wcsncpy(game, base ? base + 1 : rom, MAX_PATH * 2 - 1);
   game[MAX_PATH * 2 - 1] = L'\0';
   if ((dot = wcsrchr(game, L'.')))
      *dot = L'\0';
   for (p = game; *p; p++)
      if (*p == L'"')
         *p = L'\'';

   _snwprintf(name, 32, L"%hs", mp.nick);
   name[31] = L'\0';

   password[0] = L'\0';
   if (mp.password[0])
      _snwprintf(password, 96, L" -w \"%hs\"", mp.password);
   password[95] = L'\0';

   log_path[0] = L'\0';
   if (save_dir_cache[0] && to_wide(save_dir_cache, log_path, MAX_PATH * 2 - 32))
      wcscat(log_path, L"\\eden-room.log");
   _snwprintf(ban_path, MAX_PATH * 2, L"%ls\\user\\room-ban-list.txt", workdir);
   ban_path[MAX_PATH * 2 - 1] = L'\0';

   /* On loopback only: players arrive through RetroArch's Steam tunnel
    * (bomba_steamnet.c), so nothing is opened to the network and Windows
    * Firewall has nothing to ask. */
   _snwprintf(cmd, MAX_PATH * 8,
         L"\"%ls\" --room -s 127.0.0.1 -n \"%ls\" -p %u -m %u -g \"%ls\" -b \"%ls\"%ls%ls%ls%ls",
         exe, name, mp.port, mp.max_members, game, ban_path, password,
         log_path[0] ? L" -l \"" : L"", log_path, log_path[0] ? L"\"" : L"");
   cmd[MAX_PATH * 8 - 1] = L'\0';
   log_msg("multiplayer: starting room \"%ls\" on port %u for %u players%s",
         name, mp.port, mp.max_members, mp.password[0] ? ", with password" : "");

   if (!(room_job = CreateJobObjectW(NULL, NULL)))
      return false;
   memset(&limits, 0, sizeof(limits));
   limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
   SetInformationJobObject(room_job, JobObjectExtendedLimitInformation,
         &limits, sizeof(limits));

   memset(&si, 0, sizeof(si));
   si.cb = sizeof(si);
   {
      wchar_t *env = child_environment();
      BOOL ok = spawn_process(exe, cmd, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW
            | (env ? CREATE_UNICODE_ENVIRONMENT : 0), env, workdir, &si, &pi);
      free(env);
      if (!ok)
      {
         log_msg("multiplayer: room didn't start (%lu)", (unsigned long)GetLastError());
         CloseHandle(room_job);
         room_job = NULL;
         return false;
      }
   }
   AssignProcessToJobObject(room_job, pi.hProcess);
   ResumeThread(pi.hThread);
   CloseHandle(pi.hThread);

   /* Up before the game joins it; one that fails exits at once. */
   if (WaitForSingleObject(pi.hProcess, 1000) == WAIT_OBJECT_0)
   {
      DWORD code = 0;
      GetExitCodeProcess(pi.hProcess, &code);
      log_msg("multiplayer: room server exited (%lu), see eden-room.log", (unsigned long)code);
      CloseHandle(pi.hProcess);
      CloseHandle(room_job);
      room_job = NULL;
      return false;
   }
   CloseHandle(pi.hProcess);
   return true;
}

static void stop_room(void)
{
   if (!room_job)
      return;
   CloseHandle(room_job);
   room_job = NULL;
   log_msg("multiplayer: room closed");
}

/* eden-cli.exe joins the room given with --multiplayer but never starts
 * Eden's network (Network::Init(), which only eden.exe calls), so the
 * join fails ("Could not access RoomMember") and it quits. The function
 * is in eden-cli.exe all the same: found here as the one that logs
 * "Error initializing ENet" (x64 unwind data gives its start), it's run
 * in Eden's process right after Eden starts, before the game is loaded
 * and the room joined. Eden's files stay as they are. */
static uint8_t  *pe_file;
static IMAGE_NT_HEADERS64 *pe_nt;
static IMAGE_SECTION_HEADER *pe_sec;

static uint8_t *pe_rva(uint32_t rva)
{
   int i;
   for (i = 0; i < pe_nt->FileHeader.NumberOfSections; i++)
      if (     rva >= pe_sec[i].VirtualAddress
            && rva <  pe_sec[i].VirtualAddress + pe_sec[i].SizeOfRawData)
         return pe_file + pe_sec[i].PointerToRawData
            + (rva - pe_sec[i].VirtualAddress);
   return NULL;
}

/* The function holding code at rva (its first part, for a split one). */
static const RUNTIME_FUNCTION *pe_function(uint32_t rva)
{
   const IMAGE_DATA_DIRECTORY *dir =
      &pe_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
   const RUNTIME_FUNCTION *f = (const RUNTIME_FUNCTION*)pe_rva(dir->VirtualAddress);
   size_t lo = 0, hi = f ? dir->Size / sizeof(*f) : 0;
   const RUNTIME_FUNCTION *hit = NULL;

   while (lo < hi)
   {
      size_t mid = (lo + hi) / 2;
      if (rva < f[mid].BeginAddress)
         hi = mid;
      else if (rva >= f[mid].EndAddress)
         lo = mid + 1;
      else
      {
         hit = &f[mid];
         break;
      }
   }
   /* UNW_FLAG_CHAININFO: the part's unwind data ends with its parent. */
   while (hit)
   {
      const uint8_t *ui = pe_rva(hit->UnwindData);
      if (!ui || !((ui[0] >> 3) & 4))
         break;
      hit = (const RUNTIME_FUNCTION*)(ui + 4 + ((ui[2] + 1) & ~1) * 2);
   }
   return hit;
}

/* Network::Init's offset in the exe, 0 if not found (or not only one). */
static uint32_t find_network_init(const wchar_t *exe)
{
   static const char msg[] = "Error initializing ENet";
   HANDLE h;
   DWORD size, got;
   int64_t str = -1;
   uint32_t found = 0;
   int i, hits = 0;

   h = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
   if (h == INVALID_HANDLE_VALUE)
      return 0;
   size = GetFileSize(h, NULL);
   if (size == INVALID_FILE_SIZE || !(pe_file = (uint8_t*)malloc(size)))
   {
      CloseHandle(h);
      return 0;
   }
   if (!ReadFile(h, pe_file, size, &got, NULL) || got != size || size < 0x400)
      goto done;
   pe_nt = (IMAGE_NT_HEADERS64*)(pe_file + ((IMAGE_DOS_HEADER*)pe_file)->e_lfanew);
   if (     (uint8_t*)pe_nt + sizeof(*pe_nt) > pe_file + size
         || pe_nt->Signature != IMAGE_NT_SIGNATURE
         || pe_nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
      goto done;
   pe_sec = IMAGE_FIRST_SECTION(pe_nt);

   /* The message, in the data sections. */
   for (i = 0; i < pe_nt->FileHeader.NumberOfSections && str < 0; i++)
   {
      uint8_t *p   = pe_file + pe_sec[i].PointerToRawData;
      uint32_t len = pe_sec[i].SizeOfRawData, k;
      if (     (pe_sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            || pe_sec[i].PointerToRawData + len > size)
         continue;
      for (k = 0; k + sizeof(msg) <= len; k++)
         if (!memcmp(p + k, msg, sizeof(msg)))
         {
            str = (int64_t)pe_sec[i].VirtualAddress + k;
            break;
         }
   }
   if (str < 0)
      goto done;

   /* Code loading its address: lea r64, [rip + disp32]. */
   for (i = 0; i < pe_nt->FileHeader.NumberOfSections; i++)
   {
      uint8_t *p   = pe_file + pe_sec[i].PointerToRawData;
      uint32_t len = pe_sec[i].SizeOfRawData, k;
      if (     !(pe_sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            || pe_sec[i].PointerToRawData + len > size)
         continue;
      for (k = 0; k + 7 <= len; k++)
      {
         int32_t disp;
         uint32_t at;
         const RUNTIME_FUNCTION *f;
         if (     (p[k] != 0x48 && p[k] != 0x4C) || p[k + 1] != 0x8D
               || (p[k + 2] & 0xC7) != 0x05)
            continue;
         memcpy(&disp, p + k + 3, 4);
         at = pe_sec[i].VirtualAddress + k;
         if ((int64_t)at + 7 + disp != str || !(f = pe_function(at)))
            continue;
         if (f->BeginAddress != found)
            hits++;
         found = f->BeginAddress;
      }
   }
   if (hits != 1)
      found = 0;

done:
   CloseHandle(h);
   free(pe_file);
   pe_file = NULL;
   return found;
}

static uint32_t network_init_rva; /* 0: not in multiplayer */

/* Runs Network::Init in Eden. */
static void network_init(HANDLE process)
{
   typedef LONG (WINAPI *query_t)(HANDLE, int, PVOID, ULONG, PULONG);
   struct { PVOID exit_status; uint8_t *peb; ULONG_PTR more[4]; } info;
   query_t query  = (query_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
         "NtQueryInformationProcess");
   uint64_t base  = 0;
   HANDLE thread;
   DWORD code     = 0;

   if (     !query || query(process, 0, &info, sizeof(info), NULL) != 0
         || !ReadProcessMemory(process, info.peb + 0x10, &base, sizeof(base), NULL))
   {
      log_msg("multiplayer: Eden's image base not read (%lu)", (unsigned long)GetLastError());
      return;
   }
   thread = CreateRemoteThread(process, NULL, 0,
         (LPTHREAD_START_ROUTINE)(ULONG_PTR)(base + network_init_rva), NULL, 0, NULL);
   if (!thread)
   {
      log_msg("multiplayer: Eden's network not started (%lu)", (unsigned long)GetLastError());
      return;
   }
   WaitForSingleObject(thread, 10000);
   GetExitCodeThread(thread, &code);
   CloseHandle(thread);
   log_msg("multiplayer: Eden's network started (%lu)", (unsigned long)(code & 0xff));
}

/* ---------------------------------------------------------------------- */
/* Switch keyboard                                                        */
/* ---------------------------------------------------------------------- */

/* eden-cli has no keyboard of its own (it answers every one with "Eden"):
 * eden_kbd.dll, next to this core, is loaded into it and sends each
 * keyboard the game opens here, through a pipe named after Eden's process;
 * RetroArch asks the player with its on-screen keyboard
 * (bomba_text_input.h) and the answer goes back the same way. */
#define KBD_TEXT_MAX 500 /* UTF-16 units, as eden_kbd.c */

static struct
{
   HANDLE thread;
   HANDLE answered;   /* event: RetroArch answered */
   volatile LONG stop;
   CRITICAL_SECTION lock;
   bool lock_init;
   bool pending;      /* asked, for retro_run to pass on to RetroArch */
   char label[KBD_TEXT_MAX * 3 + 1];
   char initial[KBD_TEXT_MAX * 3 + 1];
   bool ok;           /* the answer: typed (else cancelled) */
   wchar_t text[KBD_TEXT_MAX];
   uint32_t text_len;
} kbd;

static void kbd_utf8(const wchar_t *w, uint32_t n, char *out, size_t out_len)
{
   int len = n ? WideCharToMultiByte(CP_UTF8, 0, w, (int)n, out,
         (int)out_len - 1, NULL, NULL) : 0;
   out[len > 0 ? len : 0] = '\0';
}

/* RetroArch's answer, on its main thread. */
static void kbd_done(const char *text)
{
   int n = 0;

   EnterCriticalSection(&kbd.lock);
   kbd.ok = text != NULL;
   if (text && *text)
      n = MultiByteToWideChar(CP_UTF8, 0, text, -1, kbd.text, KBD_TEXT_MAX) - 1;
   kbd.text_len = n > 0 ? (uint32_t)n : 0;
   LeaveCriticalSection(&kbd.lock);
   log_msg("keyboard: %s", text ? "typed" : "cancelled");
   SetEvent(kbd.answered);
}

/* Each retro_run: a keyboard asked for goes to RetroArch. Without
 * RetroArch's keyboard, the game gets the text it started from. */
static void kbd_run(void)
{
   struct bomba_text_input req;
   bool pending;

   if (!kbd.lock_init)
      return;
   EnterCriticalSection(&kbd.lock);
   pending     = kbd.pending;
   kbd.pending = false;
   LeaveCriticalSection(&kbd.lock);
   if (!pending)
      return;

   req.label   = kbd.label;
   req.initial = kbd.initial;
   req.done    = kbd_done;
   if (!env_cb(RETRO_ENVIRONMENT_BOMBA_TEXT_INPUT, &req))
   {
      log_msg("keyboard: RetroArch can't ask; the game's own text kept");
      kbd_done(kbd.initial);
   }
}

static bool kbd_io(HANDLE pipe, HANDLE ev, void *buf, DWORD len, bool write)
{
   while (len)
   {
      OVERLAPPED o;
      DWORD done = 0;

      memset(&o, 0, sizeof(o));
      o.hEvent = ev;
      if (     !(write ? WriteFile(pipe, buf, len, NULL, &o)
                       : ReadFile(pipe, buf, len, NULL, &o))
            && GetLastError() != ERROR_IO_PENDING)
         return false;
      while (WaitForSingleObject(ev, 100) == WAIT_TIMEOUT)
         if (kbd.stop)
         {
            CancelIo(pipe);
            return false;
         }
      if (!GetOverlappedResult(pipe, &o, &done, FALSE) || !done)
         return false;
      buf  = (uint8_t*)buf + done;
      len -= done;
   }
   return true;
}

/* The pipe: one keyboard at a time, until the core unloads. */
static DWORD WINAPI kbd_main(LPVOID param)
{
   DWORD pid = (DWORD)(uintptr_t)param;
   HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
   wchar_t name[64], label[KBD_TEXT_MAX], initial[KBD_TEXT_MAX];
   uint32_t head[4], reply[2];

   _snwprintf(name, 64, L"\\\\.\\pipe\\bomba-eden-kbd-%lu", (unsigned long)pid);
   name[63] = L'\0';
   while (!kbd.stop && ev)
   {
      HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
      OVERLAPPED o;
      bool connected;

      if (pipe == INVALID_HANDLE_VALUE)
      {
         log_msg("keyboard: pipe not made (%lu)", (unsigned long)GetLastError());
         break;
      }
      memset(&o, 0, sizeof(o));
      o.hEvent  = ev;
      ResetEvent(ev);
      connected = ConnectNamedPipe(pipe, &o) || GetLastError() == ERROR_PIPE_CONNECTED;
      if (!connected && GetLastError() == ERROR_IO_PENDING)
      {
         DWORD unused;
         while (WaitForSingleObject(ev, 100) == WAIT_TIMEOUT && !kbd.stop);
         if (kbd.stop)
            CancelIo(pipe);
         connected = !kbd.stop && GetOverlappedResult(pipe, &o, &unused, FALSE);
      }

      if (     connected
            && kbd_io(pipe, ev, head, sizeof(head), false)
            && head[0] == 0x31424B42 /* "BKB1" */
            && head[2] <= KBD_TEXT_MAX && head[3] <= KBD_TEXT_MAX
            && kbd_io(pipe, ev, label, head[2] * 2, false)
            && kbd_io(pipe, ev, initial, head[3] * 2, false))
      {
         log_msg("keyboard: the game asks (\"%.*ls\", up to %u characters)",
               (int)head[2], label, head[1]);
         EnterCriticalSection(&kbd.lock);
         kbd_utf8(label, head[2], kbd.label, sizeof(kbd.label));
         kbd_utf8(initial, head[3], kbd.initial, sizeof(kbd.initial));
         ResetEvent(kbd.answered);
         kbd.pending = true;
         LeaveCriticalSection(&kbd.lock);

         while (WaitForSingleObject(kbd.answered, 100) == WAIT_TIMEOUT && !kbd.stop);
         if (!kbd.stop)
         {
            EnterCriticalSection(&kbd.lock);
            reply[0] = kbd.ok;
            reply[1] = kbd.text_len;
            LeaveCriticalSection(&kbd.lock);
            if (     kbd_io(pipe, ev, reply, sizeof(reply), true)
                  && kbd_io(pipe, ev, kbd.text, reply[1] * 2, true))
               FlushFileBuffers(pipe);
         }
      }
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
   }
   if (ev)
      CloseHandle(ev);
   return 0;
}

static void kbd_start(DWORD pid)
{
   if (!kbd.lock_init)
   {
      InitializeCriticalSection(&kbd.lock);
      kbd.lock_init = true;
   }
   kbd.pending = false;
   kbd.stop    = 0;
   if (!kbd.answered)
      kbd.answered = CreateEventW(NULL, TRUE, FALSE, NULL);
   if (kbd.answered && !kbd.thread)
      kbd.thread = CreateThread(NULL, 0, kbd_main, (LPVOID)(uintptr_t)pid, 0, NULL);
}

static void kbd_stop(void)
{
   if (!kbd.thread)
      return;
   kbd.stop = 1;
   if (WaitForSingleObject(kbd.thread, 3000) != WAIT_OBJECT_0)
      log_msg("keyboard thread didn't stop in time");
   CloseHandle(kbd.thread);
   kbd.thread = NULL;
}

/* Loads eden_kbd.dll (next to this core) into Eden. */
static void kbd_inject(HANDLE process)
{
   wchar_t path[MAX_PATH * 2], *slash;
   HMODULE self = NULL;
   size_t size;
   void *remote;
   HANDLE thread;
   DWORD code = 0;

   if (     !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
               (LPCWSTR)(void*)kbd_inject, &self)
         || !GetModuleFileNameW(self, path, MAX_PATH * 2)
         || !(slash = wcsrchr(path, L'\\')))
      return;
   wcscpy(slash + 1, L"eden_kbd.dll");
   if (!file_exists(path))
   {
      log_msg("keyboard: eden_kbd.dll not found");
      return;
   }

   size = (wcslen(path) + 1) * sizeof(wchar_t);
   if (!(remote = VirtualAllocEx(process, NULL, size, MEM_COMMIT | MEM_RESERVE,
               PAGE_READWRITE)))
      return;
   if (     WriteProcessMemory(process, remote, path, size, NULL)
         && (thread = CreateRemoteThread(process, NULL, 0,
               (LPTHREAD_START_ROUTINE)(void*)GetProcAddress(
                  GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"),
               remote, 0, NULL)))
   {
      WaitForSingleObject(thread, 10000);
      GetExitCodeThread(thread, &code);
      CloseHandle(thread);
   }
   VirtualFreeEx(process, remote, 0, MEM_RELEASE);
   log_msg(code ? "keyboard: hooked into Eden"
                : "keyboard: Eden's keyboard not found; it answers \"Eden\"");
}

/* Once Eden runs (the overlay starts it suspended), after its C runtime
 * is up and well before the game loads: its network (multiplayer), and
 * the keyboard. */
static DWORD WINAPI emu_started_main(LPVOID param)
{
   HANDLE process = (HANDLE)param;
   int i;

   for (i = 0; i < 1200; i++)
   {
      FILETIME c, e, k, u;
      if (     GetProcessTimes(process, &c, &e, &k, &u)
            && (u.dwLowDateTime || u.dwHighDateTime))
         break;
      if (WaitForSingleObject(process, 50) == WAIT_OBJECT_0)
         goto out;
   }
   Sleep(200);

   if (network_init_rva)
      network_init(process);
   kbd_inject(process);
out:
   CloseHandle(process);
   return 0;
}

static void start_emu_helpers(HANDLE process)
{
   HANDLE dup, thread;
   if (!DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &dup,
         0, FALSE, DUPLICATE_SAME_ACCESS))
      return;
   if ((thread = CreateThread(NULL, 0, emu_started_main, dup, 0, NULL)))
      CloseHandle(thread);
   else
      CloseHandle(dup);
}

/* ---------------------------------------------------------------------- */
/* Loading screen                                                         */
/* ---------------------------------------------------------------------- */

/* The core's frame until Eden's game shows: the game's name and
 * "Loading...", in the grid's font, sizes and text colours (grid_home.c)
 * on black, drawn with GDI. */
#define LD_FONT_CAP  0.60f      /* Baloo 2 cap height, in em */
#define LD_TEXT      0xececec   /* COL_TEXT */
#define LD_TEXT_DIM  0x8c8c90   /* COL_TEXT_DIM */

static struct
{
   HDC dc;
   HBITMAP bmp;
   HGDIOBJ old;
   uint32_t *bits;      /* XRGB8888, fb_width x fb_height */
   uint32_t *faded;     /* bits darkened, while fading */
   HFONT title_font, text_font;
   wchar_t title[256];
   wchar_t fonts[2][MAX_PATH * 2];
   unsigned fonts_added; /* bit per entry of fonts */
   int dots;             /* dots drawn last; -1 nothing drawn yet */
   DWORD shown_at;       /* tick of the first frame, 0 none yet */
   bool from_grid;       /* the grid's loading screen came before */
} ld;

/* Reference pixels (grid: 423 on the screen's height) to this frame's. */
static int ld_ref(float px)
{
   return (int)(px * (float)fb_height / 423.0f + 0.5f);
}

static HFONT ld_font(float cap, int weight, const wchar_t *face)
{
   return CreateFontW(-ld_ref(cap / LD_FONT_CAP), 0, 0, 0, weight,
         FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
         CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
}

static COLORREF ld_rgb(uint32_t c)
{
   return RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
}

static void loading_close(void)
{
   unsigned i;

   if (ld.dc)
   {
      if (ld.old)
         SelectObject(ld.dc, ld.old);
      DeleteDC(ld.dc);
   }
   if (ld.bmp)
      DeleteObject(ld.bmp);
   free(ld.faded);
   if (ld.title_font)
      DeleteObject(ld.title_font);
   if (ld.text_font)
      DeleteObject(ld.text_font);
   for (i = 0; i < 2; i++)
      if (ld.fonts_added & (1u << i))
         RemoveFontResourceExW(ld.fonts[i], FR_PRIVATE, NULL);
   memset(&ld, 0, sizeof(ld));
}

/* Sizes the frame to RetroArch's window (drawn pixel for pixel there, so
 * it lines up with the grid's loading screen before it), or else the
 * screen it's on, and gets the drawing ready. The title is the file's
 * name without extension and tags ("[...]", "(...)"). */
static void loading_open(const wchar_t *rom)
{
   static const wchar_t *files[2] = { L"Baloo2-Bold.ttf", L"Baloo2-Medium.ttf" };
   MONITORINFO mi;
   RECT client;
   HWND fg = GetForegroundWindow();
   DWORD fg_pid = 0;
   const wchar_t *name = rom, *p;
   wchar_t dir[MAX_PATH * 2], *cut;
   BITMAPINFO bi;
   unsigned i;

   loading_close();
   fb_width  = 1280;
   fb_height = 720;
   mi.cbSize = sizeof(mi);
   if (fg)
      GetWindowThreadProcessId(fg, &fg_pid);
   if (     fg_pid == GetCurrentProcessId() && !IsIconic(fg)
         && GetClientRect(fg, &client)
         && client.right >= 320 && client.bottom >= 180)
   {
      fb_width  = (unsigned)client.right;
      fb_height = (unsigned)client.bottom;
   }
   else if (GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY), &mi)
         && mi.rcMonitor.right > mi.rcMonitor.left
         && mi.rcMonitor.bottom > mi.rcMonitor.top)
   {
      fb_width  = (unsigned)(mi.rcMonitor.right - mi.rcMonitor.left);
      fb_height = (unsigned)(mi.rcMonitor.bottom - mi.rcMonitor.top);
   }
   if (fb_width > 7680 || fb_height > 4320)
   {
      fb_width  = 1920;
      fb_height = 1080;
   }

   /* Started from the grid: its loading screen was up already, with the
    * name it gives the game (BOMBA_GAME_TITLE); this one carries on from
    * it, no fade in. */
   {
      char grid_title[512];
      DWORD n = GetEnvironmentVariableA("BOMBA_GAME_TITLE", grid_title,
            sizeof(grid_title));
      ld.from_grid = n > 0 && n < sizeof(grid_title)
         && to_wide(grid_title, ld.title, 256);
      SetEnvironmentVariableA("BOMBA_GAME_TITLE", NULL);
   }
   if (!ld.from_grid)
   {
      for (p = rom; *p; p++)
         if (*p == L'\\' || *p == L'/')
            name = p + 1;
      wcsncpy(ld.title, name, 255);
      ld.title[255] = L'\0';
      if ((cut = wcsrchr(ld.title, L'.')))
         *cut = L'\0';
      if ((cut = wcsstr(ld.title, L" [")))
         *cut = L'\0';
      if ((cut = wcsstr(ld.title, L" (")))
         *cut = L'\0';
   }

   /* Baloo 2 ships next to the exe (see build.sh). */
   if (GetModuleFileNameW(NULL, dir, MAX_PATH * 2) && (cut = wcsrchr(dir, L'\\')))
   {
      *cut = L'\0';
      for (i = 0; i < 2; i++)
      {
         _snwprintf(ld.fonts[i], MAX_PATH * 2, L"%ls\\bomba\\fonts\\%ls", dir, files[i]);
         ld.fonts[i][MAX_PATH * 2 - 1] = L'\0';
         if (AddFontResourceExW(ld.fonts[i], FR_PRIVATE, NULL) > 0)
            ld.fonts_added |= 1u << i;
      }
   }
   ld.title_font = ld_font(13, FW_BOLD,
         (ld.fonts_added & 1) ? L"Baloo 2" : L"Segoe UI");
   ld.text_font  = ld_font(9, FW_NORMAL,
         (ld.fonts_added & 2) ? L"Baloo 2 Medium" : L"Segoe UI");

   memset(&bi, 0, sizeof(bi));
   bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
   bi.bmiHeader.biWidth       = (LONG)fb_width;
   bi.bmiHeader.biHeight      = -(LONG)fb_height; /* top-down */
   bi.bmiHeader.biPlanes      = 1;
   bi.bmiHeader.biBitCount    = 32;
   bi.bmiHeader.biCompression = BI_RGB;
   if (     !(ld.dc  = CreateCompatibleDC(NULL))
         || !(ld.bmp = CreateDIBSection(ld.dc, &bi, DIB_RGB_COLORS,
               (void**)&ld.bits, NULL, 0))
         || !(ld.faded = (uint32_t*)malloc((size_t)fb_width * fb_height
               * sizeof(uint32_t))))
   {
      log_msg("loading screen: GDI failed (%lu)", (unsigned long)GetLastError());
      loading_close();
      return;
   }
   ld.old  = SelectObject(ld.dc, ld.bmp);
   ld.dots = -1;
   SetBkMode(ld.dc, TRANSPARENT);
}

/* Black, the title centered, and
 * "Loading" with 0-3 dots under it, the dots not moving the word. */
static void loading_draw(int dots)
{
   int title_y = ld_ref(208), text_y = ld_ref(232);
   int max_w   = (int)fb_width - 2 * ld_ref(35);
   wchar_t buf[260];
   int len;
   SIZE sz;

   memset(ld.bits, 0, (size_t)fb_width * fb_height * sizeof(uint32_t));

   /* The title, cut short with "..." if too wide. */
   SelectObject(ld.dc, ld.title_font);
   SetTextColor(ld.dc, ld_rgb(LD_TEXT));
   SetTextAlign(ld.dc, TA_CENTER | TA_BASELINE);
   len = (int)wcslen(ld.title);
   wcscpy(buf, ld.title);
   while (     len > 0 && GetTextExtentPoint32W(ld.dc, buf, (int)wcslen(buf), &sz)
            && sz.cx > max_w)
   {
      len--;
      while (len > 0 && ld.title[len - 1] == L' ')
         len--;
      wcsncpy(buf, ld.title, len);
      wcscpy(buf + len, L"...");
   }
   TextOutW(ld.dc, (int)fb_width / 2, title_y, buf, (int)wcslen(buf));

   SelectObject(ld.dc, ld.text_font);
   SetTextColor(ld.dc, ld_rgb(LD_TEXT_DIM));
   SetTextAlign(ld.dc, TA_LEFT | TA_BASELINE);
   GetTextExtentPoint32W(ld.dc, L"Loading...", 10, &sz);
   TextOutW(ld.dc, ((int)fb_width - sz.cx) / 2, text_y, L"Loading...", 7 + dots);
   GdiFlush();
}

/* How much of the loading screen shows (0 black .. 256 all): fading in
 * from its first frame, out once Eden's window is taken over. */
static unsigned loading_level(DWORD now)
{
   DWORD in  = now - ld.shown_at;
   DWORD out = ov.styled ? now - ov.styled_at : 0;
   unsigned level = (ld.from_grid || in >= FADE_MS) ? 256 : in * 256 / FADE_MS;

   if (ov.styled)
      level = out >= FADE_MS ? 0 : level * (FADE_MS - out) / FADE_MS;
   return level;
}

/* Each retro_run: the loading screen until RetroArch turns see-through
 * (its frame isn't drawn then), the dots stepping on. */
static void loading_frame(void)
{
   DWORD now   = GetTickCount();
   int dots    = (int)(now / 400 % 4);
   size_t pitch = fb_width * sizeof(uint32_t);
   unsigned level;

   if (!ld.bits || ov.transparent)
   {
      video_cb(NULL, fb_width, fb_height, pitch);
      return;
   }
   if (!ld.shown_at)
      ld.shown_at = now ? now : 1;
   if (dots != ld.dots)
   {
      loading_draw(dots);
      ld.dots = dots;
   }

   if ((level = loading_level(now)) >= 256)
      video_cb(ld.bits, fb_width, fb_height, pitch);
   else
   {
      size_t i, n = (size_t)fb_width * fb_height;
      for (i = 0; i < n; i++)
      {
         uint32_t c = ld.bits[i];
         ld.faded[i] = (((c & 0xff00ffu) * level >> 8) & 0xff00ffu)
                     | (((c & 0x00ff00u) * level >> 8) & 0x00ff00u);
      }
      video_cb(ld.faded, fb_width, fb_height, pitch);
   }
}

/* ---------------------------------------------------------------------- */
/* Overlay mode                                                           */
/* ---------------------------------------------------------------------- */

static BOOL CALLBACK find_ra_window(HWND hwnd, LPARAM out)
{
   DWORD pid = 0;
   GetWindowThreadProcessId(hwnd, &pid);
   if (pid == GetCurrentProcessId() && GetPropA(hwnd, "BombaOverlay"))
   {
      *(HWND*)out = hwnd;
      return FALSE;
   }
   return TRUE;
}

struct emu_window_search { HWND best; LONGLONG area; };

/* Eden's largest visible top-level window: its game window. Before its
 * window is taken over, a hidden one counts too (Eden started hidden,
 * see launch_main), if game-window sized. */
static BOOL CALLBACK find_emu_window(HWND hwnd, LPARAM data)
{
   struct emu_window_search *s = (struct emu_window_search*)data;
   DWORD pid = 0;
   RECT r;
   LONGLONG area;

   GetWindowThreadProcessId(hwnd, &pid);
   if (     pid != emu_pid || GetWindow(hwnd, GW_OWNER)
         || !GetWindowRect(hwnd, &r))
      return TRUE;
   if (     !IsWindowVisible(hwnd)
         && (ov.emu || r.right - r.left < 640 || r.bottom - r.top < 360))
      return TRUE;

   area = (LONGLONG)(r.right - r.left) * (r.bottom - r.top);
   if (area > s->area)
   {
      s->best = hwnd;
      s->area = area;
   }
   return TRUE;
}

static HWND current_emu_window(void)
{
   struct emu_window_search s = { NULL, 0 };
   EnumWindows(find_emu_window, (LPARAM)&s);
   return s.best;
}

/* A dialog Eden shows over the game (the Switch keyboard, an error): a
 * visible window of its other than the game window, owned by one of its
 * windows or always on top. */
static BOOL CALLBACK find_emu_popup(HWND hwnd, LPARAM out)
{
   DWORD pid = 0;

   GetWindowThreadProcessId(hwnd, &pid);
   if (     pid != emu_pid || hwnd == ov.emu || !IsWindowVisible(hwnd)
         || (     !GetWindow(hwnd, GW_OWNER)
               && !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)))
      return TRUE;
   *(HWND*)out = hwnd;
   return FALSE;
}

static HWND current_emu_popup(void)
{
   HWND popup = NULL;
   EnumWindows(find_emu_popup, (LPARAM)&popup);
   return popup;
}

static bool try_foreground_window(HWND hwnd)
{
   HWND fg       = GetForegroundWindow();
   DWORD fg_tid  = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
   DWORD our_tid = GetCurrentThreadId();

   if (fg_tid && fg_tid != our_tid)
      AttachThreadInput(our_tid, fg_tid, TRUE);
   SetForegroundWindow(hwnd);
   if (fg_tid && fg_tid != our_tid)
      AttachThreadInput(our_tid, fg_tid, FALSE);
   return GetForegroundWindow() == hwnd;
}

static bool try_foreground(void)
{
   return try_foreground_window(ov.ra);
}

/* Brings RetroArch's window to the front with focus. Windows only lets a
 * process do that if it got the last input, so if asking plainly fails, a
 * press of Alt is simulated first. */
static void focus_ra(void)
{
   INPUT in[2];

   if (GetForegroundWindow() == ov.ra || try_foreground())
      return;

   memset(in, 0, sizeof(in));
   in[0].type       = INPUT_KEYBOARD;
   in[0].ki.wVk     = VK_MENU;
   in[1]            = in[0];
   in[1].ki.dwFlags = KEYEVENTF_KEYUP;
   SendInput(2, in, sizeof(INPUT));

   log_msg("focus RetroArch: %s", try_foreground() ? "ok" : "refused");
}

static void set_ra_transparent(bool on)
{
   DWM_BLURBEHIND bb;
   HRGN region = NULL;

   memset(&bb, 0, sizeof(bb));
   bb.dwFlags = DWM_BB_ENABLE;
   bb.fEnable = on;
   if (on)
   {
      /* An empty blur region: no blur, just per-pixel alpha. */
      region       = CreateRectRgn(0, 0, -1, -1);
      bb.dwFlags  |= DWM_BB_BLURREGION;
      bb.hRgnBlur  = region;
   }
   log_msg("transparency %s: DwmEnableBlurBehindWindow hr=0x%08lx",
         on ? "on" : "off", (unsigned long)DwmEnableBlurBehindWindow(ov.ra, &bb));
   if (region)
      DeleteObject(region);

   /* BombaFade: when the see-through frame started, so the driver fades
    * black off Eden from then (the loading screen faded out to black). */
   if (on)
   {
      SetPropA(ov.ra, "BombaFade", (HANDLE)(uintptr_t)(GetTickCount() | 1));
      SetPropA(ov.ra, "BombaTransparent", (HANDLE)1);
   }
   else
   {
      RemovePropA(ov.ra, "BombaTransparent");
      RemovePropA(ov.ra, "BombaFade");
   }
   ov.transparent = on;
}

static void place_emu(bool zorder);

/* Eden's window as part of RetroArch's: no border or caption, no taskbar
 * / Alt-Tab entry (a tool window), never activated. Hidden and shown
 * again so the taskbar notices; placed in between, so it never shows
 * anywhere else. */
static void style_emu(bool zorder)
{
   LONG_PTR style = GetWindowLongPtrW(ov.emu, GWL_STYLE);
   LONG_PTR ex    = GetWindowLongPtrW(ov.emu, GWL_EXSTYLE);

   ShowWindow(ov.emu, SW_HIDE);
   SetWindowLongPtrW(ov.emu, GWL_STYLE,
         style & ~(WS_CAPTION | WS_THICKFRAME));
   SetWindowLongPtrW(ov.emu, GWL_EXSTYLE,
         (ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW);
   SetWindowPos(ov.emu, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE
         | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
   log_msg("Eden window %p styled (style %08lx -> %08lx)", (void*)ov.emu,
         (unsigned long)style, (unsigned long)GetWindowLongPtrW(ov.emu, GWL_STYLE));
   memset(&ov.placed, 0, sizeof(ov.placed));
   place_emu(zorder);
   ShowWindow(ov.emu, SW_SHOWNOACTIVATE);
}

/* Puts Eden's window exactly under RetroArch's client area, directly
 * below it in the window order (zorder; while loading only the place,
 * RetroArch's window being topmost). Invisible borders the window may
 * still have are measured (DWM's visible bounds) and placed outside.
 * Placed again if Eden moved or resized it since. */
static void place_emu(bool zorder)
{
   const UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER
      | (zorder ? 0 : SWP_NOZORDER);
   HWND after = zorder ? ov.ra : NULL;
   RECT target, win, vis;
   POINT tl = { 0, 0 };

   if (!GetClientRect(ov.ra, &target) || !ClientToScreen(ov.ra, &tl))
      return;
   OffsetRect(&target, tl.x, tl.y);
   if (target.right <= target.left || target.bottom <= target.top)
      return;

   if (GetWindowRect(ov.emu, &win) && !EqualRect(&win, &ov.emu_rect))
      memset(&ov.placed, 0, sizeof(ov.placed));

   if (!EqualRect(&target, &ov.placed))
   {
      SetWindowPos(ov.emu, after, target.left, target.top,
            target.right - target.left, target.bottom - target.top, flags);

      if (     GetWindowRect(ov.emu, &win)
            && SUCCEEDED(DwmGetWindowAttribute(ov.emu,
                  DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))
            && !EqualRect(&vis, &target))
      {
         LONG l = vis.left - win.left, t = vis.top - win.top;
         LONG r = win.right - vis.right, b = win.bottom - vis.bottom;
         SetWindowPos(ov.emu, after, target.left - l, target.top - t,
               target.right - target.left + l + r,
               target.bottom - target.top + t + b, flags);
         log_msg("Eden window borders %ld,%ld,%ld,%ld compensated", l, t, r, b);
      }
      if (GetWindowRect(ov.emu, &win))
      {
         log_msg("Eden window placed at %ld,%ld %ldx%ld (RetroArch %ld,%ld %ldx%ld)",
               win.left, win.top, win.right - win.left, win.bottom - win.top,
               target.left, target.top,
               target.right - target.left, target.bottom - target.top);
         ov.emu_rect = win;
      }
      ov.placed = target;
   }
   else if (zorder)
      SetWindowPos(ov.emu, ov.ra, 0, 0, 0, 0,
            flags | SWP_NOMOVE | SWP_NOSIZE);
}

/* While loading RetroArch's window is topmost, so Eden's can't come over
 * its loading screen. */
static void set_ra_topmost(bool on)
{
   if (on == ov.topmost)
      return;
   SetWindowPos(ov.ra, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
   ov.topmost = on;
   log_msg("RetroArch window %s", on ? "over Eden's (loading)" : "no longer topmost");
}

/* Lets Eden run, started suspended so its window can't open before
 * RetroArch's is on top. */
static void release_emu_thread(void)
{
   HANDLE thread = (HANDLE)InterlockedExchangePointer(
         (PVOID volatile*)&ov.emu_thread, NULL);

   if (!thread)
      return;
   ResumeThread(thread);
   CloseHandle(thread);
   ov.loading_since = GetTickCount();
}

/* Eden's window title, without waiting on Eden (its window thread is
 * often busy while it boots). */
static bool emu_title(wchar_t *out, int len)
{
   return ov.emu && InternalGetWindowText(ov.emu, out, len) > 0;
}

/* Eden draws frames: its title's frame counter ("... | FPS: 30 (100%)",
 * updated every second or two) is above 0. Until then its window has
 * nothing in it (the desktop would show through). */
static bool emu_presenting(void)
{
   wchar_t title[256];
   const wchar_t *fps;

   return emu_title(title, 256) && (fps = wcsstr(title, L"FPS: "))
      && wcstod(fps + 5, NULL) >= 1.0;
}

/* The game shows: Eden has drawn frames for a moment. Failing that (no
 * frame counter in its title), its audio has run a good while. */
static bool emu_ready(DWORD now)
{
   if (emu_presenting())
   {
      if (!ov.frames_seen)
         ov.frames_seen = now ? now : 1;
      if (now - ov.frames_seen >= READY_AFTER_FRAMES_MS)
      {
         log_msg("Eden ready (drawing for %lu ms)", (unsigned long)(now - ov.frames_seen));
         return true;
      }
   }
   if (ov.audio_seen && now - ov.audio_seen >= READY_AFTER_AUDIO_MS)
   {
      log_msg("Eden ready (audio for %lu ms, no frame count seen)",
            (unsigned long)(now - ov.audio_seen));
      return true;
   }
   if (now - ov.loading_since >= LOADING_MAX_MS)
   {
      log_msg("Eden still loading after %u s: shown anyway", LOADING_MAX_MS / 1000);
      return true;
   }
   return false;
}

/* While loading: RetroArch keeps focus and stays over Eden, unless
 * another program was switched to; Eden's window stays where it will
 * show. */
static void hold_loading(DWORD now)
{
   HWND fg     = GetForegroundWindow();
   DWORD pid   = 0;
   bool tick   = now - ov.last_arrange >= ARRANGE_MS;

   if (tick)
   {
      ov.last_arrange = now;
      if (ov.emu && IsWindow(ov.emu))
         place_emu(false);
   }

   if (fg)
      GetWindowThreadProcessId(fg, &pid);
   if (pid == emu_pid)
   {
      set_ra_topmost(true);
      if (tick)
         focus_ra();
   }
   else if (pid)
      set_ra_topmost(pid == GetCurrentProcessId());
}

/* Diagnostics around the switch to see-through: what's on screen (five
 * points, as composed) and Eden's window state, every 50 ms. */
static void log_switch_state(void)
{
   static const float pts[5][2] = {
      { 0.5f, 0.5f }, { 0.1f, 0.1f }, { 0.9f, 0.1f }, { 0.1f, 0.9f }, { 0.9f, 0.9f }
   };
   static DWORD last;
   DWORD now = GetTickCount();
   HDC screen;
   RECT r;
   char px[128];
   int i, n = 0;
   DWORD cloaked = 0;
   HWND above;
   char above_txt[64] = "";

   if (now - last < 50 || !GetWindowRect(ov.ra, &r))
      return;
   last = now;
   px[0] = '\0';
   if ((screen = GetDC(NULL)))
   {
      for (i = 0; i < 5; i++)
      {
         COLORREF c = GetPixel(screen,
               r.left + (int)((r.right - r.left) * pts[i][0]),
               r.top + (int)((r.bottom - r.top) * pts[i][1]));
         n += snprintf(px + n, sizeof(px) - n, " %06lx", (unsigned long)c);
      }
      ReleaseDC(NULL, screen);
   }
   if (ov.emu)
      DwmGetWindowAttribute(ov.emu, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
   above = ov.emu ? GetWindow(ov.emu, GW_HWNDPREV) : NULL;
   snprintf(above_txt, sizeof(above_txt), "%p%s", (void*)above,
         above == ov.ra ? " (RetroArch)" : "");
   log_msg("diag: screen%s | Eden visible %d iconic %d cloaked %lu above %s | transparent %d",
         px, ov.emu ? IsWindowVisible(ov.emu) : -1, ov.emu ? IsIconic(ov.emu) : -1,
         (unsigned long)cloaked, above_txt, ov.transparent);
}

/* Eden's window title, logged as it changes while loading (tells how far
 * Eden has got). */
static void log_emu_title(void)
{
   static wchar_t last[256];
   wchar_t title[256];

   if (!emu_title(title, 256) || !wcscmp(title, last))
      return;
   wcscpy(last, title);
   log_msg("Eden window title: %ls", title);
}

/* Takes Eden's window over once its game runs, then keeps the two windows
 * together. Only while Eden runs: calls on its window wait on its
 * thread. */
static void arrange(void)
{
   DWORD now = GetTickCount();
   HWND hwnd, fg;
   DWORD fg_pid = 0;

   /* From the game being ready to 2 s after RetroArch went see-through. */
   if (ov.styled && now - ov.styled_at < SHOW_DELAY_MS + 2000)
      log_switch_state();

   if (!ov.transparent)
   {
      if (!ov.styled)
      {
         if ((hwnd = current_emu_window()) && hwnd != ov.emu)
         {
            RECT r;
            GetWindowRect(hwnd, &r);
            log_msg("Eden window %p (%ld,%ld %ldx%ld)", (void*)hwnd,
                  r.left, r.top, r.right - r.left, r.bottom - r.top);
            ov.emu = hwnd;
            /* Taken over at once, under the loading screen, so it has its
             * size and draws at it well before it shows. */
            style_emu(false);
         }
         log_emu_title();
         if (!ov.emu || !IsWindow(ov.emu) || !emu_ready(now))
         {
            hold_loading(now);
            return;
         }

         /* Under RetroArch's still opaque window as it stops being
          * topmost. */
         set_ra_topmost(false);
         place_emu(true);
         ov.styled    = true;
         ov.styled_at = now;
         return;
      }
      if (now - ov.styled_at < SHOW_DELAY_MS)
         return;

      place_emu(true);
      set_ra_transparent(true);
      LockSetForegroundWindow(LSFW_UNLOCK);
      focus_ra();
      ov.last_arrange = now;
      /* RetroArch was likely paused (inactive) until now: don't count
       * that as the menu being open. */
      EnterCriticalSection(&ov.lock);
      ov.last_run = now;
      LeaveCriticalSection(&ov.lock);
      return;
   }

   if (now - ov.last_arrange >= ARRANGE_MS)
   {
      ov.last_arrange = now;

      /* Eden can recreate its window: take the new one over too. */
      if ((hwnd = current_emu_window()) && hwnd != ov.emu && !ov.hidden)
      {
         log_msg("Eden window changed to %p", (void*)hwnd);
         ov.emu = hwnd;
         style_emu(true);
      }

      if (IsIconic(ov.ra))
      {
         if (!ov.hidden)
         {
            ShowWindow(ov.emu, SW_HIDE);
            ov.hidden = true;
         }
         return;
      }
      if (ov.hidden)
      {
         ShowWindow(ov.emu, SW_SHOWNOACTIVATE);
         ov.hidden = false;
         memset(&ov.placed, 0, sizeof(ov.placed));
      }
      place_emu(true);
   }

   /* A dialog of Eden's (an error): over RetroArch, with focus, until it
    * closes. */
   if ((hwnd = current_emu_popup()))
   {
      if (hwnd != ov.popup)
      {
         ov.popup = hwnd;
         SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
         log_msg("Eden dialog %p shown: %s", (void*)hwnd,
               GetForegroundWindow() == hwnd || try_foreground_window(hwnd)
               ? "focused" : "focus refused");
      }
      else
      {
         /* Back over RetroArch's window if that came over it. */
         HWND w;
         for (w = GetWindow(hwnd, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
            if (w == ov.ra)
            {
               SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE
                     | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
               break;
            }
      }
      return;
   }
   if (ov.popup)
   {
      ov.popup = NULL;
      log_msg("Eden dialog closed");
      focus_ra();
      return;
   }

   /* Eden got focus somehow: give it back to RetroArch. */
   if ((fg = GetForegroundWindow()))
      GetWindowThreadProcessId(fg, &fg_pid);
   if (fg_pid == emu_pid)
      focus_ra();
}

/* ---------------------------------------------------------------------- */
/* Volume: RetroArch's, applied to Eden live                              */
/* ---------------------------------------------------------------------- */

static const GUID clsid_device_enumerator =
   { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID iid_device_enumerator =
   { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID iid_session_manager2 =
   { 0x77AA99A0, 0x1BD6, 0x484F, { 0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F } };
static const GUID iid_session_control2 =
   { 0xBFB7FF88, 0x7239, 0x4FC9, { 0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D } };
static const GUID iid_simple_volume =
   { 0x87CE5498, 0x68D6, 0x44E5, { 0x92, 0x15, 0x6D, 0xA4, 0x7E, 0xF8, 0x83, 0xD8 } };

/* Sets the volume of Eden's audio session on the default output (Windows'
 * per-app volume, as in its volume mixer). False until Eden has opened its
 * audio. Called from the manager thread (COM initialized there). */
static bool set_emu_volume(float gain)
{
   IMMDeviceEnumerator *en      = NULL;
   IMMDevice *dev               = NULL;
   IAudioSessionManager2 *mgr   = NULL;
   IAudioSessionEnumerator *all = NULL;
   int i, n = 0;
   bool found = false;

   if (FAILED(CoCreateInstance(&clsid_device_enumerator, NULL, CLSCTX_ALL,
               &iid_device_enumerator, (void**)&en)))
      return false;

   if (     SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eMultimedia, &dev))
         && SUCCEEDED(IMMDevice_Activate(dev, &iid_session_manager2, CLSCTX_ALL, NULL, (void**)&mgr))
         && SUCCEEDED(IAudioSessionManager2_GetSessionEnumerator(mgr, &all))
         && SUCCEEDED(IAudioSessionEnumerator_GetCount(all, &n)))
   {
      for (i = 0; i < n; i++)
      {
         IAudioSessionControl *ctl   = NULL;
         IAudioSessionControl2 *ctl2 = NULL;
         ISimpleAudioVolume *vol     = NULL;
         DWORD pid                   = 0;

         if (FAILED(IAudioSessionEnumerator_GetSession(all, i, &ctl)))
            continue;
         if (     SUCCEEDED(IAudioSessionControl_QueryInterface(ctl, &iid_session_control2, (void**)&ctl2))
               && SUCCEEDED(IAudioSessionControl2_GetProcessId(ctl2, &pid))
               && pid == emu_pid
               && SUCCEEDED(IAudioSessionControl_QueryInterface(ctl, &iid_simple_volume, (void**)&vol)))
         {
            ISimpleAudioVolume_SetMasterVolume(vol, gain, NULL);
            ISimpleAudioVolume_SetMute(vol, gain <= 0.0f, NULL);
            ISimpleAudioVolume_Release(vol);
            found = true;
         }
         if (ctl2)
            IAudioSessionControl2_Release(ctl2);
         IAudioSessionControl_Release(ctl);
      }
   }

   if (all)
      IAudioSessionEnumerator_Release(all);
   if (mgr)
      IAudioSessionManager2_Release(mgr);
   if (dev)
      IMMDevice_Release(dev);
   IMMDeviceEnumerator_Release(en);
   return found;
}

/* RetroArch's volume (gain x 1000, 0 muted) as its window publishes it
 * (BombaVolume, see gl2.c), followed by Eden. Gains above 1 (a boost
 * in RetroArch) play at Eden's full volume. */
static void follow_volume(void)
{
   static long applied = -1;     /* gain x 1000 last applied to Eden */
   static DWORD last_try;
   intptr_t prop = ov.ra ? (intptr_t)GetPropA(ov.ra, "BombaVolume") : 0;
   long milli;
   DWORD now = GetTickCount();

   if (!prop || !emu_pid)
      return;
   milli = (long)(prop - 1);
   if (milli == applied || now - last_try < 500)
      return;
   last_try = now;

   if (set_emu_volume(milli >= 1000 ? 1.0f : (float)milli / 1000.0f))
   {
      if (!ov.audio_seen)
         ov.audio_seen = now ? now : 1;
      log_msg("volume: Eden at %ld%%", milli >= 1000 ? 100 : milli / 10);
      applied = milli;
   }
}

/* Manager thread. Runs even while RetroArch doesn't run the core (it
 * pauses while its window is inactive, which it is while Eden starts):
 * window handling, and pausing Eden while RetroArch pauses the core. */
static DWORD WINAPI manager_main(LPVOID unused)
{
   DWORD started = GetTickCount();
   bool gave_up  = false;
   bool com      = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)); /* volume */

   (void)unused;
   while (!ov.stop)
   {
      bool stalled;

      Sleep(50);

      /* RetroArch restarts its video driver after the core loads, so its
       * window advertises support only a moment later; and a driver
       * restart can recreate the window. */
      if (ov.ra && !IsWindow(ov.ra))
      {
         log_msg("RetroArch window went away; looking again");
         ov.ra          = NULL;
         ov.transparent = false;
         ov.topmost     = false;
         started        = GetTickCount();
      }
      if (!ov.ra)
      {
         EnumWindows(find_ra_window, (LPARAM)&ov.ra);
         if (ov.ra)
            log_msg("RetroArch window %p", (void*)ov.ra);
         else if (GetTickCount() - started > 10000)
         {
            /* Eden runs anyway, in its own window. */
            if (!gave_up)
               log_msg("RetroArch's window never advertised overlay support");
            gave_up = true;
            release_emu_thread();
         }
         continue;
      }

      /* Eden is started (see launch_main) and RetroArch's window is in
       * front: keep it there, with focus, while Eden boots. */
      if (ov.emu_thread)
      {
         set_ra_topmost(true);
         LockSetForegroundWindow(LSFW_LOCK);
         release_emu_thread();
         log_msg("Eden started under the loading screen");
      }

      if (com)
         follow_volume();

      EnterCriticalSection(&ov.lock);
      stalled = ov.transparent
         && GetTickCount() - ov.last_run > PAUSE_AFTER_MS;
      LeaveCriticalSection(&ov.lock);

      if (ov.suspended)
      {
         if (stalled)
            continue;
         ov.resume(emu_process);
         ov.suspended = false;
         log_msg("core running again: Eden resumed");
      }
      /* Not in multiplayer: a suspended Eden stops answering the room, and
       * the other players' games drop it (communication error). */
      /* Nor while a dialog of Eden's is up: RetroArch pauses as it loses
       * focus to it, and the game waits on it. */
      else if (stalled && ov.suspend && !mp.active && !ov.popup)
      {
         /* Minimized: hide Eden first, its window can't be handled while
          * suspended. */
         if (IsIconic(ov.ra) && !ov.hidden && ov.emu)
         {
            ShowWindow(ov.emu, SW_HIDE);
            ov.hidden = true;
         }
         ov.suspend(emu_process);
         ov.suspended = true;
         log_msg("RetroArch paused the core: Eden suspended");
         continue;
      }

      arrange();
   }
   release_emu_thread();
   if (com)
      CoUninitialize();
   return 0;
}

/* Before Eden starts (see launch_main), which hands over its main thread
 * suspended, to be resumed once RetroArch's window is on top. */
static void overlay_start(void)
{
   HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");

   memset(&ov.placed, 0, sizeof(ov.placed));
   memset(&ov.emu_rect, 0, sizeof(ov.emu_rect));
   ov.active        = true;
   ov.transparent   = false;
   ov.hidden        = false;
   ov.topmost       = false;
   ov.styled        = false;
   ov.ra            = NULL;
   ov.emu           = NULL;
   ov.popup         = NULL;
   ov.emu_thread    = NULL;
   ov.loading_since = GetTickCount();
   ov.audio_seen    = 0;
   ov.frames_seen   = 0;
   ov.styled_at     = 0;
   ov.last_arrange  = 0;

   if (!ov.lock_init)
   {
      InitializeCriticalSection(&ov.lock);
      ov.lock_init = true;
   }
   ov.suspend   = ntdll ? (nt_process_fn)(void*)GetProcAddress(ntdll, "NtSuspendProcess") : NULL;
   ov.resume    = ntdll ? (nt_process_fn)(void*)GetProcAddress(ntdll, "NtResumeProcess") : NULL;
   if (!ov.suspend || !ov.resume)
   {
      ov.suspend = ov.resume = NULL;
      log_msg("NtSuspendProcess unavailable: the game won't pause in the menu");
   }
   ov.suspended = false;
   ov.last_run  = GetTickCount();
   ov.stop      = 0;
   ov.thread    = CreateThread(NULL, 0, manager_main, NULL, 0, NULL);
   if (!ov.thread)
      release_emu_thread();
}

/* Each retro_run: RetroArch runs the core (the manager thread resumes Eden
 * if it was paused). */
static void overlay_run(void)
{
   EnterCriticalSection(&ov.lock);
   ov.last_run = GetTickCount();
   LeaveCriticalSection(&ov.lock);
}

static void overlay_stop(void)
{
   if (!ov.active)
      return;

   if (ov.thread)
   {
      ov.stop = 1;
      /* Bounded, so RetroArch can't hang on it. */
      if (WaitForSingleObject(ov.thread, 3000) != WAIT_OBJECT_0)
         log_msg("manager thread didn't stop in time");
      CloseHandle(ov.thread);
      ov.thread = NULL;
   }
   release_emu_thread();
   if (ov.suspended && emu_process)
      ov.resume(emu_process);
   ov.suspended = false;

   if (ov.topmost && IsWindow(ov.ra))
      set_ra_topmost(false);
   ov.topmost = false;
   LockSetForegroundWindow(LSFW_UNLOCK);
   if (ov.transparent && IsWindow(ov.ra))
      set_ra_transparent(false);
   ov.active = false;
   ov.emu    = NULL;
}

static BOOL CALLBACK close_window(HWND hwnd, LPARAM pid)
{
   DWORD window_pid = 0;
   GetWindowThreadProcessId(hwnd, &window_pid);
   if (window_pid == (DWORD)pid && IsWindowVisible(hwnd))
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
   return TRUE;
}

static void release_process(void)
{
   if (emu_process)
      CloseHandle(emu_process);
   emu_process = NULL;
   emu_pid     = 0;
}

/* ---------------------------------------------------------------------- */
/* Launch thread                                                          */
/* ---------------------------------------------------------------------- */

/* Starting Eden takes a few seconds (the room server, eden_padinfo), so
 * it's done on a thread of its own: RetroArch keeps running and shows the
 * loading screen meanwhile. Everything it needs from RetroArch is read
 * before (save folder, options, multiplayer); retro_run reports how it
 * went. */
enum { LAUNCH_RUNNING = 0, LAUNCH_DONE, LAUNCH_FAILED };

static struct
{
   HANDLE thread;
   wchar_t exe[MAX_PATH * 2];
   wchar_t cli[MAX_PATH * 2];
   bool use_cli;        /* eden-cli.exe plays, else eden.exe (no eden-cli) */
   wchar_t rom[MAX_PATH * 2];
   wchar_t workdir[MAX_PATH * 2];
   bool overlay;        /* hand Eden over suspended to the manager thread */
   bool fullscreen;
   volatile LONG state;
   const char *volatile message; /* for retro_run to show, NULL none */
} launch;

/* eden-cli can't open a game whose path isn't plain ASCII (it exits right
 * after opening its window, e.g. on "Super Mario™ 3D World"): it's given
 * a link to the game under a plain name in Eden's user folder instead, a
 * hard link (same drive) or else a symbolic one. Falls back to the game's
 * own path. */
static const wchar_t *cli_game_path(const wchar_t *rom, wchar_t *alias, size_t alias_len)
{
   const wchar_t *p, *ext;

   for (p = rom; *p && *p < 0x80; p++);
   if (!*p)
      return rom;

   ext = wcsrchr(rom, L'.');
   if (!ext || wcschr(ext, L'\\'))
      ext = L"";
   _snwprintf(alias, alias_len, L"%ls\\user\\cli-game%ls", launch.workdir, ext);
   alias[alias_len - 1] = L'\0';
   for (p = alias; *p && *p < 0x80; p++);
   if (*p)
      return rom;

   DeleteFileW(alias);
   if (CreateHardLinkW(alias, rom, NULL)
         || CreateSymbolicLinkW(alias, rom, 0x2 /* ALLOW_UNPRIVILEGED_CREATE */))
   {
      log_msg("multiplayer: game linked as %ls (eden-cli needs a plain path)", alias);
      return alias;
   }
   log_msg("multiplayer: couldn't link the game under a plain path (%lu)",
         (unsigned long)GetLastError());
   return rom;
}

/* The grid profile's name (BOMBA_PROFILE), read at load. */
static char profile_name[64];

/* A Switch user id as Eden names its save folders: the id's two
 * little-endian halves, high one first, in hex (32 digits). */
static void user_id_folder(const unsigned char id[16], wchar_t out[33])
{
   int i;
   for (i = 0; i < 16; i++)
      _snwprintf(out + 2 * i, 3, L"%02X", id[15 - i]);
   out[32] = L'\0';
}

/* The grid profile's own Switch user id, kept with its saves
 * (<save dir>\eden-user.txt: the save folder name), so it follows them to
 * other PCs (Steam Cloud) and every profile is a user of its own. The
 * first time: the id Eden has saved under in this profile already (its
 * save folder is there), unless that is Ryujinx's default (…0001, from a
 * migrated Ryujinx), which is every Ryujinx player's and which games that
 * tell players apart online refuse (Super Mario Odyssey Online); else a
 * new one. The save folder of an old id is renamed to the new one. */
static bool profile_user_id(const unsigned char *current, unsigned char id[16])
{
   static const unsigned char ryujinx[16] = { 1 };
   wchar_t dir[MAX_PATH * 2], path[MAX_PATH * 2], from[MAX_PATH * 2];
   wchar_t old_name[33], new_name[33];
   char hex[40] = {0};
   bool have = false;
   FILE *f;
   int i;

   if (!save_dir_cache[0] || !to_wide(save_dir_cache, dir, MAX_PATH * 2))
      return false;
   _snwprintf(path, MAX_PATH * 2, L"%ls\\eden-user.txt", dir);
   path[MAX_PATH * 2 - 1] = L'\0';

   /* Saved: the folder name back into the id's bytes. */
   if ((f = _wfopen(path, L"r")))
   {
      if (fgets(hex, sizeof(hex), f) && strlen(hex) >= 32)
      {
         have = true;
         for (i = 0; i < 16 && have; i++)
         {
            unsigned v;
            if (sscanf(hex + 2 * i, "%2x", &v) != 1)
               have = false;
            id[15 - i] = (unsigned char)v;
         }
      }
      fclose(f);
   }
   if (have)
      return true;

   user_id_folder(current, old_name);
   _snwprintf(from, MAX_PATH * 2, L"%ls\\save\\0000000000000000\\%ls", dir,
         old_name);
   from[MAX_PATH * 2 - 1] = L'\0';
   if (     memcmp(current, ryujinx, 16)
         && GetFileAttributesW(from) != INVALID_FILE_ATTRIBUTES)
      memcpy(id, current, 16);
   else
   {
      GUID g;
      if (FAILED(CoCreateGuid(&g)))
         return false;
      memcpy(id, &g, 16);
   }

   /* The saves under the old id (Ryujinx's in this profile) move over. */
   user_id_folder(id, new_name);
   if (memcmp(id, current, 16))
   {
      wchar_t ry[33], to[MAX_PATH * 2];
      user_id_folder(ryujinx, ry);
      _snwprintf(from, MAX_PATH * 2, L"%ls\\save\\0000000000000000\\%ls", dir, ry);
      from[MAX_PATH * 2 - 1] = L'\0';
      _snwprintf(to, MAX_PATH * 2, L"%ls\\save\\0000000000000000\\%ls", dir,
            new_name);
      to[MAX_PATH * 2 - 1] = L'\0';
      if (     GetFileAttributesW(from) != INVALID_FILE_ATTRIBUTES
            && GetFileAttributesW(to) == INVALID_FILE_ATTRIBUTES)
      {
         if (MoveFileW(from, to))
            log_msg("Switch saves moved to user %ls", new_name);
         else
         {
            log_msg("couldn't move the Switch saves to user %ls (%lu)",
                  new_name, (unsigned long)GetLastError());
            return false;
         }
      }
   }

   if (!(f = _wfopen(path, L"w")))
      return false;
   fprintf(f, "%ls\n", new_name);
   fclose(f);
   log_msg("Switch user id %ls", new_name);
   return true;
}

/* Sets Eden's Switch user up as the grid profile: its id (see
 * profile_user_id) and its name, the one games show, other players too.
 * Its first user is the one Eden plays as; Eden makes the file the first
 * time it runs. */
static void name_switch_user(const wchar_t *workdir)
{
   /* yuzu's ProfileDataRaw: 0x10 bytes, then 8 users of 0xC8: id (0x10),
    * second id (0x10), timestamp (8), name (0x20, UTF-8, NUL-padded)... */
   unsigned char data[0x650], name[0x20], id[16];
   unsigned char *user = NULL;
   wchar_t path[MAX_PATH * 2];
   const unsigned char *s;
   size_t i, n, chars;
   bool changed = false;
   FILE *f;

   _snwprintf(path, MAX_PATH * 2, L"%ls\\user\\nand\\system\\save\\8000000000000010"
         L"\\su\\avators\\profiles.dat", workdir);
   path[MAX_PATH * 2 - 1] = L'\0';
   if (!(f = _wfopen(path, L"rb")))
      return;
   n = fread(data, 1, sizeof(data), f);
   if (n != sizeof(data) || fgetc(f) != EOF)
      n = 0;
   fclose(f);
   if (!n)
   {
      log_msg("profiles.dat not as expected; Switch user not renamed");
      return;
   }

   for (i = 0; i < 8 && !user; i++)
   {
      unsigned char *u = data + 0x10 + i * 0xC8;
      for (n = 0; n < 0x10 && !u[n]; n++);
      if (n < 0x10)
         user = u;
   }
   if (!user)
      return;

   /* Both ids are the user's. */
   if (profile_user_id(user, id) && memcmp(user, id, 16))
   {
      memcpy(user, id, 16);
      memcpy(user + 0x10, id, 16);
      changed = true;
   }

   /* At most 10 characters, as on a Switch, whole UTF-8 sequences only,
    * and a NUL after. */
   memset(name, 0, sizeof(name));
   for (s = (const unsigned char*)profile_name, n = chars = 0; *s; )
   {
      size_t len = *s < 0x80 ? 1 : *s >= 0xF0 ? 4 : *s >= 0xE0 ? 3 : 2;
      if (*s < 0x20 || chars == 10 || n + len >= sizeof(name))
         break;
      memcpy(name + n, s, len);
      n += len;
      s += len;
      chars++;
   }
   if (n && memcmp(user + 0x28, name, sizeof(name)))
   {
      memcpy(user + 0x28, name, sizeof(name));
      changed = true;
   }
   if (!changed)
      return;

   if ((f = _wfopen(path, L"wb")))
   {
      if (fwrite(data, 1, sizeof(data), f) == sizeof(data))
         log_msg("Switch user \"%s\" written", (const char*)(user + 0x28));
      fclose(f);
   }
}

static DWORD WINAPI launch_main(LPVOID unused)
{
   wchar_t cmdline[MAX_PATH * 6];
   wchar_t alias[MAX_PATH * 2];
   const wchar_t *exe = launch.exe;
   STARTUPINFOW si;
   PROCESS_INFORMATION pi;
   BOOL ok;

   (void)unused;
   network_init_rva = 0;
   if (     mp.active && launch.use_cli
         && !(network_init_rva = find_network_init(launch.cli)))
   {
      log_msg("multiplayer: Network::Init not found in eden-cli.exe; playing alone");
      launch.message = "Multiplayer doesn't work with this Eden version. Playing alone.";
      mp.active      = false;
   }
   if (mp.active && mp.host && !start_room(launch.exe, launch.workdir, launch.rom))
   {
      launch.message = "Multiplayer: the room couldn't be opened. Playing alone.";
      mp.active      = false;
   }

   write_config(launch.exe, launch.use_cli);
   link_saves(launch.exe);
   name_switch_user(launch.workdir);

   if (launch.use_cli && mp.active)
   {
      const wchar_t *game = cli_game_path(launch.rom, alias, MAX_PATH * 2);
      log_msg("multiplayer: joining %s:%u as \"%s\"%s", mp.address, mp.port, mp.nick,
            mp.password[0] ? " with password" : "");
      /* Long option only (-m isn't one of eden-cli's), and no ':' without
       * a password: "nick:@address" doesn't match its format. */
      _snwprintf(cmdline, MAX_PATH * 6,
            L"\"%ls\" %ls--multiplayer \"%hs%hs%hs@%hs:%u\" -g \"%ls\"",
            launch.cli, launch.fullscreen ? L"-f " : L"", mp.nick,
            mp.password[0] ? ":" : "", mp.password, mp.address, mp.port, game);
      exe = launch.cli;
      /* Not the room's password, which is on the command line. */
      log_msg("starting: %ls -g \"%ls\"", exe, game);
   }
   else
   {
      const wchar_t *game = launch.use_cli
         ? cli_game_path(launch.rom, alias, MAX_PATH * 2) : launch.rom;
      if (launch.use_cli)
         exe = launch.cli;
      _snwprintf(cmdline, MAX_PATH * 6, L"\"%ls\" %ls-g \"%ls\"",
            exe, launch.fullscreen ? L"-f " : L"", game);
      log_msg("starting: %ls", cmdline);
   }
   cmdline[MAX_PATH * 6 - 1] = L'\0';

   memset(&si, 0, sizeof(si));
   si.cb = sizeof(si);
   if (launch.overlay)
   {
      /* Its window's first show is a hide: it opens fullscreen and stays
       * black (its thread busy loading) until taken over, see style_emu. */
      si.dwFlags     = STARTF_USESHOWWINDOW;
      si.wShowWindow = SW_HIDE;
   }
   {
      /* Without Steam's SDL settings, see child_environment. */
      wchar_t *env = child_environment();
      ok = spawn_process(exe, cmdline, FALSE,
            (launch.overlay ? CREATE_SUSPENDED : 0)
            | (env ? CREATE_UNICODE_ENVIRONMENT : 0), env, launch.workdir, &si, &pi);
      free(env);
   }
   if (!ok)
   {
      log_msg("CreateProcess failed: %lu", (unsigned long)GetLastError());
      launch.message = "Failed to start Eden.";
      stop_room();
      InterlockedExchange(&launch.state, LAUNCH_FAILED);
      return 0;
   }

   emu_process = pi.hProcess;
   emu_pid     = pi.dwProcessId;
   if (launch.use_cli)
   {
      kbd_start(pi.dwProcessId);
      start_emu_helpers(pi.hProcess);
   }
   if (launch.overlay)
      InterlockedExchangePointer((PVOID volatile*)&ov.emu_thread, pi.hThread);
   else
      CloseHandle(pi.hThread);
   InterlockedExchange(&launch.state, LAUNCH_DONE);
   return 0;
}

/* Waits for the launch thread (bounded, so RetroArch can't hang on it).
 * True if Eden was started. */
static bool launch_join(void)
{
   if (launch.thread)
   {
      if (WaitForSingleObject(launch.thread, 15000) != WAIT_OBJECT_0)
         log_msg("launch thread didn't finish in time");
      CloseHandle(launch.thread);
      launch.thread = NULL;
   }
   return launch.state == LAUNCH_DONE;
}

/* ---------------------------------------------------------------------- */
/* libretro                                                               */
/* ---------------------------------------------------------------------- */

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   static struct retro_core_options_v2 options = { option_cats, option_defs };
   bool no_game = false;

   env_cb = cb;
   cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { (void)cb; }

RETRO_API void retro_init(void)
{
   if (!config_lock_init)
   {
      InitializeCriticalSection(&config_lock);
      config_lock_init = true;
   }
}

RETRO_API void retro_deinit(void)
{
   launch_join();
   overlay_stop();
   kbd_stop();
   release_process();
   stop_room();
   loading_close();
   if (kbd.lock_init)
   {
      DeleteCriticalSection(&kbd.lock);
      kbd.lock_init = false;
   }
   if (kbd.answered)
   {
      CloseHandle(kbd.answered);
      kbd.answered = NULL;
   }
   if (config_lock_init)
   {
      DeleteCriticalSection(&config_lock);
      config_lock_init = false;
   }
   if (ov.lock_init)
   {
      DeleteCriticalSection(&ov.lock);
      ov.lock_init = false;
   }
   if (log_file)
   {
      fclose(log_file);
      log_file = NULL;
   }
}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Eden";
   info->library_version  = "1.0";
   info->valid_extensions = "nsp|xci|nca|nro|nso";
   info->need_fullpath    = true;
   info->block_extract    = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->geometry.base_width   = fb_width;
   info->geometry.base_height  = fb_height;
   info->geometry.max_width    = fb_width;
   info->geometry.max_height   = fb_height;
   info->geometry.aspect_ratio = (float)fb_width / (float)fb_height;
   info->timing.fps            = 60.0;
   info->timing.sample_rate    = 48000.0;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

RETRO_API void retro_reset(void) { }

RETRO_API void retro_run(void)
{
   input_poll_cb();
   loading_frame();

   if (ov.active)
      overlay_run();
   kbd_run();

   /* The launch thread finished: what it had to say, and the end if Eden
    * didn't start. */
   if (launch.thread && launch.state != LAUNCH_RUNNING)
   {
      bool started = launch_join();
      if (launch.message)
      {
         show_message(launch.message);
         launch.message = NULL;
      }
      if (!started)
      {
         overlay_stop();
         env_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
         return;
      }
   }

   /* An option changed (Quick Menu, grid): into Eden's config, which it
    * reads at its next start. (Once launched: the launch thread writes
    * the config.) */
   if (!launch.thread)
   {
      bool updated = false;
      if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      {
         char before[sizeof(options_written)];
         strcpy(before, options_written);
         read_options();
         config_edit(apply_options, NULL);
         if (strcmp(before, options_written))
         {
            log_msg("options changed: %s", options_written);
            show_message("Eden setting saved. It applies the next time the game starts.");
         }
      }
   }

   if (     !launch.thread && emu_process
         && WaitForSingleObject(emu_process, 0) == WAIT_OBJECT_0)
   {
      log_msg("Eden exited");
      overlay_stop();
      release_process();
      stop_room();
      env_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
   }
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   wchar_t exe[MAX_PATH * 2];
   wchar_t rom[MAX_PATH * 2];
   wchar_t *slash;
   char env[8];
   bool overlay, fullscreen;

   if (!game || !game->path)
      return false;

   if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      return false;

   log_open();
   log_msg("loading %s", game->path);

   if (!find_eden(exe, MAX_PATH * 2))
   {
      log_msg("eden.exe not found");
      show_message("Eden not found. Put its path in system\\eden.txt "
                   "or install it to system\\Eden.");
      return false;
   }

   if (!to_wide(game->path, rom, MAX_PATH * 2))
      return false;
   loading_open(rom);

   /* The fork's GL driver says so; its window property can't be checked
    * yet, the driver restarts after this. Under RetroArch Eden runs in
    * borderless fullscreen, which also hides its menu and status bar. */
   overlay    = GetEnvironmentVariableA("BOMBA_OVERLAY", env, sizeof(env)) > 0;
   fullscreen = overlay || fullscreen_enabled();
   log_msg(overlay ? "overlay mode" : "own window mode (no overlay support)");

   /* Run from Eden's own folder. */
   wcscpy(launch.workdir, exe);
   if ((slash = wcsrchr(launch.workdir, L'\\')))
      *slash = L'\0';

   {
      DWORD n = GetEnvironmentVariableA("BOMBA_PROFILE", profile_name, sizeof(profile_name));
      if (!n || n >= sizeof(profile_name))
         profile_name[0] = '\0';
   }

   read_multiplayer_settings();
   /* The game plays in eden-cli.exe, which comes with Eden: it joins rooms
    * from its command line (see "Multiplayer"), and has the Switch
    * keyboard through RetroArch (see "Switch keyboard"). */
   _snwprintf(launch.cli, MAX_PATH * 2, L"%ls\\eden-cli.exe", launch.workdir);
   launch.cli[MAX_PATH * 2 - 1] = L'\0';
   launch.use_cli = file_exists(launch.cli);
   if (!launch.use_cli)
   {
      log_msg("eden-cli.exe not found: eden.exe plays, alone");
      if (mp.active)
         show_message("Multiplayer needs eden-cli.exe, which comes with Eden. Playing alone.");
      mp.active = false;
   }

   /* What the launch thread needs from RetroArch. */
   {
      const char *save_dir = NULL;
      save_dir_cache[0] = '\0';
      if (env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir)
         snprintf(save_dir_cache, sizeof(save_dir_cache), "%s", save_dir);
   }
   read_options();

   /* The Switch's controls under their own names, on the RetroPad
    * controls in the same places, so RetroArch and the grid's Bindings page
    * show them (the grid lists the controls an emulator names). */
   {
      static const struct retro_input_descriptor desc[] = {
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "X" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Y" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "ZL" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "ZR" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Left Stick Press" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "Right Stick Press" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Minus (-)" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Plus (+)" },
         { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X, "Left Stick X" },
         { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y" },
         { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X" },
         { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y" },
         { 0, 0, 0, 0, NULL },
      };
      env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)desc);
   }

   if (overlay)
      overlay_start();

   wcscpy(launch.exe, exe);
   wcscpy(launch.rom, rom);
   launch.overlay    = overlay && ov.thread;
   launch.fullscreen = fullscreen;
   launch.message    = NULL;
   launch.state      = LAUNCH_RUNNING;
   if (!(launch.thread = CreateThread(NULL, 0, launch_main, NULL, 0, NULL)))
   {
      log_msg("launch thread failed (%lu)", (unsigned long)GetLastError());
      show_message("Failed to start Eden.");
      overlay_stop();
      return false;
   }
   return true;
}

RETRO_API bool retro_load_game_special(unsigned type,
      const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

RETRO_API void retro_unload_game(void)
{
   launch_join();
   /* Restore RetroArch's window and let Eden run again first. */
   overlay_stop();

   /* Content closed from RetroArch while Eden still runs: ask it to close
    * normally rather than killing it, so it can flush saves. */
   if (emu_process && WaitForSingleObject(emu_process, 0) == WAIT_TIMEOUT)
      EnumWindows(close_window, (LPARAM)emu_pid);
   kbd_stop();
   release_process();
   stop_room();
   loading_close();
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }

RETRO_API void retro_cheat_reset(void) { }
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

RETRO_API void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
