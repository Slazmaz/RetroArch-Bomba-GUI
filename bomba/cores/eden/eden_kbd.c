/* Bomba: the Switch keyboard for eden-cli.exe. Loaded into eden-cli by the
 * Eden core (eden_libretro.c, "Switch keyboard"). eden-cli has no keyboard
 * of its own: its stand-in (yuzu's DefaultSoftwareKeyboardApplet,
 * frontend/applets/software_keyboard.cpp) answers every normal keyboard
 * with "Eden". Its ShowNormalKeyboard is patched here to send what the
 * game asks to the core instead (a pipe named after this process), which
 * has RetroArch ask the player; the answer goes back to the game through
 * the stand-in's own SubmitNormalText. Eden's files stay as they are.
 *
 * Found by what Eden logs (ShowNormalKeyboard: "...show the normal software
 * keyboard."); the applet's members as its InitializeKeyboard stores them
 * (KeyboardInitializeParameters from +0x08, the normal keyboard's callback,
 * a std::function, at +0xd0). Eden's strings are libstdc++ std::u16string.
 * If anything doesn't match, nothing is patched.
 *
 * Also: TCP sockets outside Eden's room (see install_sockets), players 2-8
 * joining while the game runs (see install_players), vibration (see
 * install_vibration) and settings changed while the game runs (see
 * settings_main). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define OFF_HEADER    0x28 /* header_text */
#define OFF_SUB       0x48 /* sub_text */
#define OFF_GUIDE     0x68 /* guide_text */
#define OFF_INITIAL   0x88 /* initial_text */
#define OFF_MAX_LEN   0xac /* max_text_length */
#define OFF_NORMAL_CB 0xd0 /* submit_normal_callback */

#define TEXT_MAX 500       /* UTF-16 units, each way */

/* libstdc++'s std::u16string. */
struct u16str
{
   uint16_t *p;
   size_t len;
   union { size_t cap; uint16_t buf[8]; } u;
};

/* The stand-in's SubmitNormalText(std::u16string) const: copies the text
 * and calls the callback with SwkbdResult::Ok. */
typedef void (*submit_fn)(void *self, struct u16str *text);
/* std::function's invoker: (callback, result, text, confirmed). */
typedef void (*invoke_fn)(void *fn, uint32_t *result, struct u16str *text,
      uint8_t *confirmed);

static submit_fn submit;

/* A request: the applet, and what it asks. */
struct request
{
   uint8_t *self;
   uint32_t max_len;
   uint32_t label_len, initial_len;
   uint16_t label[TEXT_MAX];
   uint16_t initial[TEXT_MAX];
};

/* The applet's string at off, if it looks like one. */
static uint32_t read_str(const uint8_t *self, size_t off, uint16_t *out)
{
   const struct u16str *s = (const struct u16str*)(self + off);
   size_t n;

   if (!s->p || s->len > 4096 || IsBadReadPtr(s->p, s->len * 2))
      return 0;
   n = s->len < TEXT_MAX ? s->len : TEXT_MAX;
   memcpy(out, s->p, n * 2);
   return (uint32_t)n;
}

static void answer(uint8_t *self, bool ok, uint16_t *text, uint32_t len)
{
   struct u16str s;

   if (ok)
   {
      s.p   = text;
      s.len = len;
      submit(self, &s);
   }
   else
   {
      /* Cancelled: the callback itself, as SubmitNormalText calls it. */
      void *manager    = *(void**)(self + OFF_NORMAL_CB + 0x10);
      invoke_fn invoke = *(invoke_fn*)(self + OFF_NORMAL_CB + 0x18);
      uint32_t result  = 1; /* SwkbdResult::Cancel */
      uint8_t confirmed = 1;

      s.p        = s.u.buf;
      s.len      = 0;
      s.u.buf[0] = 0;
      if (manager && invoke)
         invoke(self + OFF_NORMAL_CB, &result, &s, &confirmed);
   }
}

static bool io(HANDLE h, void *buf, DWORD len, bool write)
{
   DWORD done = 0;
   while (len)
   {
      if (!(write ? WriteFile(h, buf, len, &done, NULL)
                  : ReadFile(h, buf, len, &done, NULL)) || !done)
         return false;
      buf  = (uint8_t*)buf + done;
      len -= done;
   }
   return true;
}

/* Asks the core (it waits for the player), and answers the game. Without
 * the core, the stand-in's answer: the text the game started from, else
 * "Eden". */
