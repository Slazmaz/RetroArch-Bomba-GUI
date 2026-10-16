/* Bomba: crash reports (see bomba_crash.h). */

#include "bomba_crash.h"

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "bomba_version.h"

#define CRASH_MAX_FRAMES 64

typedef BOOL (WINAPI *minidump_write_t)(HANDLE, DWORD, HANDLE,
      MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
      PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

/* The logs folder, worked out at startup: little is safe once crashed. */
static wchar_t crash_dir[MAX_PATH];

static void crash_write(HANDLE f, const char *s)
{
   DWORD written;
   WriteFile(f, s, (DWORD)strlen(s), &written, NULL);
}

/* "module+0xoffset", e.g. retroarch.exe+0x1a2b3c. */
static void crash_write_addr(HANDLE f, const char *prefix, DWORD64 addr)
{
   char line[MAX_PATH + 64];
   char name[MAX_PATH] = "?";
   HMODULE mod         = NULL;
   wchar_t path[MAX_PATH];

   if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(uintptr_t)addr, &mod)
         && GetModuleFileNameW(mod, path, MAX_PATH))
   {
      wchar_t *base = wcsrchr(path, L'\\');
      WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : path, -1,
            name, sizeof(name), NULL, NULL);
   }

   if (mod)
      snprintf(line, sizeof(line), "%s%s+0x%llx\r\n", prefix, name,
            (unsigned long long)(addr - (DWORD64)(uintptr_t)mod));
   else
      snprintf(line, sizeof(line), "%s0x%llx\r\n", prefix,
            (unsigned long long)addr);
   crash_write(f, line);
}

static void crash_write_stack(HANDLE f, CONTEXT ctx)
{
#ifdef _WIN64
   int i;

   for (i = 0; i < CRASH_MAX_FRAMES && ctx.Rip; i++)
   {
      DWORD64 base = 0, frame = 0;
      void *handler_data;
      PRUNTIME_FUNCTION fn;

      crash_write_addr(f, "  ", ctx.Rip);

      if ((fn = RtlLookupFunctionEntry(ctx.Rip, &base, NULL)))
         RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx,
               &handler_data, &frame, NULL);
      else
      {
         /* A leaf function: the return address is on top of the stack. */
         if (IsBadReadPtr((void*)(uintptr_t)ctx.Rsp, sizeof(DWORD64)))
            break;
         ctx.Rip  = *(DWORD64*)(uintptr_t)ctx.Rsp;
         ctx.Rsp += sizeof(DWORD64);
      }
   }
#endif
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
   SYSTEMTIME t;
   wchar_t path[MAX_PATH + 64];
   char line[256];
   HANDLE f;
   HMODULE dbghelp;

   CreateDirectoryW(crash_dir, NULL);
   GetLocalTime(&t);

   swprintf(path, MAX_PATH + 64, L"%ls\\crash-%04u%02u%02u-%02u%02u%02u.txt",
         crash_dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
   f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
         FILE_ATTRIBUTE_NORMAL, NULL);
   if (f != INVALID_HANDLE_VALUE)
   {
      snprintf(line, sizeof(line),
            "Bomba GUI " BOMBA_VERSION " crashed\r\n"
            "Exception 0x%08lx at ",
            (unsigned long)ep->ExceptionRecord->ExceptionCode);
      crash_write(f, line);
      crash_write_addr(f, "",
            (DWORD64)(uintptr_t)ep->ExceptionRecord->ExceptionAddress);
      if (     ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
            && ep->ExceptionRecord->NumberParameters >= 2)
      {
         snprintf(line, sizeof(line), "%s address 0x%llx\r\n",
               ep->ExceptionRecord->ExceptionInformation[0] ? "Writing" : "Reading",
               (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
         crash_write(f, line);
      }
      crash_write(f, "Stack:\r\n");
      crash_write_stack(f, *ep->ContextRecord);
      CloseHandle(f);
   }

   if ((dbghelp = LoadLibraryW(L"dbghelp.dll")))
   {
      minidump_write_t write_dump = (minidump_write_t)(void*)
         GetProcAddress(dbghelp, "MiniDumpWriteDump");

      wcscpy(path + wcslen(path) - 3, L"dmp");
      f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL);
      if (write_dump && f != INVALID_HANDLE_VALUE)
      {
         MINIDUMP_EXCEPTION_INFORMATION info;
         info.ThreadId          = GetCurrentThreadId();
         info.ExceptionPointers = ep;
         info.ClientPointers    = FALSE;
         write_dump(GetCurrentProcess(), GetCurrentProcessId(), f,
               (MINIDUMP_TYPE)(MiniDumpNormal
                  | MiniDumpWithIndirectlyReferencedMemory),
               &info, NULL, NULL);
      }
      if (f != INVALID_HANDLE_VALUE)
         CloseHandle(f);
   }

   /* Windows still handles the crash as usual. */
   return EXCEPTION_CONTINUE_SEARCH;
}

void bomba_crash_init(void)
{
   wchar_t *slash;

   /* <install>\app\retroarch.exe -> <install>\logs */
   if (!GetModuleFileNameW(NULL, crash_dir, MAX_PATH))
      return;
   if ((slash = wcsrchr(crash_dir, L'\\')))
      *slash = L'\0';
   if ((slash = wcsrchr(crash_dir, L'\\')))
      *slash = L'\0';
   if (wcslen(crash_dir) + 6 >= MAX_PATH)
      return;
   wcscat(crash_dir, L"\\logs");

   SetUnhandledExceptionFilter(crash_filter);
}

#else

void bomba_crash_init(void) { }

#endif
