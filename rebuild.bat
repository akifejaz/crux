@echo off
REM Incremental rebuild -- use this after editing source files.
REM Usage:
REM   rebuild.bat            (Release, default)
REM   rebuild.bat Debug
REM   rebuild.bat --clean    (wipe build\ first)

setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "CMAKE_EXE="
set "CMAKE_CMD=cmake"
where cmake >nul 2>&1
if errorlevel 1 (
    if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
    if not defined CMAKE_EXE if exist "%ProgramFiles(x86)%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles(x86)%\CMake\bin\cmake.exe"
    if not defined CMAKE_EXE if exist "%LocalAppData%\Programs\CMake\bin\cmake.exe" set "CMAKE_EXE=%LocalAppData%\Programs\CMake\bin\cmake.exe"

    if defined CMAKE_EXE (
        set "CMAKE_CMD=!CMAKE_EXE!"
        echo [ok]  cmake found at !CMAKE_EXE!
    ) else (
        echo [error] cmake not found on PATH.
        echo         Install CMake ^>= 3.25 from https://cmake.org/download/
        echo         and re-open your shell so PATH is refreshed.
        exit /b 1
    )
)
"%CMAKE_CMD%" --version >nul 2>&1
if errorlevel 1 (
    echo [error] CMake command is configured but failed to run.
    echo         Restart your shell and try again.
    exit /b 1
)

if /I "%~1"=="--clean" (
    if exist build rmdir /s /q build
    shift
)

set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"

if not exist build (
    "%CMAKE_CMD%" -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 || exit /b 1
)

REM Kill any lingering crux processes so MSBuild can overwrite the .exe files.
REM A stuck download from a previous run holds crux.exe locked and causes
REM LNK1104 "cannot open file". Silence errors when no process is running.
taskkill /F /IM crux.exe        >nul 2>&1
taskkill /F /IM crux_server.exe >nul 2>&1
taskkill /F /IM crux_tests.exe  >nul 2>&1
taskkill /F /IM yt-dlp.exe      >nul 2>&1
taskkill /F /IM ffmpeg.exe      >nul 2>&1

"%CMAKE_CMD%" --build build --config %CFG% --parallel
exit /b %errorlevel%