static DWORD WINAPI ask_main(LPVOID param)
{
   static uint16_t eden[] = { 'E', 'd', 'e', 'n' };
   struct request *req = (struct request*)param;
   uint16_t text[TEXT_MAX];
   uint32_t head[4], reply[2];
   wchar_t name[64];
   HANDLE h = INVALID_HANDLE_VALUE;
   bool asked = false;
   int tries;

   _snwprintf(name, 64, L"\\\\.\\pipe\\bomba-eden-kbd-%lu",
         (unsigned long)GetCurrentProcessId());
   name[63] = L'\0';
   for (tries = 0; tries < 50 && h == INVALID_HANDLE_VALUE; tries++)
   {
      h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, 0, NULL);
      if (h == INVALID_HANDLE_VALUE)
      {
         if (GetLastError() != ERROR_PIPE_BUSY)
            break;
         WaitNamedPipeW(name, 200);
      }
   }

   if (h != INVALID_HANDLE_VALUE)
   {
      head[0] = 0x31424B42; /* "BKB1" */
      head[1] = req->max_len;
      head[2] = req->label_len;
      head[3] = req->initial_len;
      if (     io(h, head, sizeof(head), true)
            && io(h, req->label, req->label_len * 2, true)
            && io(h, req->initial, req->initial_len * 2, true)
            && io(h, reply, sizeof(reply), false)
            && reply[1] <= TEXT_MAX
            && io(h, text, reply[1] * 2, false))
      {
         uint32_t len = reply[1];
         if (req->max_len && len > req->max_len)
            len = req->max_len;
         answer(req->self, reply[0] != 0, text, len);
         asked = true;
      }
      CloseHandle(h);
   }

   if (!asked)
   {
      if (req->initial_len)
         answer(req->self, true, req->initial, req->initial_len);
      else
         answer(req->self, true, eden, 4);
   }
   HeapFree(GetProcessHeap(), 0, req);
   return 0;
}

/* In place of the stand-in's ShowNormalKeyboard: what the game asks, then
 * the answer on a thread of its own (as eden.exe's keyboard answers later,
 * from its UI). The label: the header, else the guide, else the sub text. */
static void show_normal_keyboard(uint8_t *self)
{
   struct request *req = (struct request*)HeapAlloc(GetProcessHeap(),
         HEAP_ZERO_MEMORY, sizeof(*req));
   HANDLE thread;

   if (!req)
      return;
   req->self = self;
   req->max_len = *(uint32_t*)(self + OFF_MAX_LEN);
   if (req->max_len > TEXT_MAX)
      req->max_len = TEXT_MAX;
   if (     !(req->label_len = read_str(self, OFF_HEADER, req->label))
         && !(req->label_len = read_str(self, OFF_GUIDE, req->label)))
      req->label_len = read_str(self, OFF_SUB, req->label);
   req->initial_len = read_str(self, OFF_INITIAL, req->initial);

   if ((thread = CreateThread(NULL, 0, ask_main, req, 0, NULL)))
      CloseHandle(thread);
   else
      HeapFree(GetProcessHeap(), 0, req);
}

/* ---------------------------------------------------------------------- */
/* Finding and patching                                                   */
/* ---------------------------------------------------------------------- */

static uint8_t *image;
static IMAGE_NT_HEADERS64 *nt;
static IMAGE_SECTION_HEADER *sec;

/* The function holding code at rva (its first part, for a split one). */
static const RUNTIME_FUNCTION *function_at(uint32_t rva)
{
   const IMAGE_DATA_DIRECTORY *dir =
      &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
   const RUNTIME_FUNCTION *f = (const RUNTIME_FUNCTION*)(image + dir->VirtualAddress);
   size_t lo = 0, hi = dir->Size / sizeof(*f);
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
      const uint8_t *ui = image + hit->UnwindData;
      if (!((ui[0] >> 3) & 4))
         break;
      hit = (const RUNTIME_FUNCTION*)(ui + 4 + ((ui[2] + 1) & ~1) * 2);
   }
   return hit;
}

/* Where the data string s is (its rva), 0 if not found. */
static uint32_t find_string(const char *s)
{
   size_t n = strlen(s) + 1;
   int i;

   for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
   {
      uint8_t *p   = image + sec[i].VirtualAddress;
      uint32_t len = sec[i].Misc.VirtualSize, k;
      if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
         continue;
      for (k = 0; k + n <= len; k++)
         if (p[k] == (uint8_t)s[0] && !memcmp(p + k, s, n))
            return sec[i].VirtualAddress + k;
   }
   return 0;
}

/* The one function loading the address at rva (lea r64, [rip + disp32]). */
static const RUNTIME_FUNCTION *function_using(uint32_t rva)
{
   const RUNTIME_FUNCTION *found = NULL, *f;
   int i;

   for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
   {
      uint8_t *p   = image + sec[i].VirtualAddress;
      uint32_t len = sec[i].Misc.VirtualSize, k;
      if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
         continue;
      for (k = 0; k + 7 <= len; k++)
      {
         int32_t disp;
         uint32_t at;
         if (     (p[k] != 0x48 && p[k] != 0x4C) || p[k + 1] != 0x8D
               || (p[k + 2] & 0xC7) != 0x05)
            continue;
         memcpy(&disp, p + k + 3, 4);
         at = sec[i].VirtualAddress + k;
         if ((int64_t)at + 7 + disp != rva || !(f = function_at(at)))
            continue;
         if (found && found != f)
            return NULL;
         found = f;
      }
   }
   return found;
}

static bool has_bytes(const uint8_t *p, size_t len, const uint8_t *b, size_t n)
{
   size_t k;
   for (k = 0; k + n <= len; k++)
      if (!memcmp(p + k, b, n))
         return true;
   return false;
}

