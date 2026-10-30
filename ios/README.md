# iOS / iPadOS app

Status: **not started.** Contributions are welcome. If you'd like to take it
on, feel free to open an issue to talk it through, or send a pull request
straight away.

The idea: an iPad or iPhone as one more screen in the fleet, doing what the
Android app ([`android/`](../android/README.md)) does.

- Receive the stream from the PC using the wire protocol in
  [`protocol/PROTOCOL.md`](../protocol/PROTOCOL.md): discovery, pairing with
  a 6-digit code and encrypted sessions over Wi-Fi.
- Decode H.264 in hardware with VideoToolbox and show it full-screen.
- Send touches and Apple Pencil input (hover, pressure) back to the PC.
- Keep the screen on while streaming.

Wi-Fi is the natural first step; USB would need a different route than adb.
The Android app's source map (`StreamServer`, `VideoDecoder`, `Protocol`,
`TouchInput`) is a good guide to what's involved.
