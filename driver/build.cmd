@echo off
rem Builds SstDisplay.dll and stages the driver package in build\package.
rem Unsigned: install.ps1 signs it on the PC it installs on.
setlocal
set KITS=%ProgramFiles(x86)%\Windows Kits\10
set KITSVER=10.0.26100.0

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
if "%VSPATH%"=="" (
    echo Visual Studio C++ build tools not found.
    exit /b 1
)
rem vcvars64.bat calls vswhere.exe by name.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%~dp0build" || exit /b 1

set PKG=%~dp0build\package
if exist "%PKG%" rmdir /s /q "%PKG%"
mkdir "%PKG%"
copy /y "%~dp0build\SstDisplay.dll" "%PKG%\" >nul || exit /b 1
copy /y "%~dp0SstDisplay.inf" "%PKG%\" >nul || exit /b 1
rem Fills in DriverVer (date + version from the clock), $ARCH$ and $UMDFVERSION$.
"%KITS%\bin\%KITSVER%\x64\stampinf.exe" -f "%PKG%\SstDisplay.inf" -a amd64 -d * -v * -u 2.25.0 -k 1.0 >nul || exit /b 1
echo.
echo Driver package staged in %PKG% (unsigned; install.ps1 signs and installs it)