/* SubmitNormalText: the function called from ShowNormalKeyboard that
 * calls the callback at +0xd0 (lea rcx, [rsi+0xd0]; call [rsi+0xe8]). */
static submit_fn find_submit(const RUNTIME_FUNCTION *show)
{
   static const uint8_t lea[]  = { 0x48, 0x8D, 0x8E, 0xD0, 0x00, 0x00, 0x00 };
   static const uint8_t call[] = { 0xFF, 0x96, 0xE8, 0x00, 0x00, 0x00 };
   uint32_t k;

   for (k = show->BeginAddress; k + 5 <= show->EndAddress; k++)
   {
      int32_t disp;
      uint32_t target;
      const RUNTIME_FUNCTION *f;
      if (image[k] != 0xE8)
         continue;
      memcpy(&disp, image + k + 1, 4);
      target = (uint32_t)((int64_t)k + 5 + disp);
      if (     (f = function_at(target)) && f->BeginAddress == target
            && f->EndAddress - target < 0x200
            && has_bytes(image + target, f->EndAddress - target, lea, sizeof(lea))
            && has_bytes(image + target, f->EndAddress - target, call, sizeof(call)))
         return (submit_fn)(void*)(image + target);
   }
   return NULL;
}

static bool install_keyboard(void)
{
   const RUNTIME_FUNCTION *show;
   uint32_t msg;
   uint8_t patch[12] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };
   uint8_t *at;
   void *hook = (void*)show_normal_keyboard;
   DWORD old;

   if (     !(msg = find_string("(STUBBED) called, backend requested to show "
                  "the normal software keyboard."))
         || !(show = function_using(msg))
         || show->EndAddress - show->BeginAddress < sizeof(patch)
         || !(submit = find_submit(show)))
      return false;

   /* mov rax, hook; jmp rax */
   at = image + show->BeginAddress;
   memcpy(patch + 2, &hook, 8);
   if (!VirtualProtect(at, sizeof(patch), PAGE_EXECUTE_READWRITE, &old))
      return false;
   memcpy(at, patch, sizeof(patch));
   VirtualProtect(at, sizeof(patch), old, &old);
   FlushInstructionCache(GetCurrentProcess(), at, sizeof(patch));
   return true;
}

/* ---------------------------------------------------------------------- */
/* TCP outside the room                                                   */
/* ---------------------------------------------------------------------- */

/* In a room, every socket the game makes is a ProxySocket (BSD_USA::
 * SocketImpl, service/sockets/bsd.cpp: "room_member->IsConnected() ?
 * ProxySocket : Socket"), which carries datagrams over the room only: its
 * Connect, Send and Recv are stubs, so TCP through the room never works.
 * Here SocketImpl's IsConnected() answers false for stream sockets, so
 * those are real sockets (a game's own servers, through RetroArch's shared
 * ports); datagrams stay in the room, for LAN play.
 *
 * Found by what SocketImpl logs ("New socket fd={}"): the register keeping
 * its type (mov from r8d, the third argument, at the start; masked with
 * and 0xdfffffff), and its one IsConnected() call (call; test al, al;
 * jz/je). That call goes to a stub near Eden's image:
 *   cmp <type>, 1 (Type::STREAM); jne real; xor eax, eax; ret
 *   real: jmp [IsConnected] */

#define TYPE_STREAM 1

/* The register (0-15) SocketImpl keeps its type in, -1 if not found. */
static int find_type_reg(const RUNTIME_FUNCTION *f)
{
   const uint8_t *p = image + f->BeginAddress;
   uint32_t len     = f->EndAddress - f->BeginAddress, k;
   int reg          = -1;

   for (k = 0; k + 3 <= len && k < 0x40; k++)
   {
      /* mov r32, r8d: 44/45 89 (11 000 rm), or 41/45 8B (11 reg 000). */
      if ((p[k] == 0x44 || p[k] == 0x45) && p[k + 1] == 0x89
            && (p[k + 2] & 0xF8) == 0xC0)
         reg = (p[k + 2] & 7) | (p[k] == 0x45 ? 8 : 0);
      else if ((p[k] == 0x41 || p[k] == 0x45) && p[k + 1] == 0x8B
            && (p[k + 2] & 0xC7) == 0xC0)
         reg = ((p[k + 2] >> 3) & 7) | (p[k] == 0x45 ? 8 : 0);
      if (reg >= 0)
         break;
   }
   /* Only one kept across calls (rbx, rbp, rsi, rdi, r12-r15). */
   if (reg < 0 || reg == 0 || reg == 1 || reg == 2 || reg == 4
         || (reg >= 8 && reg <= 11))
      return -1;

   /* and <reg>, 0xdfffffff: the type, its flag bit cleared. */
   for (k = 0; k + 7 <= len; k++)
   {
      const uint8_t *q = p + k;
      if (reg >= 8 && q[0] != 0x41)
         continue;
      if (reg >= 8)
         q++;
      if (     q[0] == 0x81 && q[1] == (0xE0 | (reg & 7))
            && q[2] == 0xFF && q[3] == 0xFF && q[4] == 0xFF && q[5] == 0xDF)
         return reg;
   }
   return -1;
}

