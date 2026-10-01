@echo off
rem Update to the newest version, refresh dependencies, rebuild.
rem Options: -Channel dev  -Force  -Check
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\update.ps1" %*
exit /b %ERRORLEVEL%
