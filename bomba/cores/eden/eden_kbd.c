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
 * Also: TCP sockets outside Eden's room (see install_sockets). The DLL
 * loads if either patch went in. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
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

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
   bool keyboard, sockets;
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

   keyboard = install_keyboard();
   sockets  = install_sockets();
   return keyboard || sockets;
}
