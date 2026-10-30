# Android app

Java app (minSdk 19, Android 4.4+, no AndroidX) that shows the PC's display.

- Listens on port 27183. Android 4.x: `127.0.0.1` only (USB via `adb forward`).
  Android 5+: all interfaces, so Wi-Fi works too.
- Decodes with the hardware decoder (MediaCodec) straight onto a SurfaceView;
  frames are shown as soon as they're decoded.
- Full-screen, landscape, screen kept on while open.

## Build and install

Needs JDK 17+ and the Android SDK (`local.properties` → `sdk.dir`).

```
gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Logs: `adb logcat -s SST.Server SST.Decoder` (prints decode fps and decode
latency every 5 seconds).

## Source map

| File | Job |
|---|---|
| `MainActivity.java` | full-screen view, status text, starts/stops the server with the surface |
| `StreamServer.java` | accepts the PC's connection, handshake, message loop |
| `VideoDecoder.java` | MediaCodec H.264 decoder, key-frame recovery, latency stats |
| `Protocol.java` | wire protocol (see `protocol/PROTOCOL.md`) |
| `TouchInput.java` | touches and pen on the picture → InputEvent snapshots for the PC |

## Touch and pen

Fingers and the pen on the picture are sent to the PC (setting "Touch
controls the PC", on by default; turn it off on tablets with a broken touch
screen). The menu handle on the left edge stays the tablet's own. A pen
also sends hover, pressure, the barrel button and the eraser end.
