/* Helper for eden_libretro: works out Eden's input settings for Player 1.
 *
 *   eden_padinfo <SDL2.dll> <enable_raw_input> <enable_joycon_driver>
 *                <enable_procon_driver>     (the three as 0 / 1)
 *                [<key>=b:<sdl button> | <key>=a:<sdl axis>:<+|->] ...
 *
 * The optional overrides bind a Switch control (an Eden key such as
 * player_0_button_a) to another SDL GameController button or axis
 * direction, for bindings the player changed in RetroArch.
 *
 * Asks SDL2 what Eden's own SDL2 asks when it maps a controller: with the
 * hints Eden sets (SDLDriver::SDLDriver), the first game controller's GUID
 * and which raw button / axis / hat each standard control is. Prints
 * qt-config.ini [Controls] lines, key=value, or nothing if no controller.
 * Lines starting with '#' are for the core's log.
 *
 * A separate process on purpose: like Eden, it opens SDL fresh, with only
 * the environment it inherits; inside RetroArch's process SDL may not see
 * controllers the way Eden does.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint8_t data[16]; } sdl_guid_t;
typedef struct
{
   int bind_type; /* 0 none, 1 button, 2 axis, 3 hat */
   union { int button; int axis; struct { int hat, hat_mask; } hat; } value;
} sdl_bind_t;

#define SDL_INIT_GAMECONTROLLER 0x00002000u

enum
{
   SDL_A = 0, SDL_B, SDL_X, SDL_Y, SDL_BACK, SDL_GUIDE, SDL_START,
   SDL_LSTICK, SDL_RSTICK, SDL_LSHOULDER, SDL_RSHOULDER,
   SDL_DUP, SDL_DDOWN, SDL_DLEFT, SDL_DRIGHT, SDL_MISC1
};
enum { SDL_LEFTX = 0, SDL_LEFTY, SDL_RIGHTX, SDL_RIGHTY, SDL_TRIGGERL, SDL_TRIGGERR };

static struct
{
   int (*InitSubSystem)(uint32_t);
   int (*SetHint)(const char*, const char*);
   int (*NumJoysticks)(void);
   void (*JoystickUpdate)(void);
   const char *(*JoystickNameForIndex)(int);
   int (*IsGameController)(int);
   void *(*GameControllerOpen)(int);
   const char *(*GameControllerName)(void*);
   void *(*GameControllerGetJoystick)(void*);
   sdl_guid_t (*JoystickGetGUID)(void*);
   sdl_bind_t (*GetBindForButton)(void*, int);
   sdl_bind_t (*GetBindForAxis)(void*, int);
} sdl;

static int sdl_load(const char *path)
{
   HMODULE m = LoadLibraryA(path);
   FARPROC p;
   if (!m)
      return 0;
#define SDL_SYM(field, name) \
   if (!(p = GetProcAddress(m, name))) return 0; \
   memcpy(&sdl.field, &p, sizeof(p))
   SDL_SYM(InitSubSystem,             "SDL_InitSubSystem");
   SDL_SYM(SetHint,                   "SDL_SetHint");
   SDL_SYM(NumJoysticks,              "SDL_NumJoysticks");
   SDL_SYM(JoystickUpdate,            "SDL_JoystickUpdate");
   SDL_SYM(JoystickNameForIndex,      "SDL_JoystickNameForIndex");
   SDL_SYM(IsGameController,          "SDL_IsGameController");
   SDL_SYM(GameControllerOpen,        "SDL_GameControllerOpen");
   SDL_SYM(GameControllerName,        "SDL_GameControllerName");
   SDL_SYM(GameControllerGetJoystick, "SDL_GameControllerGetJoystick");
   SDL_SYM(JoystickGetGUID,           "SDL_JoystickGetGUID");
   SDL_SYM(GetBindForButton,          "SDL_GameControllerGetBindForButton");
   SDL_SYM(GetBindForAxis,            "SDL_GameControllerGetBindForAxis");
#undef SDL_SYM
   return 1;
}

/* One Eden input parameter, as its SDL driver builds it
 * (BuildParamPackageForBinding), quoted for the INI. An axis is pressed
 * past half way in its direction (dir '+' or '-'). */
