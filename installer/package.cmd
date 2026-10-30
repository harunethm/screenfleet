@echo off
rem Builds everything and stages the public release in
rem dist\ScreenFleet-<version>\ plus dist\ScreenFleet-<version>.zip:
rem setup exe, signed release APK, readme, licenses, SHA-256 checksums.
setlocal
call "%~dp0build.cmd" || exit /b 1
pushd "%~dp0..\android" || exit /b 1
call "%~dp0..\android\gradlew.bat" assembleRelease --console=plain -q || (popd & exit /b 1)
popd
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0package.ps1" || exit /b 1
