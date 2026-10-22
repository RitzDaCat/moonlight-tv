@echo off
setlocal
if not "%~1"=="" goto with_action
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0windows-host.ps1" -Action install
set "setup_exit=%errorlevel%"
echo.
pause
exit /b %setup_exit%
:with_action
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0windows-host.ps1" -Action "%~1"
exit /b %errorlevel%