static void print_bind_dir(const char *key, const char *head, sdl_bind_t b, char dir)
{
   static const char *hat_dir[] = { "", "up", "right", "", "down", "", "", "", "left" };

   switch (b.bind_type)
   {
      case 1:
         printf("%s=\"%s,button:%d\"\n", key, head, b.value.button);
         break;
      case 2:
         printf("%s=\"%s,axis:%d,threshold:0.5,invert:%c\"\n", key, head,
               b.value.axis, dir == '-' ? '-' : '+');
         break;
      case 3:
         printf("%s=\"%s,hat:%d,direction:%s\"\n", key, head, b.value.hat.hat,
               b.value.hat.hat_mask >= 0 && b.value.hat.hat_mask <= 8
               ? hat_dir[b.value.hat.hat_mask] : "");
         break;
      default:
         printf("%s=\"[empty]\"\n", key);
         break;
   }
}

/* The player's override for a key, if given: SDL's bind for it. */
static int override_bind(int argc, char **argv, void *pad, const char *key,
      sdl_bind_t *out, char *dir)
{
   size_t key_len = strlen(key);
   int i, n;

   for (i = 5; i < argc; i++)
   {
      const char *v = argv[i];
      if (strncmp(v, key, key_len) || v[key_len] != '=')
         continue;
      v += key_len + 1;
      if (v[0] == 'b' && v[1] == ':' && sscanf(v + 2, "%d", &n) == 1)
      {
         *out = sdl.GetBindForButton(pad, n);
         *dir = '+';
         return 1;
      }
      if (v[0] == 'a' && v[1] == ':' && sscanf(v + 2, "%d", &n) == 1)
      {
         const char *sign = strrchr(v, ':');
         *out = sdl.GetBindForAxis(pad, n);
         *dir = sign && sign[1] == '-' ? '-' : '+';
         return 1;
      }
   }
   return 0;
}

static int g_argc;
static char **g_argv;
static void *g_pad;

static void print_bind(const char *key, const char *head, sdl_bind_t b)
{
   sdl_bind_t o;
   char dir = '+';
   if (override_bind(g_argc, g_argv, g_pad, key, &o, &dir))
   {
      printf("# %s: player's binding\n", key);
      b = o;
   }
   print_bind_dir(key, head, b, dir);
}

