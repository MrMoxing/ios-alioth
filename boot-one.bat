@echo off
rem One-click HackiOS boot on Redmi K40 (alioth) -- Windows entry point.
rem All logic lives in boot-one.ps1 next to this file (it needs .NET for the framebuffer -> PNG);
rem this wrapper just starts it with ExecutionPolicy Bypass so double-click works.
rem
rem   boot-one.bat
rem   boot-one.bat -Secs 60 -Shots 3 -Gap 5
rem   boot-one.bat -NoSystem
setlocal
set "HERE=%~dp0"
if not exist "%HERE%boot-one.ps1" (
    echo [boot-one] boot-one.ps1 is missing next to this .bat
    if defined NO_PAUSE exit /b 1
    pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%boot-one.ps1" %*
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo.
    echo [boot-one] exit code %RC%
    if defined NO_PAUSE exit /b %RC%
    pause
)
exit /b %RC%
