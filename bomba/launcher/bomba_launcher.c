/* bomba.exe: the install's front door, next to its user folders (roms,
 * saves, settings) with the program in app\. Runs app\launch.ps1 (which
 * starts app\retroarch.exe with its retroarch.cfg) with no console window,
 * passing its own arguments on (Steam's launch option adds its RetroArch
 * and any +connect_lobby), and waits for it, so Steam sees the game
 * running until RetroArch closes. */

#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR args, int show)
{
   wchar_t dir[MAX_PATH], cmd[32768], *slash;
   STARTUPINFOW si;
   PROCESS_INFORMATION pi;
   DWORD code = 1;

   GetModuleFileNameW(NULL, dir, MAX_PATH);
   if ((slash = wcsrchr(dir, L'\\')))
      *slash = L'\0';

   _snwprintf(cmd, 32768,
         L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden "
         L"-File \"%ls\\app\\launch.ps1\" %ls", dir, args ? args : L"");
   cmd[32767] = L'\0';

   ZeroMemory(&si, sizeof(si));
   si.cb          = sizeof(si);
   si.dwFlags     = STARTF_USESHOWWINDOW;
   si.wShowWindow = SW_HIDE;
   if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
            NULL, dir, &si, &pi))
   {
      MessageBoxW(NULL, L"Couldn't start PowerShell to run app\\launch.ps1.",
            L"Bomba", MB_ICONERROR);
      return 1;
   }
   WaitForSingleObject(pi.hProcess, INFINITE);
   GetExitCodeProcess(pi.hProcess, &code);
   CloseHandle(pi.hThread);
   CloseHandle(pi.hProcess);
   return (int)code;
}
