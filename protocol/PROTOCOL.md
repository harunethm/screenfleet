# ScreenFleet protocol (v1)

One TCP connection between the **host** (PC) and the **device** (tablet/phone).
The device always listens; the host always connects:

- **USB** (any Android version): the host runs `adb forward tcp:27183 tcp:27183`
  and connects to `127.0.0.1:27183`.
- **Wi-Fi** (Android 5+): the host connects to `<device-ip>:27183`.

Default port: **27183**.

## Framing

All integers are **big-endian** (matches Java's `DataInputStream`).

```
+--------+-------------+---------------------+
| type   | length      | payload             |
| u8     | u32         | `length` bytes      |
+--------+-------------+---------------------+
```

Maximum payload: 8 MiB. Receivers ignore trailing payload bytes they don't
understand, so fields can be appended in later versions.

Strings are `u16 byteLength` followed by UTF-8 bytes.

## Session flow

```
host                                  device
 |  -------- Hello -------------------> |
 |  <------- DeviceInfo --------------- |
 |  -------- StreamStart -------------> |
 |  -------- VideoFrame (key) --------> |
 |  -------- VideoFrame ... ----------> |
 |  <------- RequestKeyFrame (any time) |
 |  <------> Ping / Pong (either side)  |
 |  <------> Bye (either side)          |
 |  <------- DeviceStats (every ~1 s)   |
 |  <------- InputEvent (touch / pen)   |
```

## Messages

| Type | Name | Direction | Payload |
|---:|---|---|---|
| 1 | Hello | host → device | `u32 magic = 0x53535442 ("SSTB")`, `u16 version`, `string hostName` |
| 2 | DeviceInfo | device → host | `u16 version`, `u16 width`, `u16 height`, `u16 densityDpi`, `u16 sdkInt`, `u16 maxFps`, `u32 flags`, `string model`, then (appended) `u32 connectRequestAge`, `string deviceId` |
| 3 | StreamStart | host → device | `u8 codec (1 = H.264)`, `u16 width`, `u16 height`, `u16 fps`, `u32 bitrate (bit/s)`, `u8 flags` (bit 0 = the tablet is a monitor of its own; always set) |
| 4 | VideoFrame | host → device | `u8 flags`, `u64 ptsUs`, `bytes data` (rest of payload) |
| 5 | RequestKeyFrame | device → host | empty |
| 6 | Ping | either | `u64 timeUs` (sender's clock) |
| 7 | Pong | either | the Ping payload echoed back unchanged |
| 8 | Bye | either | empty |
| 9 | DeviceStats | device → host | `u32 decodeUs` (average decode time in the last second, 0 = unknown), `u16 framesShown` |
| 19 | InputEvent | device → host | `u8 flags`, `u8 count`, then `count` pointers (see below) |

### DeviceInfo

- `width` × `height`: the resolution the device wants to display, in pixels, in
  the orientation it will be used. The host streams exactly this size.
- `flags`: bit 0 = touch input supported, bit 1 = pen input supported, bit 2 =
  the decoder holds frames until newer ones arrive (some Android 4.x
  decoders, whatever the stream says): after each change the host sends a
  few repeats of the unchanged picture a few ms apart to push it out.
- `connectRequestAge`: seconds since the user tapped Connect on the device
  (opening the app counts), `0xFFFFFFFF` = never. A host that was told to
  disconnect from this device answers DeviceInfo with Bye (instead of
  StreamStart) until the device asks again after that moment. Clocks don't
  need to agree: the host compares the age with its own clock.
- `deviceId`: a random id made once per install, so two tablets of the same
  model are told apart. Missing from older devices (the host then uses the
  model name).
- The host may send **Bye instead of StreamStart**; the device then simply
  waits for the next connection.

### VideoFrame

- `data` is H.264 **Annex-B** (start-code delimited NAL units). Key frames
  carry SPS/PPS in-band, so a decoder can start from any key frame.
- `flags`: bit 0 = key frame, bit 1 = codec config only (SPS/PPS, no picture).
- There are no B-frames; frames arrive in display order and should be
  rendered as soon as they are decoded.
- When the screen doesn't change, no frames are sent. The device keeps
  showing the last picture.

### InputEvent

Touch and pen on the picture, sent only while streaming (after StreamStart).
Each message is a **snapshot of every pointer the device tracks right now**:
fingers that touch the screen and a pen that touches or hovers. A pointer
missing from the snapshot has been lifted (or the pen left the screen). The
host works out down / move / up from the difference with the previous
snapshot, so a lost or merged message can't leave a finger stuck down.

- `flags`: bit 0 = canceled (the gesture was taken away, e.g. by the system):
  the host cancels the pointers of the previous snapshot instead of lifting
  them, so they don't click. A canceled snapshot has `count` 0.
- `count`: 0 to 10 (10 pointers at most).
- Each pointer, 9 bytes: `u8 id` (0..31, stable while the pointer is down),
  `u8 tool` (1 = finger, 2 = pen), `u8 state` (bit 0 = touching the screen,
  bit 1 = pen barrel button, bit 2 = pen eraser end), `u16 x`, `u16 y`
  (0..65535 across the picture: 0 = left / top edge, 65535 = right / bottom
  edge), `u16 pressure` (pen: 0..1024; fingers: 0).
- A finger is always touching; only a pen can hover. At most one pen.
- Anything else (more than 10 pointers, an unknown tool, a repeated id, a
  short payload) makes the host drop that message; bytes after the last
  pointer are ignored.
- Over Wi-Fi the message is Sealed like everything else, so only the paired
  tablet can move the pointer. The host may ignore it (the user turned
  touch off for this tablet); the device isn't told.

## Discovery (UDP, Wi-Fi, Android 5+)

The host broadcasts a query to `255.255.255.255:27183` (UDP) about every two
seconds; each device answers the sender directly. No listening port is
needed on the PC (Windows lets unicast replies to a broadcast through).

- **Query**: exactly 256 bytes: `u32 magic = 0x53535451 ("SSTQ")`, `u16 version`,
  zero padding. Devices ignore any other size, so a reply (well under 256
  bytes) is never larger than the query.
- **Reply**: `u32 magic = 0x53535452 ("SSTR")`, `u16 version`, `u16 tcpPort`,
  `u8 flags` (bit 0 = busy streaming), `u16 width`, `u16 height`, `u16 sdkInt`,
  `u32 connectRequestAge`, `string model`, `string deviceId`, `string busyWith`
  (the host name it streams to). Strings are at most 64 bytes.
- Devices answer at most 20 queries per second.

Discovery only lists devices; it never makes the host connect. Over Wi-Fi
the host streams only to devices it is paired with (next section).

## Pairing and encrypted sessions (Wi-Fi)

USB needs neither: the cable and adb's own authorization keep it private,
and the device takes plaintext only from 127.0.0.1 (the adb forward). Over
Wi-Fi the device accepts only encrypted sessions from paired hosts, and the
host streams only to paired devices. (Hosts treat loopback addresses like
USB, for the test tools.)

### Building blocks

- ECDH on P-256; public keys are 65 bytes uncompressed (`04 || X || Y`), the
  shared secret `Z` is the 32-byte X coordinate.
- SHA-256, HMAC-SHA256, HKDF-SHA256 (RFC 5869), AES-256-GCM (16-byte tag).
- `bytesN` is N raw bytes. In transcripts, `||` concatenates, and strings
  are written as on the wire (`u16 length` + UTF-8).
- Ids are random, made once: `hostId` per PC, `deviceId` per tablet install.

### Pairing (once per host and device)

The user starts it on the host; the device must be showing its waiting
screen. The device asks "Pair with <hostName>?" and shows a 6-digit code;
the user types that code on the host and taps Pair on the device.

| Type | Name | Direction | Payload |
|---:|---|---|---|
| 10 | PairStart | host → device | `string hostId`, `string hostName`, `bytes32 commit = SHA256(pkH \|\| nH)` |
| 11 | PairReply | device → host | `string deviceId`, `string model`, `bytes65 pkD`, `bytes32 nD` |
| 12 | PairReveal | host → device | `bytes65 pkH`, `bytes32 nH` |
| 13 | PairConfirm | host → device | `bytes32 HMAC(K, "host" \|\| T)` |
| 14 | PairDone | device → host | `bytes32 HMAC(K, "device" \|\| T)` |

`pkH`, `pkD` are fresh ECDH keys and `nH`, `nD` 32 random bytes. The device
checks `commit` against PairReveal. Then both compute

```
T    = SHA256("SST pair v1" || hostId || deviceId || pkH || pkD || nH || nD)
code = (first 4 bytes of SHA256("SST code" || T), big-endian) mod 1000000
K    = HKDF(salt = T, ikm = Z, info = "SST pairing key", 32 bytes)
```

The host commits to its key before it sees the device's, so a machine in
the middle can't search for keys that make both sides show the same code;
each attempt succeeds with probability 1 in a million. The host sends
PairConfirm only when the typed code matches; the device sends PairDone only
after its user tapped Pair and PairConfirm checked out. Both then keep `K`
for the other side's id, and the connection ends with Bye. Either side may
send Bye at any point to cancel; the device gives up after 120 s.

### Encrypted session

The host appends to Hello: `u8 flags` (bit 0 = secure), `string hostId`,
`bytes65 epkH`, `bytes32 nH` (fresh ECDH key and nonce per connection).

| Type | Name | Direction | Payload |
|---:|---|---|---|
| 15 | NotPaired | device → host | `string deviceId`, `string model` (then the device closes) |
| 16 | SessionReply | device → host | `string deviceId`, `bytes65 epkD`, `bytes32 nD`, `bytes32 macD` |
| 17 | SessionConfirm | host → device | `bytes32 macH` |
| 18 | Sealed | either | AES-256-GCM ciphertext + tag of `u8 type \|\| payload` |

```
T          = SHA256("SST session v1" || hostId || deviceId || epkH || epkD || nH || nD)
keys       = HKDF(salt = T, ikm = K || Z, info = "SST session keys", 96 bytes)
kHostToDev = keys[0..32], kDevToHost = keys[32..64], kConfirm = keys[64..96]
macD       = HMAC(kConfirm, "device" || T),  macH = HMAC(kConfirm, "host" || T)
```

`Z` comes from the per-connection keys, so a recorded session stays secret
even if `K` leaks later. After SessionConfirm every message in both
directions is Sealed (from DeviceInfo on). The GCM nonce is never sent: it
is `u32 direction` (1 host → device, 2 device → host) followed by a `u64`
message counter starting at 0 in each direction. Anything that fails to
check out ends the connection.
