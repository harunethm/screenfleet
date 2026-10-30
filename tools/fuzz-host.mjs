// Robustness test for the Windows host: plays a misbehaving tablet and runs
// sst-host.exe against it. Each streaming case adds a small virtual monitor
// for a few seconds (needs the display driver).
// A case passes when the host is still running at the end (no crash, no
// hang on exit) and, where it applies, reacted as expected.
// Usage: node tools/fuzz-host.mjs [path\to\sst-host.exe]
import net from 'node:net';
import os from 'node:os';
import { spawn } from 'node:child_process';
import { createECDH, randomBytes } from 'node:crypto';

// Cases marked `wifi` reach the host on this PC's network address, so it
// treats them as Wi-Fi: an encrypted session from a paired tablet is required.
const lanAddress = Object.values(os.networkInterfaces()).flat()
  .find((a) => a && a.family === 'IPv4' && !a.internal)?.address;
const str = (s) => Buffer.concat([u16(Buffer.byteLength(s)), Buffer.from(s)]);

const exe = process.argv[2] ?? 'host-windows\\build\\sst-host.exe';
const PORT = 27190;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const u16 = (v) => Buffer.from([(v >> 8) & 255, v & 255]);
const u32 = (v) => { const b = Buffer.alloc(4); b.writeUInt32BE(v >>> 0); return b; };
const msg = (type, payload = Buffer.alloc(0)) => Buffer.concat([Buffer.from([type]), u32(payload.length), payload]);
const deviceInfo = ({ version = 1, w = 640, h = 400, fps = 30, model = 'Fuzz tablet' } = {}) => {
  const name = Buffer.isBuffer(model) ? model : Buffer.from(model);
  return msg(2, Buffer.concat([u16(version), u16(w), u16(h), u16(160), u16(31), u16(fps), u32(1), u16(name.length), name]));
};

// InputEvent (touch / pen): a snapshot of pointers.
const ptr = (id, tool, state, x, y, p = 0) => Buffer.concat([Buffer.from([id, tool, state]), u16(x), u16(y), u16(p)]);
const input = (pointers, flags = 0, count = pointers.length) =>
  msg(19, Buffer.concat([Buffer.from([flags, count]), ...pointers]));

const ecdhKey = () => { const e = createECDH('prime256v1'); e.generateKeys(); return e.getPublicKey(); };

