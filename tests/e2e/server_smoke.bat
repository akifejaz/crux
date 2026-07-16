@echo off
REM Dashboard server smoke -- starts crux_server on a test port, hits the
REM endpoints with curl.exe (Windows 10 1803+), verifies they respond,
REM then kills the server.
REM
REM Does NOT exercise real job execution -- that would require yt-dlp to
REM hit the network. Only checks that the HTTP layer is wired correctly.

setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0..\.."

set /a FAIL=0
set "PORT=18181"
set "BASE=http://127.0.0.1:%PORT%"

set "SERVER=build\Release\crux_server.exe"
if not exist "%SERVER%" set "SERVER=build\crux_server.exe"
if not exist "%SERVER%" (
    echo [error] crux_server.exe not built. Run rebuild.bat first.
    exit /b 1
)
where curl.exe >nul 2>&1
if errorlevel 1 (
    echo [skip] curl.exe not available -- cannot exercise dashboard endpoints.
    exit /b 0
)

echo === starting crux_server on port %PORT% ===
start "crux_server_test" /B "%SERVER%" --port %PORT% >nul 2>&1

REM Wait for the server to open the port (poll for up to 5s).
set /a TRIES=25
:wait
curl.exe -sS -o nul "%BASE%/api/info" >nul 2>&1
if not errorlevel 1 goto ready
set /a TRIES-=1
if %TRIES%==0 (
    echo [error] server never opened %PORT%
    goto :kill_and_exit_fail
)
timeout /t 1 /nobreak >nul
goto wait

:ready
echo   ok    server is up

echo.
echo === GET /api/info ===
curl.exe -sS "%BASE%/api/info" > "%TEMP%\crux_info.json"
findstr /C:"ytshorts_exe" "%TEMP%\crux_info.json" >nul || findstr /C:"crux" "%TEMP%\crux_info.json" >nul
if errorlevel 1 (echo   MISS && set /a FAIL+=1) else (echo   ok)

echo.
echo === GET / (dashboard HTML served) ===
curl.exe -sS "%BASE%/" > "%TEMP%\crux_root.html"
findstr /C:"<title>crux" "%TEMP%\crux_root.html" >nul
if errorlevel 1 (echo   MISS && set /a FAIL+=1) else (echo   ok)

echo.
echo === GET /logo.svg (static asset served) ===
curl.exe -sS -o nul -w "%%{http_code}" "%BASE%/logo.svg" > "%TEMP%\crux_status.txt"
set /p STATUS=<"%TEMP%\crux_status.txt"
if "%STATUS%"=="200" (echo   ok ^(HTTP 200^)) else (echo   MISS ^(HTTP %STATUS%^) && set /a FAIL+=1)

echo.
echo === GET /api/jobs (list -- empty ok) ===
curl.exe -sS "%BASE%/api/jobs" > "%TEMP%\crux_jobs.json"
findstr /C:"jobs" "%TEMP%\crux_jobs.json" >nul
if errorlevel 1 (echo   MISS && set /a FAIL+=1) else (echo   ok)

echo.
echo === POST /api/run without url returns 400 ===
curl.exe -sS -X POST -H "Content-Type: application/json" -d "{}" -o "%TEMP%\crux_err.json" -w "%%{http_code}" "%BASE%/api/run" > "%TEMP%\crux_status.txt"
set /p STATUS=<"%TEMP%\crux_status.txt"
if "%STATUS%"=="400" (echo   ok ^(HTTP 400^)) else (echo   MISS ^(HTTP %STATUS%^) && set /a FAIL+=1)

echo.
echo === GET /api/jobs/DOES_NOT_EXIST returns 404 ===
curl.exe -sS -o nul -w "%%{http_code}" "%BASE%/api/jobs/DOES_NOT_EXIST" > "%TEMP%\crux_status.txt"
set /p STATUS=<"%TEMP%\crux_status.txt"
if "%STATUS%"=="404" (echo   ok ^(HTTP 404^)) else (echo   MISS ^(HTTP %STATUS%^) && set /a FAIL+=1)

echo.
echo === killing server ===
taskkill /F /IM crux_server.exe >nul 2>&1

del /q "%TEMP%\crux_info.json"   >nul 2>&1
del /q "%TEMP%\crux_root.html"    >nul 2>&1
del /q "%TEMP%\crux_jobs.json"    >nul 2>&1
del /q "%TEMP%\crux_err.json"     >nul 2>&1
del /q "%TEMP%\crux_status.txt"   >nul 2>&1

echo.
if %FAIL%==0 (
    echo === server_smoke: PASS ===
    exit /b 0
) else (
    echo === server_smoke: %FAIL% failure^(s^) ===
    exit /b 1
)


:kill_and_exit_fail
taskkill /F /IM crux_server.exe >nul 2>&1
exit /b 1
