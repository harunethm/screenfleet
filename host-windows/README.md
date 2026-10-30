# Windows host

`sst-host.exe` captures one display and streams it to the Android app.

## Build

Needs Visual Studio 2022 Build Tools (C++ workload, Windows SDK).

```
build.cmd            (Release)
build.cmd Debug
```

Output: `build\ScreenFleet.exe` (the app), `build\sst-host.exe` and
`build\sst-fake-device.exe`. The C++ runtime is linked statically, so the
.exe runs on any Windows 10/11 PC as is.

## Run

```
sst-host                              extend: the tablet becomes a new monitor (USB)
sst-host --list                       list displays
sst-host --display 0                  mirror display 0 instead
sst-host --connect 192.168.1.50       extend over Wi-Fi (Android 5+)
```

Options: `--fps N` (default 60), `--bitrate KBPS` (default 8000),
`--adb PATH`. The stream size always matches the device's screen.

## Test without a tablet

```
sst-fake-device --out received.h264 --key-every 3
sst-host --display 0 --connect 127.0.0.1
```

`sst-fake-device` behaves like the app: it answers the handshake, saves the
video, sends pings and asks for key frames, and prints fps / bitrate / RTT.

Or encode straight to a file: `sst-host --dump out.h264 --seconds 5 --constant`.

## Source map

| File | Job |
|---|---|
| `src/outputs.cpp` | list monitors and the GPU driving each one |
| `src/capture.cpp` | DXGI Desktop Duplication (GPU texture, auto-recovers) |
| `src/converter.cpp` | GPU video processor: scale + BGRA → NV12, BT.709 |
| `src/encoder.cpp` | hardware H.264 via Media Foundation (async MFT) |
| `src/session.cpp` | frame pacing loop, file/network sinks (incl. touches from the tablet) |
| `src/touch_input.cpp` | touches and pen from a tablet -> synthetic touch / pen devices on its monitor |
| `src/net.cpp` | TCP framing (see `protocol/PROTOCOL.md`) |
| `src/adb.cpp` | `adb forward` for USB connections |
| `src/virtual_display.cpp` | talks to the display driver: add/remove monitors, keepalive |
| `src/main.cpp` | options, device handshake, reconnect loop |
| `tools/fake_device.cpp` | test stand-in for the Android app |

## Not done yet

Mouse pointer in Mirror mode (Extend shows it: Windows draws it into the
virtual monitor), display rotation, software-encoder fallback for PCs
without a hardware H.264 encoder.

Touch and pen need Windows 10 1809 or newer (synthetic pointer devices);
`sst-host --touch-log` logs them instead of acting on them, for tests.
