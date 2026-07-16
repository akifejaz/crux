@echo off
REM Launches the crux dashboard server and opens the default browser.
REM Usage:
REM   dashboard.bat            :: default port 8181
REM   dashboard.bat 9090       :: custom port

setlocal EnableExtensions
cd /d "%~dp0"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=8181"

set "EXE=build\Release\crux_server.exe"
if not exist "%EXE%" set "EXE=build\crux_server.exe"
if not exist "%EXE%" goto :missing_exe

if not exist "dashboard\index.html" goto :missing_dashboard

echo Starting crux dashboard on http://127.0.0.1:%PORT%/ ...
start "" "http://127.0.0.1:%PORT%/"
"%EXE%" --port %PORT%
exit /b %errorlevel%


:missing_exe
echo [error] crux_server.exe not built yet.
echo         First time?  Run:  setup.bat
echo         After edits? Run:  rebuild.bat
exit /b 1


:missing_dashboard
echo [error] dashboard\index.html is missing.
exit /b 1