/* The one "call rel32; test al, al; jz" in SocketImpl (IsConnected()),
 * NULL if not exactly one. */
static uint8_t *find_is_connected_call(const RUNTIME_FUNCTION *f)
{
   uint8_t *p   = image + f->BeginAddress;
   uint32_t len = f->EndAddress - f->BeginAddress, k;
   uint8_t *hit = NULL;

   for (k = 0; k + 11 <= len; k++)
      if (     p[k] == 0xE8 && p[k + 5] == 0x84 && p[k + 6] == 0xC0
            && (p[k + 7] == 0x74 || (p[k + 7] == 0x0F && p[k + 8] == 0x84)))
      {
         if (hit)
            return NULL;
         hit = p + k;
      }
   return hit;
}

/* Memory for code within reach of a rel32 from near (2 GB). */
static uint8_t *alloc_near(uint8_t *near_at, size_t size)
{
   uintptr_t base = (uintptr_t)near_at & ~(uintptr_t)0xFFFF;
   uintptr_t step;

   for (step = 0x10000; step < 0x7FF00000; step += 0x10000)
   {
      void *m;
      if (base > step && (m = VirtualAlloc((void*)(base - step), size,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
         return (uint8_t*)m;
      if ((m = VirtualAlloc((void*)(base + step), size,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
         return (uint8_t*)m;
   }
   return NULL;
}

static bool install_sockets(void)
{
   const RUNTIME_FUNCTION *f;
   uint32_t msg;
   uint8_t *call, *stub, *real;
   int32_t disp;
   int reg;
   size_t n = 0;
   DWORD old;

   if (     !(msg = find_string("New socket fd={}"))
         || !(f = function_using(msg))
         || (reg = find_type_reg(f)) < 0
         || !(call = find_is_connected_call(f))
         || !(stub = alloc_near(call, 64)))
      return false;
   memcpy(&disp, call + 1, 4);
   real = call + 5 + disp;

   /* cmp <reg>, TYPE_STREAM */
   if (reg >= 8)
      stub[n++] = 0x41;
   stub[n++] = 0x83;
   stub[n++] = (uint8_t)(0xF8 | (reg & 7));
   stub[n++] = TYPE_STREAM;
   /* jne real; xor eax, eax; ret */
   stub[n++] = 0x75;
   stub[n++] = 0x03;
   stub[n++] = 0x31;
   stub[n++] = 0xC0;
   stub[n++] = 0xC3;
   /* real: jmp [rip + 0]; IsConnected */
   stub[n++] = 0xFF;
   stub[n++] = 0x25;
   memset(stub + n, 0, 4);
   n += 4;
   memcpy(stub + n, &real, 8);
   n += 8;
   FlushInstructionCache(GetCurrentProcess(), stub, n);

   disp = (int32_t)(stub - (call + 5));
   if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old))
      return false;
   memcpy(call + 1, &disp, 4);
   VirtualProtect(call, 5, old, &old);
   FlushInstructionCache(GetCurrentProcess(), call, 5);
   return true;
}

/* ---------------------------------------------------------------------- */
/* Players joining                                                        */
/* ---------------------------------------------------------------------- */

/* Eden connects a player only as its config says, when the game first asks
 * for controllers (EmulatedController::ReloadFromSettings, one per
 * controller from HIDCore::ReloadInputDevices), and players 2-8 never after.
 * The core writes them as not connected and says which have joined (played
 * on their RetroArch port) in memory named after this process: each one is
 * connected here with Eden's own EmulatedController::Connect, at once and
 * after every reload.
 *
 * Connect is found by what it logs ("Controller type {} is not
 * supported"); ReloadFromSettings as the function calling it that another
 * function calls 8 times or more, not SetSupportedNpadStyleTag (which logs
 * "Reconnecting controller type {} as Pro controller"). Those calls go
 * through reload_hook, which learns each controller: its NpadIdType is its
 * first member (Player1-8: 0-7). */

#define PLAYERS   8
#define MAX_SITES 16

typedef void (*connect_fn)(void *self, bool use_temporary_value);
typedef void (*reload_fn)(void *self);

/* Shared with the core (struct dsu_shared there). */
struct shared
{
   volatile LONG joined;           /* bit n: player n+1, from the core */
   volatile LONG rumble[PLAYERS];  /* strong motor | weak << 16, for it */
};

static connect_fn eden_connect;
static reload_fn eden_reload;
static void *volatile players[PLAYERS];
static struct shared *volatile shared;
static volatile LONG connected;        /* bits connected here since a reload */

static void reload_hook(void *self)
{
   uint32_t id = *(uint32_t*)self;

   eden_reload(self);
   if (id >= PLAYERS)
      return;
   players[id] = self;
   InterlockedAnd(&connected, ~(1L << id));
   if (id && shared && (shared->joined & (1L << id)))
   {
      InterlockedOr(&connected, 1L << id);
      eden_connect(self, false);
   }
}

/* The core's memory, once it's there; then players who joined since:
 * connected (if that patch went in). */
static DWORD WINAPI players_main(LPVOID unused)
{
   wchar_t name[64];
   HANDLE map = NULL;
   (void)unused;

   _snwprintf(name, 64, L"Local\\bomba-eden-pads-%lu",
         (unsigned long)GetCurrentProcessId());
   name[63] = L'\0';
   for (;; Sleep(100))
   {
      LONG mask;
      int id;

      if (!shared)
      {
         if (!map && !(map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE,
                     FALSE, name)))
            continue;
         if (!(shared = (struct shared*)MapViewOfFile(map,
                     FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(struct shared))))
            continue;
      }
      if (!eden_connect)
         continue;
      mask = shared->joined & ~connected;
      for (id = 1; id < PLAYERS; id++)
         if ((mask & (1L << id)) && players[id])
         {
            InterlockedOr(&connected, 1L << id);
            eden_connect(players[id], false);
         }
   }
   return 0;
}

/* The function containing the call at rva's start, or 0. */
static uint32_t caller_of(uint32_t at)
{
   const RUNTIME_FUNCTION *f = function_at(at);
   return f ? f->BeginAddress : 0;
}

/* Every "call rel32" in the code to one of targets[n]: fn(site, target). */
static void each_call(const uint32_t *targets, int n,
      void (*fn)(uint8_t *site, uint32_t target, void *ctx), void *ctx)
{
   int i, t;

   for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
   {
      uint8_t *p   = image + sec[i].VirtualAddress;
      uint32_t len = sec[i].Misc.VirtualSize, k;
      if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
         continue;
      for (k = 0; k + 5 <= len; k++)
      {
         int32_t disp;
         uint32_t target;
         if (p[k] != 0xE8)
            continue;
         memcpy(&disp, p + k + 1, 4);
         target = (uint32_t)((int64_t)sec[i].VirtualAddress + k + 5 + disp);
         for (t = 0; t < n; t++)
            if (target == targets[t])
               fn(p + k, target, ctx);
      }
   }
}

/* The functions calling Connect. */
struct callers { uint32_t fn[32]; int n; };

static void add_caller(uint8_t *site, uint32_t target, void *ctx)
{
   struct callers *c = (struct callers*)ctx;
   uint32_t f = caller_of((uint32_t)(site - image));
   int i;
   (void)target;
   for (i = 0; i < c->n && c->fn[i] != f; i++);
   if (f && i == c->n && c->n < 32)
      c->fn[c->n++] = f;
}

/* Calls to those functions, by (callee, caller). */
struct sites
{
   struct { uint32_t callee, caller; int n; uint8_t *site[MAX_SITES]; } e[64];
   int n;
};

static void add_site(uint8_t *site, uint32_t target, void *ctx)
{
   struct sites *s = (struct sites*)ctx;
   uint32_t f = caller_of((uint32_t)(site - image));
   int i;

   for (i = 0; i < s->n && !(s->e[i].callee == target && s->e[i].caller == f); i++);
   if (i == s->n)
   {
      if (s->n == 64)
         return;
      s->e[i].callee = target;
      s->e[i].caller = f;
      s->e[i].n      = 0;
      s->n++;
   }
   if (s->e[i].n < MAX_SITES)
      s->e[i].site[s->e[i].n] = site;
   s->e[i].n++;
}

static bool install_players(void)
{
   static struct sites sites;
   struct callers callers = { { 0 }, 0 };
   const RUNTIME_FUNCTION *f;
   uint32_t msg, connect, skip = 0;
   uint8_t *stub;
   void *hook = (void*)reload_hook;
   int i, k, found = -1;

   if (     !(msg = find_string("Controller type {} is not supported"))
         || !(f = function_using(msg)))
      return false;
   connect = f->BeginAddress;
   if ((msg = find_string("Reconnecting controller type {} as Pro controller"))
         && (f = function_using(msg)))
      skip = f->BeginAddress;

   each_call(&connect, 1, add_caller, &callers);
   sites.n = 0;
   each_call(callers.fn, callers.n, add_site, &sites);
   for (i = 0; i < sites.n; i++)
      if (     sites.e[i].n >= 8 && sites.e[i].n <= MAX_SITES
            && sites.e[i].callee != skip && sites.e[i].caller)
      {
         if (found >= 0)
            return false;
         found = i;
      }
   if (found < 0 || !(stub = alloc_near(sites.e[found].site[0], 16)))
      return false;

   eden_connect = (connect_fn)(void*)(image + connect);
   eden_reload  = (reload_fn)(void*)(image + sites.e[found].callee);

   /* mov rax, hook; jmp rax */
   stub[0] = 0x48;
   stub[1] = 0xB8;
   memcpy(stub + 2, &hook, 8);
   stub[10] = 0xFF;
   stub[11] = 0xE0;
   FlushInstructionCache(GetCurrentProcess(), stub, 12);

   for (k = 0; k < sites.e[found].n; k++)
   {
      uint8_t *call = sites.e[found].site[k];
      int32_t disp  = (int32_t)(stub - (call + 5));
      DWORD old;
      if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old))
         return false;
      memcpy(call + 1, &disp, 4);
      VirtualProtect(call, 5, old, &old);
      FlushInstructionCache(GetCurrentProcess(), call, 5);
   }
   return true;
}

/* ---------------------------------------------------------------------- */
/* Vibration                                                              */
/* ---------------------------------------------------------------------- */

/* The players' pads are the core's (Eden's cemuhookudp driver, UDPClient),
 * whose protocol has no vibration: UDPClient keeps InputEngine's
 * stand-ins, SetVibration "not supported" and IsVibrationEnabled false.
 * Here they hand Eden's vibration (its strength setting applied) to the
 * core, which rumbles the pad on the player's RetroArch port; pad n is
 * player n+1. Amplitudes become motor strengths as Eden's SDL driver makes
 * them (SDLDriver::SetVibration, for pads without HD rumble), the low band
 * the strong motor.
 *
 * Found in UDPClient's vtable: the slot of its GetInputDevices (which
 * formats "UDP Controller {}"), with SetVibration 11 slots before and
 * IsVibrationEnabled 10, if those still are the stand-ins
 * (mov eax, 10 (DriverResult::NotSupported); ret / xor eax, eax; ret). */

#define DRIVER_SUCCESS       0
#define DRIVER_NOT_SUPPORTED 10

/* Eden's PadIdentifier and Common::Input::VibrationStatus. */
struct pad_id { uint8_t guid[16]; size_t port, pad; };
struct vibration
{
   float low_amplitude, low_frequency, high_amplitude, high_frequency;
   int type; /* 0 linear, 1 exponential */
};

static uint16_t motor(float amplitude, int type)
{
   float factor = type == 0 ? 0.5f : 0.35f;
   if (!(amplitude > 0.0f))
      return 0;
   if (amplitude > 1.0f)
      amplitude = 1.0f;
   return (uint16_t)((amplitude + powf(amplitude, factor)) * 0.5f * 65535.0f);
}

static int set_vibration(void *self, const struct pad_id *id,
      const struct vibration *v)
{
   (void)self;
   if (id->pad >= PLAYERS || !shared)
      return DRIVER_NOT_SUPPORTED;
   InterlockedExchange(&shared->rumble[id->pad],
         (LONG)(motor(v->low_amplitude, v->type)
            | ((uint32_t)motor(v->high_amplitude, v->type) << 16)));
   return DRIVER_SUCCESS;
}

static bool is_vibration_enabled(void *self, const struct pad_id *id)
{
   (void)self;
   return id->pad < PLAYERS;
}

/* p is the start of a function in Eden's code. */
static bool in_code(const uint8_t *p)
{
   int i;
   for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
      if (     (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            && p >= image + sec[i].VirtualAddress
            && p <  image + sec[i].VirtualAddress + sec[i].Misc.VirtualSize)
         return true;
   return false;
}

static bool install_vibration(void)
{
   static const uint8_t not_supported[] = { 0xB8, 0x0A, 0x00, 0x00, 0x00, 0xC3 };
   static const uint8_t no[]            = { 0x31, 0xC0, 0xC3 };
   const RUNTIME_FUNCTION *f;
   uint8_t *want, **slot = NULL, **vt;
   void *set = (void*)set_vibration, *enabled = (void*)is_vibration_enabled;
   uint32_t msg;
   DWORD old;
   int i;

   if (!(msg = find_string("UDP Controller {}")) || !(f = function_using(msg)))
      return false;
   want = image + f->BeginAddress;

   for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
   {
      uint8_t *p   = image + sec[i].VirtualAddress;
      uint32_t len = sec[i].Misc.VirtualSize, k;
      if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
         continue;
      for (k = 11 * 8; k + 8 <= len; k += 8)
         if (*(uint8_t**)(p + k) == want)
         {
            if (slot)
               return false;
            slot = (uint8_t**)(p + k);
         }
   }
   if (!slot)
      return false;
   vt = slot - 11;
   if (     !in_code(vt[0]) || memcmp(vt[0], not_supported, sizeof(not_supported))
         || !in_code(vt[1]) || memcmp(vt[1], no, sizeof(no)))
      return false;

   if (!VirtualProtect(vt, 2 * sizeof(void*), PAGE_READWRITE, &old))
      return false;
   vt[0] = (uint8_t*)set;
   vt[1] = (uint8_t*)enabled;
   VirtualProtect(vt, 2 * sizeof(void*), old, &old);
   return true;
}

/* ---------------------------------------------------------------------- */
/* Settings while the game runs                                           */
/* ---------------------------------------------------------------------- */

/* The core's options (Eden's settings) changed in RetroArch while the game
 * runs go straight into Eden's Settings::values, as Eden's own settings
 * dialog sets them: only those Eden marks as changeable while a game runs
 * (runtime_modifiable), each as SwitchableSetting::SetValue does (the
 * per-game value if the game has one). Then what Eden does after: the
 * aspect ratio needs the picture laid out again (SDL's "exposed" event,
 * EmuWindow_SDL2::OnResize), the console mode the game told
 * (WindowSystem::OnOperationModeChanged, what Eden's docked toggle ends
 * in). The core names the settings and their values in memory named after
 * this process; what can change here goes back to it.
 *
 * A setting is found by its label (key in the config), a libstdc++
 * std::string kept in the object: Setting<T> (BasicSetting: vptr, label
 * +0x08, runtime_modifiable +0x31; value +0x40), the virtual base of a
 * SwitchableSetting<T> 0x10 before it (vptr, use_global +0x08, custom
 * +0x0c). Only 4-byte values (the core's are all enums).
 *
 * The WindowSystem is learnt from its Update (logs "called, home_menu={}
 * application={} overlay={}"), which the game's start calls; its
 * OnOperationModeChanged is two functions before SendButtonAppletMessage-
 * Locked, the function before OnSystemButtonPress (logs "Overlay long-press
 * toggle: ..."), in source order with OnExitRequested between, both alike
 * but for the one lifecycle call. */

#define SETTINGS 16

/* Shared with the core (struct settings_shared there). */
struct settings_shared
{
   volatile LONG seq;              /* from the core: bumped on a change */
   volatile LONG live;             /* bit n: setting n changes here */
   struct { char key[32]; volatile LONG value; } item[SETTINGS]; /* -1: none */
};

typedef void (*window_fn)(void *self);

static window_fn opmode_changed;
static uint8_t *update_stub;          /* its last 8 bytes: the WindowSystem */

static bool in_image(const void *p)
{
   return (const uint8_t*)p >= image
      && (const uint8_t*)p < image + nt->OptionalHeader.SizeOfImage;
}

/* The SwitchableSetting with label key in Eden's data, NULL if none (or not
 * as above, or not changeable while the game runs). */
static uint8_t *find_setting(const char *key)
{
   size_t n = strlen(key);
   uint8_t *found = NULL;
   int i;

   for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
   {
      uint8_t *p   = image + sec[i].VirtualAddress;
      uint32_t len = sec[i].Misc.VirtualSize, k;
      if (     (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            || !(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE))
         continue;
      for (k = 0x18; k + 16 + n + 1 <= len; k += 8)
      {
         uint8_t *str = p + k, *base = str - 8, *sw = base - 0x10;
         uint8_t *vt;
         if (     *(uint8_t**)str != str + 16 || *(size_t*)(str + 8) != n
               || memcmp(str + 16, key, n + 1))
            continue;
         vt = *(uint8_t**)sw;
         if (     !in_image(*(uint8_t**)base) || !in_image(vt)
               || *(int64_t*)(vt - 0x18) != 0x10 || base[0x31] > 1 || sw[8] > 1)
            continue;
         if (found)
            return NULL;
         found = sw;
      }
   }
   if (found && !found[0x10 + 0x31])
      return NULL;
   return found;
}

/* The value Eden uses now. */
static volatile LONG *setting_value(uint8_t *sw)
{
   return sw[8] ? (volatile LONG*)(sw + 0x10 + 0x40) : (volatile LONG*)(sw + 0x0c);
}

static BOOL CALLBACK expose_window(HWND hwnd, LPARAM unused)
{
   DWORD pid = 0;
   (void)unused;
   GetWindowThreadProcessId(hwnd, &pid);
   if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd))
      RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE);
   return TRUE;
}

