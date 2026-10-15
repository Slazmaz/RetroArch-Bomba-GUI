@echo off
rem bash's dirname needs forward slashes.
set "script=%~dp0update.sh"
F:\msys64\usr\bin\bash.exe "%script:\=/%"
echo.
pause
