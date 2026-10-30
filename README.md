# ScreenFleet

Use an Android tablet or phone (Android 4.4 and newer) as a second monitor for
Windows, and later macOS. Fully open source, no third-party apps.

| Folder | What it is | Status |
|---|---|---|
| [`protocol/`](protocol/PROTOCOL.md) | Wire protocol shared by host and app | v1 |
| [`host-windows/`](host-windows/README.md) | Windows host (C++): capture → GPU encode → stream | working (video, extend + mirror) |
| [`driver/`](driver/README.md) | Virtual monitor driver (C++, IddCx), self-signed | working |
| [`android/`](android/README.md) | Android app (Java, minSdk 19) | working (display) |
| [`host-mac/`](host-mac/README.md) | macOS host | not started, contributions welcome |
| [`ios/`](ios/README.md) | iOS / iPadOS app | not started, contributions welcome |
| `installer/` | Windows installer (Inno Setup): app + driver | 1.0.0 |

## Install (Windows)

`installer\build.cmd` builds the app, the signed driver and
`installer\build\ScreenFleet-<version>-setup.exe`. The installer puts the app
in Program Files, installs the virtual display driver (one admin prompt) and
adds Start menu and desktop shortcuts; uninstalling removes the driver and its
certificate again. adb (for USB) is not included: install Android
platform-tools separately.

`installer\package.cmd` makes the public release on top of that:
`installer\dist\ScreenFleet-<version>\` (setup exe, signed release APK,
readme, licenses, SHA-256 checksums) and the same as a zip. The Android
release key stays outside the repo (see `android/app/build.gradle`).

## How it works

```
Windows PC                                              Tablet
┌──────────────────────────────────────────┐           ┌──────────────────────┐
│ virtual monitor → Desktop Duplication    │  USB/adb  │ MediaCodec (hardware │
│   → GPU scale + NV12 → hardware H.264    │ ───────▶  │  H.264 decoder)      │
│   (NVENC / AMD / Intel via Media Found.) │  or Wi-Fi │   → full-screen view │
│                       ◀── touch / pen ───│ ◀──────── │                      │
└──────────────────────────────────────────┘           └──────────────────────┘
```

- USB for Android 4.4 (`adb forward`), Wi-Fi or USB for Android 5+.
- The picture stays on the GPU from capture to encoder; nothing is copied
  through the CPU.
- No B-frames, low-latency encoder mode, frames sent only when the screen
  changes.