static DWORD WINAPI settings_main(LPVOID unused)
{
   uint8_t *found[SETTINGS] = { 0 };
   struct settings_shared *s = NULL;
   wchar_t name[64];
   HANDLE map = NULL;
   LONG seen = 0;
   int i;
   (void)unused;

   _snwprintf(name, 64, L"Local\\bomba-eden-settings-%lu",
         (unsigned long)GetCurrentProcessId());
   name[63] = L'\0';
   for (;; Sleep(100))
   {
      if (!map && !(map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE,
                  FALSE, name)))
         continue;
      if (!(s = (struct settings_shared*)MapViewOfFile(map,
                  FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(*s))))
         continue;
      break;
   }

   /* Eden's settings exist before its main(), well before this. */
   for (i = 0; i < SETTINGS && s->item[i].key[0]; i++)
   {
      char key[32];
      memcpy(key, s->item[i].key, sizeof(key));
      key[31] = '\0';
      found[i] = find_setting(key);
      if (found[i] && !strcmp(key, "use_docked_mode") && !opmode_changed)
         found[i] = NULL;
      if (found[i])
         InterlockedOr(&s->live, 1L << i);
   }

   for (;; Sleep(100))
   {
      bool expose = false;
      LONG seq = s->seq;
      if (seq == seen)
         continue;
      seen = seq;

      for (i = 0; i < SETTINGS; i++)
      {
         LONG value = s->item[i].value;
         volatile LONG *at;
         if (!found[i] || value < 0 || *(at = setting_value(found[i])) == value)
            continue;
         if (!strcmp(s->item[i].key, "use_docked_mode"))
         {
            /* Not before the game started (it reads it then). */
            void *ws = *(void *volatile*)(update_stub + 21);
            if (!ws)
               continue;
            *at = value;
            opmode_changed(ws);
            continue;
         }
         *at = value;
         if (!strcmp(s->item[i].key, "aspect_ratio"))
            expose = true;
      }
      if (expose)
         EnumWindows(expose_window, 0);
   }
   return 0;
}