// Each case gets the socket after the host connected and sent Hello.
const cases = [
  ['no reply at all', { expect: null }, async (s) => { await sleep(4000); }],
  ['garbage reply', { expect: null }, async (s) => { s.write(randomBytes(3000)); await sleep(1500); }],
  ['wrong message type', { expect: /unexpected reply/ }, async (s) => { s.write(msg(5)); await sleep(1500); }],
  ['protocol version 99', { expect: /protocol v99/ }, async (s) => { s.write(deviceInfo({ version: 99 })); await sleep(1500); }],
  ['screen 0 x 0', { expect: /invalid screen size/ }, async (s) => { s.write(deviceInfo({ w: 0, h: 0 })); await sleep(1500); }],
  ['screen 65535 x 65535', { expect: /invalid screen size/ }, async (s) => { s.write(deviceInfo({ w: 65535, h: 65535 })); await sleep(1500); }],
  ['max fps 0', { expect: /streaming started/ }, async (s) => { s.write(deviceInfo({ fps: 0 })); await sleep(3000); }],
  ['name: 60 KiB with newlines and control chars', { expect: /streaming started/ }, async (s) => {
    s.write(deviceInfo({ model: Buffer.from('Evil\r\n[tablet0]\r\nname=x\x00\x07'.repeat(4000)) }));
    await sleep(3000);
  }],
  ['length 0xFFFFFFFF', { expect: /too large/ }, async (s) => { s.write(Buffer.concat([Buffer.from([2]), u32(0xffffffff)])); await sleep(1500); }],
  ['streaming, then key frame request flood', { expect: /streaming started/ }, async (s) => {
    s.write(deviceInfo());
    await sleep(1500);
    for (let i = 0; i < 5000; i++) s.write(msg(5));
    await sleep(2500);
  }],
  ['streaming, then 8 MiB Ping', { expect: /streaming started/ }, async (s) => {
    s.write(deviceInfo());
    await sleep(1500);
    s.write(msg(6, randomBytes(8 * 1024 * 1024)));
    await sleep(3000);
  }],
  ['streaming, then garbage DeviceStats and unknown types', { expect: /streaming started/ }, async (s) => {
    s.write(deviceInfo());
    await sleep(1500);
    for (let t = 9; t < 250; t += 3) s.write(msg(t, randomBytes(32)));
    s.write(msg(9, Buffer.from([1])));
    await sleep(2000);
  }],
  ['streaming, then stops reading (send timeout)', { expect: /connection lost while sending|streaming stopped/, quietMs: 0 },
    async (s) => {
      s.write(deviceInfo({ w: 1280, h: 800, fps: 60 }));
      await sleep(500);
      s.pause(); // stop reading: the host's sends back up
      await sleep(16000);
    }],
  // Touch and pen (the host runs with --touch-log: nothing is clicked on this PC)
  ['input before DeviceInfo', { expect: /unexpected reply/, reject: /input Fuzz/ }, async (s) => {
    s.write(input([ptr(0, 1, 1, 100, 100)]));
    await sleep(1500);
  }],
  ['streaming, then valid touches and pen', { expect: /finger 1 up[\s\S]*pen down[\s\S]*pen away/ }, async (s) => {
    s.write(deviceInfo());
    await sleep(1500);
    s.write(input([ptr(0, 1, 1, 0, 0)]));
    s.write(input([ptr(0, 1, 1, 65535, 65535), ptr(1, 1, 1, 30000, 30000)]));
    s.write(input([]));
    s.write(input([ptr(0, 2, 0, 1000, 1000)]));             // pen hovering
    s.write(input([ptr(0, 2, 7, 2000, 2000, 60000)]));      // touching, barrel, eraser, pressure out of range
    s.write(input([]));
    await sleep(1500);
  }],
  ['streaming, then malformed input', { expect: /dropped \d+ malformed input/, reject: /input Fuzz tablet: (finger 5|finger 7|pen)/ },
    async (s) => {
      s.write(deviceInfo());
      await sleep(1500);
      s.write(msg(19));                                                  // empty
      s.write(input([ptr(5, 1, 1, 1, 1)], 0, 2));                         // count says 2, one given
      s.write(input(Array.from({ length: 11 }, (_, i) => ptr(i, 1, 1, 9, 9))));  // 11 pointers
      s.write(input([ptr(7, 1, 1, 1, 1), ptr(7, 1, 1, 2, 2)]));            // same id twice
      s.write(input([ptr(32, 1, 1, 1, 1)]));                               // id out of range
      s.write(input([ptr(5, 9, 1, 1, 1)]));                                // unknown tool
      s.write(input([ptr(5, 1, 0, 1, 1)]));                                // hovering finger
      s.write(input([ptr(1, 2, 1, 1, 1), ptr(2, 2, 1, 2, 2)]));            // two pens
      s.write(input([ptr(5, 1, 1, 1, 1)], 1));                             // canceled with pointers
      s.write(input([], 0, 255));
      await sleep(1500);
    }],
  ['streaming, then input flood (garbage and valid)', { expect: /streaming started/ }, async (s) => {
    s.write(deviceInfo());
    await sleep(1500);
    for (let i = 0; i < 20000; i++) {
      if (i % 2) s.write(msg(19, randomBytes(i % 90)));
      else s.write(input([ptr(i % 10, 1, 1, (i * 37) & 0xffff, (i * 91) & 0xffff)]));
    }
    s.write(input([]));
    await sleep(3000);
  }],
  ['streaming, 10 fingers down, then the tablet vanishes', { expect: /finger 29 down[\s\S]*finger 29 canceled/ },
    async (s) => {
      s.write(deviceInfo());
      await sleep(1500);
      s.write(input(Array.from({ length: 10 }, (_, i) => ptr(i + 20, 1, 1, i * 6000, i * 6000))));
      await sleep(500);
      s.destroy(); // no lift: the host must let go on its own
      await sleep(2500);
    }],
  // Wi-Fi: the encrypted handshake against a hostile tablet
  ['Wi-Fi: plain DeviceInfo (old app, must not stream)', { wifi: true, expect: /without encryption/, reject: /streaming started/ },
    async (s) => { s.write(deviceInfo()); await sleep(1500); }],
  ['Wi-Fi: unencrypted touches from an unpaired tablet', { wifi: true, reject: /streaming started|input Fuzz/ },
    async (s) => {
      s.write(deviceInfo());
      for (let i = 0; i < 50; i++) s.write(input([ptr(0, 1, 1, i * 1000, i * 1000)]));
      s.write(input([]));
      await sleep(1500);
    }],
  ['Wi-Fi: NotPaired with a 60 KiB name', { wifi: true, reject: /streaming started/ }, async (s) => {
    s.write(msg(15, Buffer.concat([str('id'), u16(60000), Buffer.alloc(60000, 0x41)])));
    await sleep(1500);
  }],
  ['Wi-Fi: SessionReply cut short', { wifi: true, reject: /streaming started/ }, async (s) => {
    s.write(msg(16, Buffer.concat([str('fuzz-id'), randomBytes(20)])));
    await sleep(1500);
  }],
  ['Wi-Fi: SessionReply from an unknown tablet', { wifi: true, reject: /streaming started/ }, async (s) => {
    s.write(msg(16, Buffer.concat([str('fuzz-id'), ecdhKey(), randomBytes(32), randomBytes(32)])));
    await sleep(1500);
  }],
  ['Wi-Fi: Sealed garbage instead of a reply', { wifi: true, reject: /streaming started/ }, async (s) => {
    for (let i = 0; i < 10; i++) s.write(msg(18, randomBytes(4000)));
    await sleep(1500);
  }],
];

