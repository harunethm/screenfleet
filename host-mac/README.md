# macOS host

Status: **not started.** Contributions are welcome. If you'd like to take it
on, feel free to open an issue to talk it through, or send a pull request
straight away.

The idea: the same job as the Windows host, so the Android app works with it
unchanged.

- Virtual monitor: `CGVirtualDisplay`, one per tablet, at the tablet's size.
- Capture: ScreenCaptureKit (`SCStream`) on that display.
- Encode: VideoToolbox, hardware H.264, low-latency mode, no B-frames.
- Transport: the wire protocol in [`protocol/PROTOCOL.md`](../protocol/PROTOCOL.md):
  USB through `adb forward`, Wi-Fi with pairing and encrypted sessions.
- Touch and pen from the tablet → pointer events on that display.

The Windows host ([`host-windows/`](../host-windows/README.md)) is the
reference: same handshake, same messages, same pairing. A first step that is
already useful is mirror-only over USB, without the virtual monitor.
