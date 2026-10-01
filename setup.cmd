@echo off
rem One-command setup for Windows. Installs missing tools, fetches dependencies, builds.
rem Options: -CheckOnly  -Yes  -DepsOnly  -Config debug
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\setup.ps1" %*
exit /b %ERRORLEVEL%