async function runCase(name, { expect, reject, wifi }, fn) {
  if (wifi && !lanAddress) {
    console.log(`SKIP  ${name}  (no network address)`);
    return true;
  }
  const address = wifi ? lanAddress : '127.0.0.1';
  // The host retries, so every connection (not only the first) needs an error handler.
  const server = net.createServer((c) => c.on('error', () => {}));
  const output = [];
  await new Promise((r) => server.listen(PORT, address, r));
  const host = spawn(exe, ['--connect', `${address}:${PORT}`, '--fps', '30', '--touch-log'], { stdio: ['ignore', 'pipe', 'pipe'] });
  host.stderr.on('data', (d) => output.push(d.toString()));
  host.stdout.on('data', (d) => output.push(d.toString()));
  let exitCode = null;
  host.on('exit', (code) => { exitCode = code; });

  const sock = await new Promise((resolve) => server.once('connection', resolve));
  sock.on('error', () => {});
  // read the Hello
  await new Promise((r) => sock.once('data', r));
  try {
    await fn(sock);
  } catch {}
  const alive = exitCode === null;
  sock.destroy();
  server.close();
  host.kill();
  await sleep(500);
  const log = output.join('');
  const reacted = (!expect || expect.test(log)) && !(reject && reject.test(log));
  const ok = alive && reacted;
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${alive ? '' : `  (host exited with ${exitCode})`}${reacted ? '' : '  (expected ' + (expect ?? 'no ' + reject) + ')'}`);
  if (!ok) console.log(log.split('\n').slice(-8).join('\n'));
  return ok;
}

let failed = 0;
for (const [name, opts, fn] of cases) if (!(await runCase(name, opts, fn))) failed++;
console.log(failed ? `${failed} case(s) FAILED` : `all ${cases.length} cases passed`);
process.exit(failed ? 1 : 0);