/* The rel32 calls in f, up to max: their targets. */
static int calls_in(const RUNTIME_FUNCTION *f, uint32_t *out, int max)
{
   uint32_t k;
   int n = 0;
   for (k = f->BeginAddress; k + 5 <= f->EndAddress && n < max; k++)
   {
      int32_t disp;
      if (image[k] != 0xE8)
         continue;
      memcpy(&disp, image + k + 1, 4);
      out[n++] = (uint32_t)((int64_t)k + 5 + disp);
   }
   return n;
}

static bool calls_to(const RUNTIME_FUNCTION *f, uint32_t target)
{
   uint32_t c[64];
   int n = calls_in(f, c, 64), i;
   for (i = 0; i < n; i++)
      if (c[i] == target)
         return true;
   return false;
}

/* The function before f in Eden's code (the exception table is sorted). */
static const RUNTIME_FUNCTION *function_before(const RUNTIME_FUNCTION *f)
{
   const IMAGE_DATA_DIRECTORY *dir =
      &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
   const RUNTIME_FUNCTION *first = (const RUNTIME_FUNCTION*)(image + dir->VirtualAddress);
   if (f <= first)
      return NULL;
   return function_at(f[-1].BeginAddress) == &f[-1] ? &f[-1] : NULL;
}

/* opmode and exit: the same calls but for one each. */
static bool alike(const RUNTIME_FUNCTION *a, const RUNTIME_FUNCTION *b)
{
   uint32_t ca[32], cb[32];
   int na = calls_in(a, ca, 32), nb = calls_in(b, cb, 32), i, j, odd = 0;

   if (na != nb || !na)
      return false;
   for (i = 0; i < na; i++)
   {
      for (j = 0; j < nb && cb[j] != ca[i]; j++);
      if (j == nb)
         odd++;
   }
   return odd == 1;
}

