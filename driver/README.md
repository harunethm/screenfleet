# Virtual monitor driver

`SstDisplay.dll`: a Windows Indirect Display (IddCx, UMDF 2.25) user-mode
driver that adds virtual monitors on request. Based on Microsoft's IddCx sample
(MIT). Self-signed with a local certificate; no test mode needed.

## How it works

- Installs one virtual display adapter (`Root\SstDisplay`, shown in Device
  Manager as "ScreenFleet Virtual Display"). It has **no monitors** until
  `sst-host.exe` asks for one.
- When a tablet connects, the host sends `ADD_MONITOR` with the tablet's exact
  resolution and refresh rate. Windows sees a new monitor named
  **"ScreenFleet"** and extends the desktop onto it. On disconnect the host
  sends `REMOVE_MONITOR`.
- The host sends a keepalive every second. If it stops for 5 s (host crashed
  or was killed), the driver removes its monitors by itself.
- Each monitor gets a generated EDID whose serial number is derived from the
  tablet (model + resolution), so Windows remembers where you placed it.
- The driver never touches the pixels: it hands every frame straight back to
  Windows. The host captures the monitor with Desktop Duplication.
- Control interface: `include/sst_display_control.h` (device interface GUID
  and IOCTLs). Only SYSTEM, Administrators and the signed-in user can open it.

## Build, install

Building needs the VS 2022 C++ Build Tools and the WDK 10.0.26100.

```
build.cmd                 compile + stage build\package (unsigned, stampinf)
powershell .\install.ps1  sign on this PC + install (asks for admin)
```

`install.ps1` also updates an existing installation. Installing needs no WDK,
only Windows 10 2004 or later; the Windows installer runs the same script.

### Signing: per PC, no key kept

The package is shipped unsigned. `install.ps1` makes a new certificate on the
PC it runs on, signs the package with it and deletes the private key right
away, so no key that this PC trusts exists anywhere (not even with us). Each
install does this again and removes the certificates of earlier installs.

### What gets changed on the PC

| Step | Change | Undo |
|---|---|---|
| `install.ps1` | Adds "CN=ScreenFleet Driver (signed on this PC)" (public part only) to Local Machine *Trusted Root* and *Trusted Publishers* | `uninstall.ps1` |
| `install.ps1` | Adds the driver to the driver store and creates the `Root\SstDisplay` device | `uninstall.ps1` |

## Debug output

The driver logs with `OutputDebugString` (prefix `[SstDisplay]`); view it with
Sysinternals DebugView (capture Global Win32).
