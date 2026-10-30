@echo off
rem Builds the app and the driver package, then the installer:
rem build\ScreenFleet-<version>-setup.exe. Needs Inno Setup 6 (winget install JRSoftware.InnoSetup).
setlocal

set ISCC=
for /f "delims=" %%i in ('where ISCC.exe 2^>nul') do if not defined ISCC set "ISCC=%%i"
if not defined ISCC if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC (
    echo Inno Setup 6 not found.
    exit /b 1
)

call "%~dp0..\host-windows\build.cmd" Release || exit /b 1
call "%~dp0..\driver\build.cmd" || exit /b 1

"%ISCC%" /Q "%~dp0ScreenFleet.iss" || exit /b 1
echo.
echo Installer in %~dp0build