static bool install_window_system(void)
{
   static const char *update_msg = "called, home_menu={} application={} overlay={}";
   static const char *button_msg = "Overlay long-press toggle: "
         "overlay_in_foreground={} window_visible={}";
   const RUNTIME_FUNCTION *update, *button, *send, *exit_req, *opmode;
   uint32_t msg, target;
   uint8_t *site = NULL, *stub;
   uint8_t *real;
   int32_t disp;
   DWORD old;
   int i;

   if (     !(msg = find_string(update_msg)) || !(update = function_using(msg))
         || !(msg = find_string(button_msg)) || !(button = function_using(msg))
         || !(send = function_before(button))
         || !calls_to(button, send->BeginAddress)
         || !(exit_req = function_before(send))
         || !(opmode = function_before(exit_req))
         || !alike(opmode, exit_req))
      return false;

   /* Update's one caller. */
   target = update->BeginAddress;
   for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
   {
      uint8_t *p   = image + sec[i].VirtualAddress;
      uint32_t len = sec[i].Misc.VirtualSize, k;
      if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
         continue;
      for (k = 0; k + 5 <= len; k++)
      {
         if (p[k] != 0xE8)
            continue;
         memcpy(&disp, p + k + 1, 4);
         if ((uint32_t)((int64_t)sec[i].VirtualAddress + k + 5 + disp) != target)
            continue;
         if (site)
            return false;
         site = p + k;
      }
   }
   if (!site || !(stub = alloc_near(site, 32)))
      return false;

   /* mov [rip + 14], rcx; jmp [rip + 0]; Update; the WindowSystem */
   real = image + target;
   memcpy(stub, "\x48\x89\x0D\x0E\x00\x00\x00\xFF\x25\x00\x00\x00\x00", 13);
   memcpy(stub + 13, &real, 8);
   memset(stub + 21, 0, 8);
   FlushInstructionCache(GetCurrentProcess(), stub, 29);
   update_stub = stub;

   disp = (int32_t)(stub - (site + 5));
   if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old))
      return false;
   memcpy(site + 1, &disp, 4);
   VirtualProtect(site, 5, old, &old);
   FlushInstructionCache(GetCurrentProcess(), site, 5);

   opmode_changed = (window_fn)(void*)(image + opmode->BeginAddress);
   return true;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
   bool players_in, vibration;
   HANDLE t;
   (void)inst;
   (void)reserved;
   if (reason != DLL_PROCESS_ATTACH)
      return TRUE;

   image = (uint8_t*)GetModuleHandleW(NULL);
   nt    = (IMAGE_NT_HEADERS64*)(image + ((IMAGE_DOS_HEADER*)image)->e_lfanew);
   if (     nt->Signature != IMAGE_NT_SIGNATURE
         || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
      return FALSE;
   sec = IMAGE_FIRST_SECTION(nt);

   install_keyboard();
   install_sockets();
   players_in = install_players();
   vibration  = install_vibration();
   install_window_system();
   if (players_in || vibration)
   {
      if ((t = CreateThread(NULL, 0, players_main, NULL, 0, NULL)))
         CloseHandle(t);
   }
   /* Settings are found by their labels, not Eden's code: always. */
   if ((t = CreateThread(NULL, 0, settings_main, NULL, 0, NULL)))
      CloseHandle(t);
   return TRUE;
}
