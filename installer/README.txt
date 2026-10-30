ScreenFleet {VERSION}
=================

Use an Android tablet or phone (Android 4.4 or newer) as a second monitor
for a Windows 10/11 PC, over USB or Wi-Fi.

Files
-----
  ScreenFleet-{VERSION}-setup.exe   Windows app + virtual display driver
  ScreenFleet-{VERSION}.apk         Android app
  SHA256SUMS.txt                checksums of the two files above
  LICENSE.txt                   MIT license
  OFL.txt                       license of the Geist font used by the apps

1. On the PC
------------
Run ScreenFleet-{VERSION}-setup.exe (Windows 10 2004 or newer, 64-bit).
It installs the app to Program Files and the virtual display driver, which
lets Windows extend the desktop onto the tablet. Windows asks once for
administrator rights. Uninstall it from Settings > Apps; that also removes
the driver.

Windows SmartScreen may warn about the installer because it is not signed
with a commercial certificate yet: choose "More info", then "Run anyway".

2. On the tablet
----------------
Copy ScreenFleet-{VERSION}.apk to the tablet and open it, or install it from
the PC with:  adb install ScreenFleet-{VERSION}.apk
Android asks to allow installing apps from this source the first time.

3. Connect
----------
Open ScreenFleet on both.
- Wi-Fi (Android 5+): the PC lists the tablet under Tablets; pair it once
  with the code the tablet shows.
- USB (any version, the only option on Android 4.4): turn on USB debugging
  on the tablet and install Android platform-tools (adb) on the PC:
  https://developer.android.com/tools/releases/platform-tools
