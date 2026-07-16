@echo off
REM =====================================================================
REM ytshorts — first-run setup for Windows
REM   1) checks prerequisites (cmake, MSVC)
REM   2) downloads yt-dlp.exe + ffmpeg.exe into third_party\bin\
REM   3) configures and builds with CMake + Visual Studio
REM   4) runs a --version smoke test
REM
REM Run in cmd.exe OR PowerShell:
REM   .\setup.bat
REM =====================================================================

setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

echo.
echo === ytshorts setup ===
echo Working directory: %CD%
echo.

REM ---------------------------------------------------------------------
REM Prerequisites
REM ---------------------------------------------------------------------
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
echo [ok]  cmake

where git >nul 2>&1
if errorlevel 1 (
    echo [warn] git not found. CMake FetchContent will not be able to pull
    echo        CLI11 / nlohmann-json / spdlog / doctest.  Install Git for
    echo        Windows from https://git-scm.com/download/win, or set
    echo        VCPKG_ROOT to a vcpkg install that provides those packages.
) else (
    echo [ok]  git
)

REM ---------------------------------------------------------------------
REM third_party\bin
REM ---------------------------------------------------------------------
if not exist "third_party\bin" mkdir "third_party\bin"

REM ---- yt-dlp ---------------------------------------------------------
if exist "third_party\bin\yt-dlp.exe" (
    echo [ok]  third_party\bin\yt-dlp.exe already present
) else (
    echo [..]  downloading yt-dlp.exe ...
    call :download ^
        "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe" ^
        "third_party\bin\yt-dlp.exe"
    if errorlevel 1 (
        echo [error] yt-dlp download failed.
        echo         Manually download yt-dlp.exe from
        echo         https://github.com/yt-dlp/yt-dlp/releases/latest
        echo         and place it at third_party\bin\yt-dlp.exe.
        exit /b 1
    )
    echo [ok]  yt-dlp.exe
)

REM ---- ffmpeg ---------------------------------------------------------
if exist "third_party\bin\ffmpeg.exe" (
    echo [ok]  third_party\bin\ffmpeg.exe already present
) else (
    echo [..]  downloading ffmpeg release-essentials zip ...
    call :download ^
        "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip" ^
        "third_party\bin\ffmpeg.zip"
    if errorlevel 1 (
        echo [error] ffmpeg download failed.
        echo         Manually download a Windows build from
        echo         https://www.gyan.dev/ffmpeg/builds/ and copy
        echo         ffmpeg.exe + ffprobe.exe to third_party\bin\.
        exit /b 1
    )
    echo [..]  extracting ffmpeg ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "Expand-Archive -Path 'third_party\bin\ffmpeg.zip' -DestinationPath 'third_party\bin\_ffmpeg' -Force" ^
        || (echo [error] extraction failed & exit /b 1)
    for /d %%D in ("third_party\bin\_ffmpeg\ffmpeg-*") do (
        copy /y "%%D\bin\ffmpeg.exe"  "third_party\bin\"  >nul
        copy /y "%%D\bin\ffprobe.exe" "third_party\bin\"  >nul
    )
    rmdir /s /q "third_party\bin\_ffmpeg"
    del /q "third_party\bin\ffmpeg.zip"
    if not exist "third_party\bin\ffmpeg.exe" (
        echo [error] extracted ffmpeg.exe not found. Zip layout may have changed.
        exit /b 1
    )
    echo [ok]  ffmpeg.exe, ffprobe.exe
)

REM ---------------------------------------------------------------------
REM CMake configure
REM ---------------------------------------------------------------------
echo.
echo === configure ===
if defined VCPKG_ROOT (
    echo [ok]  VCPKG_ROOT is set: %VCPKG_ROOT%  (will use vcpkg toolchain)
)

set "GEN_ARGS=-G ""Visual Studio 17 2022"" -A x64"
where cl >nul 2>&1
if errorlevel 1 (
    REM Not inside a VS Developer prompt. That is OK — CMake will invoke
    REM MSBuild via the VS generator. Detect whether VS 2022 is installed.
    if not exist "%ProgramFiles%\Microsoft Visual Studio\2022" (
        if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022" (
            echo [warn] Visual Studio 2022 not found in the usual paths.
            echo        Falling back to the default CMake generator.
            set "GEN_ARGS="
        )
    )
)

"%CMAKE_CMD%" -S . -B build %GEN_ARGS% -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 goto :configure_failed

REM ---------------------------------------------------------------------
REM Build
REM ---------------------------------------------------------------------
echo.
echo === build (Release) ===
"%CMAKE_CMD%" --build build --config Release --parallel
if errorlevel 1 (
    echo [error] Build failed.  See messages above.
    exit /b 1
)

REM ---------------------------------------------------------------------
REM Locate the built executable
REM ---------------------------------------------------------------------
set "YTSHORTS_EXE="
if exist "build\Release\ytshorts.exe" set "YTSHORTS_EXE=build\Release\ytshorts.exe"
if not defined YTSHORTS_EXE if exist "build\ytshorts.exe" set "YTSHORTS_EXE=build\ytshorts.exe"
if not defined YTSHORTS_EXE (
    echo [error] Build succeeded but ytshorts.exe was not found.
    exit /b 1
)

echo.
echo === smoke test ===
"%YTSHORTS_EXE%" --version
if errorlevel 1 (
    echo [warn] --version returned non-zero.
)

echo.
echo === run tests ===
if exist "build\Release\ytshorts_tests.exe" (
    "build\Release\ytshorts_tests.exe"
) else if exist "build\ytshorts_tests.exe" (
    "build\ytshorts_tests.exe"
) else (
    echo [warn] ytshorts_tests.exe not found — skipping.
)

echo.
echo =====================================================================
echo Setup complete.  Try:
echo    run.bat "https://www.youtube.com/watch?v=iIY9fPgY5wM" --dry-run --dump-heatmap
echo =====================================================================
exit /b 0


:configure_failed
echo.
echo [error] CMake configure failed.  Common causes:
echo         - no C++ compiler installed. Install the "Desktop development
echo           with C++" workload from the Visual Studio Installer.
echo         - proxy / firewall blocking github.com for FetchContent.
echo         - stale build\ directory from a previous configure.  Try:
echo               rmdir /s /q build ^&^& setup.bat
exit /b 1


REM =====================================================================
REM :download URL OUTFILE
REM Uses curl.exe (built into Windows 10 1803+) with a PowerShell fallback.
REM =====================================================================
:download
set "URL=%~1"
set "OUT=%~2"
where curl.exe >nul 2>&1
if not errorlevel 1 (
    curl.exe -L --fail --retry 3 --retry-delay 2 -o "%OUT%" "%URL%"
    exit /b %errorlevel%
)
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ProgressPreference='SilentlyContinue'; try { Invoke-WebRequest -Uri '%URL%' -OutFile '%OUT%' -UseBasicParsing } catch { exit 1 }"
exit /b %errorlevel%
