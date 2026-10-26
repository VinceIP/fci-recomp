@echo off
rem Drag the same disc image (or the folder with the disc's files) you gave BuildGame.cmd onto this file. It
rem builds that game's map editor beside the game; the game's Map Editor menu then starts it.
setlocal
rem started from PowerShell 7, its module path would hide Windows PowerShell's own commands
set "PSModulePath="
rem what must be here before anything runs: the files beside this one, PowerShell, 64-bit Windows
if not exist "%~dp0tools\bootstrap.ps1" goto not_extracted
where powershell >nul 2>nul || goto no_powershell
if /i "%PROCESSOR_ARCHITECTURE%"=="x86" if not defined PROCESSOR_ARCHITEW6432 goto not_64bit
set "DISC=%~1"
if "%DISC%"=="" (
    echo Drag your Far Cry Instincts or Evolution disc image, or the folder with the disc's files, onto BuildEditor.cmd.
    echo Or drag it into this window now and press Enter:
    set /p "DISC="
)
if "%DISC%"=="" exit /b 1
set "DISC=%DISC:"=%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\bootstrap.ps1" editor "%DISC%"
if errorlevel 1 (
    echo.
    echo The build stopped. The reason and what to do are above.
    echo Run BuildEditor.cmd again to resume where it stopped.
) else (
    echo.
    echo Build complete. Choose Map Editor in the game's main menu.
)
goto end
:not_extracted
echo error: the files that come with BuildEditor.cmd are not beside it. Extract the whole download to a
echo folder first, such as C:\fci-recomp, then run BuildEditor.cmd from there.
goto end
:no_powershell
echo error: Windows PowerShell was not found. It may be disabled or
echo blocked on this PC.
goto end
:not_64bit
echo error: 64-bit Windows 10 or 11 is required.
:end
pause
