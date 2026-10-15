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
 * Multiplayer (see "Multiplayer" below) runs the game in eden-cli.exe, which
 * comes with Eden, instead of eden.exe.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <winioctl.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>

#define FB_WIDTH  320
#define FB_HEIGHT 240

/* How long Eden's window must exist before it's taken over (skips the
 * moment it opens blank). */
#define OVERLAY_DELAY_MS 1500
/* No retro_run for this long: RetroArch paused the core; pause Eden. */
#define PAUSE_AFTER_MS   250
/* How often the window arrangement is checked. */
#define ARRANGE_MS       250

typedef LONG (NTAPI *nt_process_fn)(HANDLE);

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;

static uint32_t framebuffer[FB_WIDTH * FB_HEIGHT];
static HANDLE emu_process;
static DWORD emu_pid;

static struct
{
   bool active;        /* overlay mode */
   bool transparent;   /* RetroArch's window is see-through now */
   bool hidden;        /* Eden hidden while RetroArch is minimized */
   HWND ra;            /* RetroArch's window */
   HWND emu;           /* Eden's game window, once it appears */
   DWORD emu_seen;     /* tick it appeared */
   DWORD last_arrange; /* tick the arrangement was last checked */
   RECT placed;        /* where Eden's window was last put */

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
   wcsncat(path, L"\\config\\remaps\\Bomba.cfg", MAX_PATH * 2 - wcslen(path) - 1);
   if (!(f = _wfopen(path, L"rb")))
      return;
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
      BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE,
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

/* The options' values as last written, to tell a real change. */
static char options_written[512];

static void apply_options(char **buf, size_t *len, void *data)
{
   char now[512] = {0};
   size_t i;

   (void)data;
   for (i = 0; i < sizeof(option_keys) / sizeof(option_keys[0]); i++)
   {
      struct retro_variable var = { option_keys[i].option, NULL };
      if (!env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value)
         continue;
      ini_set(buf, len, option_keys[i].section, option_keys[i].key, var.value);
      strncat(now, var.value, sizeof(now) - strlen(now) - 2);
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
   size_t tl = wcslen(target);
   DWORD attr, bytes;
   HANDLE h;
   BOOL ok;

   if (tl > 2 * MAX_PATH)
      return false;

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
   const char *save_dir = NULL;
   wchar_t dir[MAX_PATH * 2], link[MAX_PATH * 2], target[MAX_PATH * 2];
   wchar_t *slash, *p;

   if (!env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) || !save_dir
         || !to_wide(save_dir, dir, MAX_PATH * 2))
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
 * Joining needs eden-cli.exe (eden.exe can't join from its command line).
 * Hosting also starts eden.exe's dedicated room server, hidden, for as long
 * as the game runs, and joins it. */
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
} mp;

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
   const char *save_dir = NULL;
   char nick[32];
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

   player_name(nick, sizeof(nick));
   _snwprintf(name, 32, L"%hs", nick);
   name[31] = L'\0';

   password[0] = L'\0';
   if (mp.password[0])
      _snwprintf(password, 96, L" -w \"%hs\"", mp.password);
   password[95] = L'\0';

   log_path[0] = L'\0';
   if (     env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir
         && to_wide(save_dir, log_path, MAX_PATH * 2 - 32))
      wcscat(log_path, L"\\eden-room.log");
   _snwprintf(ban_path, MAX_PATH * 2, L"%ls\\user\\room-ban-list.txt", workdir);
   ban_path[MAX_PATH * 2 - 1] = L'\0';

   _snwprintf(cmd, MAX_PATH * 8,
         L"\"%ls\" --room -n \"%ls\" -p %u -m %u -g \"%ls\" -b \"%ls\"%ls%ls%ls%ls",
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
      BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE,
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

/* Eden's largest visible top-level window: its game window. */
static BOOL CALLBACK find_emu_window(HWND hwnd, LPARAM data)
{
   struct emu_window_search *s = (struct emu_window_search*)data;
   DWORD pid = 0;
   RECT r;
   LONGLONG area;

   GetWindowThreadProcessId(hwnd, &pid);
   if (     pid != emu_pid || !IsWindowVisible(hwnd)
         || GetWindow(hwnd, GW_OWNER) || !GetWindowRect(hwnd, &r))
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

static bool try_foreground(void)
{
   HWND fg       = GetForegroundWindow();
   DWORD fg_tid  = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
   DWORD our_tid = GetCurrentThreadId();

   if (fg_tid && fg_tid != our_tid)
      AttachThreadInput(our_tid, fg_tid, TRUE);
   SetForegroundWindow(ov.ra);
   if (fg_tid && fg_tid != our_tid)
      AttachThreadInput(our_tid, fg_tid, FALSE);
   return GetForegroundWindow() == ov.ra;
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

   if (on)
      SetPropA(ov.ra, "BombaTransparent", (HANDLE)1);
   else
      RemovePropA(ov.ra, "BombaTransparent");
   ov.transparent = on;
}

/* Eden's window as part of RetroArch's: no border or caption, no taskbar
 * / Alt-Tab entry (a tool window), never activated. Hidden and shown
 * again so the taskbar notices. */
static void style_emu(void)
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
   ShowWindow(ov.emu, SW_SHOWNOACTIVATE);
   memset(&ov.placed, 0, sizeof(ov.placed));
   log_msg("Eden window %p styled (style %08lx -> %08lx)", (void*)ov.emu,
         (unsigned long)style, (unsigned long)GetWindowLongPtrW(ov.emu, GWL_STYLE));
}

/* Puts Eden's window exactly under RetroArch's client area, directly
 * below it in the window order. Invisible borders the window may still
 * have are measured (DWM's visible bounds) and placed outside. */
static void place_emu(void)
{
   const UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
   RECT target, win, vis;
   POINT tl = { 0, 0 };

   if (!GetClientRect(ov.ra, &target) || !ClientToScreen(ov.ra, &tl))
      return;
   OffsetRect(&target, tl.x, tl.y);
   if (target.right <= target.left || target.bottom <= target.top)
      return;

   if (!EqualRect(&target, &ov.placed))
   {
      SetWindowPos(ov.emu, ov.ra, target.left, target.top,
            target.right - target.left, target.bottom - target.top, flags);

      if (     GetWindowRect(ov.emu, &win)
            && SUCCEEDED(DwmGetWindowAttribute(ov.emu,
                  DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))
            && !EqualRect(&vis, &target))
      {
         LONG l = vis.left - win.left, t = vis.top - win.top;
         LONG r = win.right - vis.right, b = win.bottom - vis.bottom;
         SetWindowPos(ov.emu, ov.ra, target.left - l, target.top - t,
               target.right - target.left + l + r,
               target.bottom - target.top + t + b, flags);
         log_msg("Eden window borders %ld,%ld,%ld,%ld compensated", l, t, r, b);
      }
      if (GetWindowRect(ov.emu, &win))
         log_msg("Eden window placed at %ld,%ld %ldx%ld (RetroArch %ld,%ld %ldx%ld)",
               win.left, win.top, win.right - win.left, win.bottom - win.top,
               target.left, target.top,
               target.right - target.left, target.bottom - target.top);
      ov.placed = target;
   }
   else
      SetWindowPos(ov.emu, ov.ra, 0, 0, 0, 0,
            flags | SWP_NOMOVE | SWP_NOSIZE);
}

/* Takes Eden's window over once it has been up a moment, then keeps the
 * two windows together. Only while Eden runs: calls on its window wait on
 * its thread. */
static void arrange(void)
{
   DWORD now = GetTickCount();
   HWND hwnd, fg;
   DWORD fg_pid = 0;

   if (!ov.transparent)
   {
      if (!(hwnd = current_emu_window()))
         return;
      if (hwnd != ov.emu)
      {
         RECT r;
         GetWindowRect(hwnd, &r);
         log_msg("Eden window %p (%ld,%ld %ldx%ld)", (void*)hwnd,
               r.left, r.top, r.right - r.left, r.bottom - r.top);
         ov.emu      = hwnd;
         ov.emu_seen = now;
         return;
      }
      if (now - ov.emu_seen < OVERLAY_DELAY_MS)
         return;

      style_emu();
      place_emu();
      set_ra_transparent(true);
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
         style_emu();
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
      place_emu();
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
         started        = GetTickCount();
      }
      if (!ov.ra)
      {
         EnumWindows(find_ra_window, (LPARAM)&ov.ra);
         if (ov.ra)
            log_msg("RetroArch window %p", (void*)ov.ra);
         else if (GetTickCount() - started > 10000)
         {
            log_msg("RetroArch's window never advertised overlay support");
            break;
         }
         continue;
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
      else if (stalled && ov.suspend)
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
   if (com)
      CoUninitialize();
   return 0;
}

static void overlay_start(void)
{
   HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");

   memset(&ov.placed, 0, sizeof(ov.placed));
   ov.active       = true;
   ov.transparent  = false;
   ov.hidden       = false;
   ov.ra           = NULL;
   ov.emu          = NULL;
   ov.emu_seen     = 0;
   ov.last_arrange = 0;

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
   if (ov.suspended && emu_process)
      ov.resume(emu_process);
   ov.suspended = false;

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
   overlay_stop();
   release_process();
   stop_room();
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
   info->geometry.base_width   = FB_WIDTH;
   info->geometry.base_height  = FB_HEIGHT;
   info->geometry.max_width    = FB_WIDTH;
   info->geometry.max_height   = FB_HEIGHT;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
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
   /* Black until RetroArch turns see-through; ignored after that. */
   video_cb(framebuffer, FB_WIDTH, FB_HEIGHT, FB_WIDTH * sizeof(uint32_t));

   if (ov.active)
      overlay_run();

   /* An option changed (Quick Menu, grid): into Eden's config, which it
    * reads at its next start. */
   {
      bool updated = false;
      if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      {
         char before[sizeof(options_written)];
         strcpy(before, options_written);
         config_edit(apply_options, NULL);
         if (strcmp(before, options_written))
         {
            log_msg("options changed: %s", options_written);
            show_message("Eden setting saved. It applies the next time the game starts.");
         }
      }
   }

   if (emu_process && WaitForSingleObject(emu_process, 0) == WAIT_OBJECT_0)
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
   wchar_t cli[MAX_PATH * 2];
   wchar_t rom[MAX_PATH * 2];
   wchar_t cmdline[MAX_PATH * 6];
   wchar_t workdir[MAX_PATH * 2];
   wchar_t *slash;
   char env[8];
   STARTUPINFOW si;
   PROCESS_INFORMATION pi;
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

   /* The fork's GL driver says so; its window property can't be checked
    * yet, the driver restarts after this. Under RetroArch Eden runs in
    * borderless fullscreen, which also hides its menu and status bar. */
   overlay    = GetEnvironmentVariableA("BOMBA_OVERLAY", env, sizeof(env)) > 0;
   fullscreen = overlay || fullscreen_enabled();
   log_msg(overlay ? "overlay mode" : "own window mode (no overlay support)");

   /* Run from Eden's own folder. */
   wcscpy(workdir, exe);
   if ((slash = wcsrchr(workdir, L'\\')))
      *slash = L'\0';

   read_multiplayer_settings();
   if (mp.active)
   {
      _snwprintf(cli, MAX_PATH * 2, L"%ls\\eden-cli.exe", workdir);
      cli[MAX_PATH * 2 - 1] = L'\0';
      if (!file_exists(cli))
      {
         log_msg("multiplayer: eden-cli.exe not found");
         show_message("Multiplayer needs eden-cli.exe, which comes with Eden. Playing alone.");
         mp.active = false;
      }
      else if (mp.host && !start_room(exe, workdir, rom))
      {
         show_message("Multiplayer: the room couldn't be opened. Playing alone.");
         mp.active = false;
      }
   }

   write_config(exe, mp.active);
   link_saves(exe);

   if (mp.active)
   {
      char nick[32];
      player_name(nick, sizeof(nick));
      log_msg("multiplayer: joining %s:%u as \"%s\"%s", mp.address, mp.port, nick,
            mp.password[0] ? " with password" : "");
      _snwprintf(cmdline, MAX_PATH * 6, L"\"%ls\" %ls-m \"%hs:%hs@%hs:%u\" -g \"%ls\"",
            cli, fullscreen ? L"-f " : L"", nick, mp.password, mp.address, mp.port, rom);
      wcscpy(exe, cli);
   }
   else
      _snwprintf(cmdline, MAX_PATH * 6, L"\"%ls\" %ls-g \"%ls\"",
            exe, fullscreen ? L"-f " : L"", rom);
   cmdline[MAX_PATH * 6 - 1] = L'\0';

   memset(&si, 0, sizeof(si));
   si.cb = sizeof(si);
   /* Not the room's password, which is on the command line. */
   if (mp.active)
      log_msg("starting: %ls -g \"%ls\"", exe, rom);
   else
      log_msg("starting: %ls", cmdline);
   {
      /* Without Steam's SDL settings, see child_environment. */
      wchar_t *env = child_environment();
      BOOL ok = CreateProcessW(exe, cmdline, NULL, NULL, FALSE,
            env ? CREATE_UNICODE_ENVIRONMENT : 0, env, workdir, &si, &pi);
      free(env);
      if (!ok)
      {
         log_msg("CreateProcess failed: %lu", (unsigned long)GetLastError());
         show_message("Failed to start Eden.");
         stop_room();
         return false;
      }
   }

   CloseHandle(pi.hThread);
   emu_process = pi.hProcess;
   emu_pid     = pi.dwProcessId;

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
   /* Restore RetroArch's window and let Eden run again first. */
   overlay_stop();

   /* Content closed from RetroArch while Eden still runs: ask it to close
    * normally rather than killing it, so it can flush saves. */
   if (emu_process && WaitForSingleObject(emu_process, 0) == WAIT_TIMEOUT)
      EnumWindows(close_window, (LPARAM)emu_pid);
   release_process();
   stop_room();
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