int main(int argc, char **argv)
{
   /* Laid out by position like RetroArch's RetroPad and Eden's own default
    * mapping: the Switch's A is the right face button, B the bottom one,
    * X the top, Y the left. */
   static const struct { const char *key; int sdl_button; } buttons[] = {
      { "player_0_button_a",          SDL_B },
      { "player_0_button_b",          SDL_A },
      { "player_0_button_x",          SDL_Y },
      { "player_0_button_y",          SDL_X },
      { "player_0_button_lstick",     SDL_LSTICK },
      { "player_0_button_rstick",     SDL_RSTICK },
      { "player_0_button_l",          SDL_LSHOULDER },
      { "player_0_button_r",          SDL_RSHOULDER },
      { "player_0_button_plus",       SDL_START },
      { "player_0_button_minus",      SDL_BACK },
      { "player_0_button_dleft",      SDL_DLEFT },
      { "player_0_button_dup",        SDL_DUP },
      { "player_0_button_dright",     SDL_DRIGHT },
      { "player_0_button_ddown",      SDL_DDOWN },
      { "player_0_button_home",       SDL_GUIDE },
      { "player_0_button_screenshot", SDL_MISC1 },
   };
   void *pad = NULL;
   char guid[33], head[96];
   sdl_guid_t g;
   int i, n = 0;

   if (argc < 5 || !sdl_load(argv[1]))
   {
      printf("# can't load %s\n", argc > 1 ? argv[1] : "SDL2.dll");
      return 1;
   }

   /* Diagnostics, for the core's log: what this process can see. */
   {
      LPCH env = GetEnvironmentStringsA(), e;
      HMODULE xi = LoadLibraryA("xinput1_4.dll");
      DWORD (WINAPI *get_state)(DWORD, void*) =
         xi ? (DWORD (WINAPI*)(DWORD, void*))(void*)GetProcAddress(xi, "XInputGetState") : NULL;
      unsigned char state[16];
      DWORD s;

      for (e = env; e && *e; e += strlen(e) + 1)
         if (!_strnicmp(e, "SDL", 3) || !_strnicmp(e, "Steam", 5))
            printf("# env %.200s\n", e);
      if (env)
         FreeEnvironmentStringsA(env);
      printf("# Steam overlay DLL loaded: %s\n",
            GetModuleHandleA("GameOverlayRenderer64.dll") ? "yes" : "no");
      for (s = 0; get_state && s < 4; s++)
         printf("# XInput slot %lu: %s\n", (unsigned long)s,
               get_state(s, state) == 0 ? "connected" : "empty");
   }

   /* Eden's hints. */
   sdl.SetHint("SDL_JOYSTICK_RAWINPUT", argv[2][0] == '1' ? "1" : "0");
   sdl.SetHint("SDL_ACCELEROMETER_AS_JOYSTICK", "0");
   if (argv[3][0] == '1')
      sdl.SetHint("SDL_JOYSTICK_HIDAPI_JOY_CONS", "0");
   else
   {
      sdl.SetHint("SDL_JOYSTICK_HIDAPI_JOY_CONS", "1");
      sdl.SetHint("SDL_JOYSTICK_HIDAPI_COMBINE_JOY_CONS", "0");
      sdl.SetHint("SDL_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS", "1");
   }
   sdl.SetHint("SDL_JOYSTICK_HIDAPI_SWITCH", argv[4][0] == '1' ? "0" : "1");
   sdl.SetHint("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0");
   sdl.SetHint("SDL_JOYSTICK_HIDAPI_XBOX", "0");

   if (sdl.InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
   {
      printf("# SDL game controller init failed\n");
      return 1;
   }

   /* Some backends add controllers a moment after init. */
   for (i = 0; i < 20 && !(n = sdl.NumJoysticks()); i++)
   {
      sdl.JoystickUpdate();
      Sleep(50);
   }
   printf("# SDL sees %d joystick(s)\n", n);
   for (i = 0; i < n; i++)
      printf("#  %d: %s%s\n", i, sdl.JoystickNameForIndex(i),
            sdl.IsGameController(i) ? "" : " (not a game controller)");

   for (i = 0; i < n && !pad; i++)
      if (sdl.IsGameController(i))
         pad = sdl.GameControllerOpen(i);
   if (!pad)
      return 0;
   g_argc = argc;
   g_argv = argv;
   g_pad  = pad;

   /* Eden's GUID: SDL's, with the name CRC (bytes 2-3) cleared, as hex. */
   g = sdl.JoystickGetGUID(sdl.GameControllerGetJoystick(pad));
   g.data[2] = g.data[3] = 0;
   for (i = 0; i < 16; i++)
      snprintf(guid + i * 2, 3, "%02x", g.data[i]);
   snprintf(head, sizeof(head), "engine:sdl,port:0,guid:%s", guid);
   printf("# Player 1 = %s (%s)\n", sdl.GameControllerName(pad), guid);

   for (i = 0; i < (int)(sizeof(buttons) / sizeof(buttons[0])); i++)
      print_bind(buttons[i].key, head, sdl.GetBindForButton(pad, buttons[i].sdl_button));

   /* ZL / ZR: the triggers, axes in SDL. */
   print_bind("player_0_button_zl", head, sdl.GetBindForAxis(pad, SDL_TRIGGERL));
   print_bind("player_0_button_zr", head, sdl.GetBindForAxis(pad, SDL_TRIGGERR));

   /* Sticks (BuildParamPackageForAnalog). */
   {
      sdl_bind_t lx = sdl.GetBindForAxis(pad, SDL_LEFTX),  ly = sdl.GetBindForAxis(pad, SDL_LEFTY);
      sdl_bind_t rx = sdl.GetBindForAxis(pad, SDL_RIGHTX), ry = sdl.GetBindForAxis(pad, SDL_RIGHTY);
      if (lx.bind_type == 2 && ly.bind_type == 2)
         printf("player_0_lstick=\"%s,axis_x:%d,axis_y:%d,offset_x:0,offset_y:0,"
               "invert_x:+,invert_y:+\"\n", head, lx.value.axis, ly.value.axis);
      if (rx.bind_type == 2 && ry.bind_type == 2)
         printf("player_0_rstick=\"%s,axis_x:%d,axis_y:%d,offset_x:0,offset_y:0,"
               "invert_x:+,invert_y:+\"\n", head, rx.value.axis, ry.value.axis);
   }
   printf("player_0_connected=true\n");
   return 0;
}
