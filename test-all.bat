@echo off
REM One-command test runner. Builds Release (if needed), runs every layer
REM of tests, and reports pass/fail. Exits non-zero on any failure so CI
REM can wrap it.
REM
REM Layers exercised (fast -> slow):
REM   1. Standalone verifier (pure functions, no deps)
REM   2. Unit tests via doctest (parser, detector, planner, CLI, manifest, proc)
REM   3. Executable smoke (--version, --help, missing-arg behaviour)
REM   4. Dashboard server smoke (endpoints via curl)

setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set /a FAIL=0

echo.
echo ###########################################################
echo # crux :: test-all
echo ###########################################################

REM ---- build ------------------------------------------------------------
if not exist "build\Release\crux.exe" (
    echo.
    echo === build ^(Release^) ===
    call rebuild.bat
    if errorlevel 1 (
        echo [error] build failed -- aborting.
        exit /b 1
    )
)

REM ---- unit + integration -----------------------------------------------
echo.
echo === layer 1/4  standalone verifier ===
set "SV=build\Release\crux_standalone_verify.exe"
if not exist "%SV%" set "SV=build\crux_standalone_verify.exe"
if not exist "%SV%" (
    echo   [skip] not built ^(cmake may need reconfigure^)
) else (
    "%SV%"
    if errorlevel 1 set /a FAIL+=1
)

echo.
echo === layer 2/4  unit tests ^(doctest^) ===
set "UT=build\Release\crux_tests.exe"
if not exist "%UT%" set "UT=build\crux_tests.exe"
if not exist "%UT%" (
    echo   [error] crux_tests not built
    set /a FAIL+=1
) else (
    "%UT%"
    if errorlevel 1 set /a FAIL+=1
)

echo.
echo === layer 3/4  executable smoke ===
call tests\e2e\smoke.bat
if errorlevel 1 set /a FAIL+=1

echo.
echo === layer 4/4  dashboard server smoke ===
call tests\e2e\server_smoke.bat
if errorlevel 1 set /a FAIL+=1

REM ---- report -----------------------------------------------------------
echo.
echo ###########################################################
if %FAIL%==0 (
    echo # ALL TESTS PASSED
    echo ###########################################################
    exit /b 0
) else (
    echo # %FAIL% LAYER^(S^) FAILED
    echo ###########################################################
    exit /b 1
)
