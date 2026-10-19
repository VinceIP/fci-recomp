@echo off
rem Build the map editor for the game BuildGame.cmd set up. The game's Map Editor menu then starts it.
setlocal
rem started from PowerShell 7, its module path would hide Windows PowerShell's own commands
set "PSModulePath="
rem what must be here before anything runs: the files beside this one, PowerShell, 64-bit Windows
if not exist "%~dp0tools\bootstrap.ps1" goto not_extracted
where powershell >nul 2>nul || goto no_powershell
if /i "%PROCESSOR_ARCHITECTURE%"=="x86" if not defined PROCESSOR_ARCHITEW6432 goto not_64bit
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\bootstrap.ps1" editor
if errorlevel 1 (
    echo.
    echo Build error. Check dependencies, and run BuildEditor.cmd again to resume.
) else (
    echo.
    echo Build complete. Choose Map Editor in the game's main menu.
)
goto end

:not_extracted
echo error: the files that come with BuildEditor.cmd are not beside it. Extract the whole download to a
echo folder first, such as C:\fci-recomp, then run BuildGame.cmd from there.
goto end
:no_powershell
echo error: Windows PowerShell was not found. It may be disabled or
echo blocked on this PC.
goto end
:not_64bit
echo error: 64-bit Windows 10 or 11 is required.
:end
pause
