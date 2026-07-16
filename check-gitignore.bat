@echo off
REM Sanity-check that .gitignore catches the things it should.
REM Run this from the repo root.  Requires git on PATH.

setlocal EnableExtensions
cd /d "%~dp0"

set "SHOULD_IGNORE=build\CMakeCache.txt build\Release\crux.exe build\Release\crux_server.exe build\Release\crux_tests.exe build\_deps\httplib-src\README.md build\ytshorts_core.lib third_party\bin\yt-dlp.exe third_party\bin\ffmpeg.exe third_party\yt-dlp.exe third_party\ffmpeg.exe out\iIY9fPgY5wM\clip_01_000000.mp4 out\web-20260716-162846-855\run.log out\web-20260716-162846-855\manifest.json verify.exe crux.exe .vs\crux.sln .vscode\settings.json .idea\workspace.xml cookies.txt Thumbs.db .DS_Store node_modules\foo.js __pycache__\bar.pyc some.log foo.tmp scratch\notes.md .env vcpkg_installed\x64-windows"

set "SHOULD_TRACK=src\main.cpp src\core\detector.cpp README.md LICENSE PLAN.md CMakeLists.txt vcpkg.json dashboard\logo.svg dashboard\index.html tests\fixtures\heatmap_scores.json setup.bat run.bat rebuild.bat dashboard.bat third_party\.gitkeep"

set /a FAIL=0

echo === paths that MUST be ignored ===
for %%f in (%SHOULD_IGNORE%) do (
    git check-ignore -q "%%f"
    if errorlevel 1 (
        echo   MISS  %%f
        set /a FAIL+=1
    ) else (
        echo   ok    %%f
    )
)

echo.
echo === paths that MUST be tracked ===
for %%f in (%SHOULD_TRACK%) do (
    git check-ignore -q "%%f"
    if errorlevel 1 (
        echo   ok    %%f
    ) else (
        echo   MISS  %%f  ^(would be ignored^)
        set /a FAIL+=1
    )
)

echo.
if %FAIL%==0 (
    echo === .gitignore is correct ===
    exit /b 0
) else (
    echo === %FAIL% path^(s^) misclassified ===
    exit /b 1
)
