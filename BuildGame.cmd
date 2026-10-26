@echo off
rem Drag your Far Cry Instincts or Far Cry Instincts Evolution disc image (or the folder with the disc's
rem files) onto this file. The game is checked, recompiled and built on this PC, then installed in a folder
rem named after it here (FarCryInstincts or FarCryEvolution).
setlocal
rem started from PowerShell 7, its module path would hide Windows PowerShell's own commands
set "PSModulePath="
rem what must be here before anything runs: the files beside this one, PowerShell, 64-bit Windows
if not exist "%~dp0tools\bootstrap.ps1" goto not_extracted
where powershell >nul 2>nul || goto no_powershell
if /i "%PROCESSOR_ARCHITECTURE%"=="x86" if not defined PROCESSOR_ARCHITEW6432 goto not_64bit
set "DISC=%~1"
if "%DISC%"=="" (
    echo Drag your Far Cry Instincts or Evolution disc image, or the folder with the disc's files, onto BuildGame.cmd.
    echo Or drag it into this window now and press Enter:
    set /p "DISC="
)
if "%DISC%"=="" exit /b 1
set "DISC=%DISC:"=%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\bootstrap.ps1" setup "%DISC%"
if errorlevel 1 (
    echo.
    echo The build stopped. The reason and what to do are above.
    echo Run BuildGame.cmd again to resume where it stopped.
) else (
    echo.
    echo Build complete. The game is in the folder named after it, beside BuildGame.cmd.
)
goto end

:not_extracted
echo error: the files that come with BuildGame.cmd are not beside it. Extract the whole download to a
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
