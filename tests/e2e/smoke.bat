@echo off
REM End-to-end smoke test -- verifies the built binaries respond correctly.
REM Called by test-all.bat. Exits non-zero on any failure.

setlocal EnableExtensions
cd /d "%~dp0..\.."

set /a FAIL=0

set "CRUX=build\Release\crux.exe"
if not exist "%CRUX%" set "CRUX=build\crux.exe"
set "SERVER=build\Release\crux_server.exe"
if not exist "%SERVER%" set "SERVER=build\crux_server.exe"
set "TESTS=build\Release\crux_tests.exe"
if not exist "%TESTS%" set "TESTS=build\crux_tests.exe"
set "STDVERIFY=build\Release\crux_standalone_verify.exe"
if not exist "%STDVERIFY%" set "STDVERIFY=build\crux_standalone_verify.exe"

echo === executable presence ===
call :check_exists "%CRUX%"      || set /a FAIL+=1
call :check_exists "%SERVER%"    || set /a FAIL+=1
call :check_exists "%TESTS%"     || set /a FAIL+=1
call :check_exists "%STDVERIFY%" || set /a FAIL+=1

echo.
echo === crux --version ===
"%CRUX%" --version
if errorlevel 1 (echo   MISS && set /a FAIL+=1) else (echo   ok)

echo.
echo === crux --help exits 0 ===
"%CRUX%" --help >nul
if errorlevel 1 (echo   MISS && set /a FAIL+=1) else (echo   ok)

echo.
echo === crux with missing URL exits non-zero ===
"%CRUX%" >nul 2>&1
if errorlevel 1 (echo   ok) else (echo   MISS && set /a FAIL+=1)

echo.
echo === crux with bogus URL fails cleanly (does NOT crash) ===
"%CRUX%" "not_a_real_video_id" --dry-run >nul 2>&1
if errorlevel 1 (echo   ok ^(non-zero exit, as expected^)) else (echo   MISS ^(should have failed^) && set /a FAIL+=1)

echo.
echo === unit tests (crux_tests.exe) ===
"%TESTS%"
if errorlevel 1 (echo   MISS && set /a FAIL+=1) else (echo   ok)

echo.
echo === standalone verifier ===
"%STDVERIFY%"
if errorlevel 1 (echo   MISS && set /a FAIL+=1) else (echo   ok)

echo.
if %FAIL%==0 (
    echo === smoke: PASS ===
    exit /b 0
) else (
    echo === smoke: %FAIL% failure^(s^) ===
    exit /b 1
)


:check_exists
if exist "%~1" (
    echo   ok    %~1
    exit /b 0
) else (
    echo   MISS  %~1
    exit /b 1
)
