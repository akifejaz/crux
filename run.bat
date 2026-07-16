@echo off
REM Convenience wrapper: forwards all arguments to the built crux.exe.
REM Usage:
REM   run.bat <youtube-url> [flags]
REM   run.bat "https://www.youtube.com/watch?v=iIY9fPgY5wM" --dry-run --dump-heatmap
REM   run.bat "iIY9fPgY5wM" --max-clips 3 --format 916blur

setlocal EnableExtensions
cd /d "%~dp0"

set "EXE=build\Release\crux.exe"
if not exist "%EXE%" set "EXE=build\crux.exe"
if not exist "%EXE%" (
    echo [error] crux.exe not built yet.  Run setup.bat first.
    exit /b 1
)

"%EXE%" %*
exit /b %errorlevel%
