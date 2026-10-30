// Robustness test for the Android app: plays a misbehaving PC against the
// tablet's listener and checks after every case that the app is still
// running and still answers a well-behaved PC.
//   USB (default): through `adb forward`, like the PC app; plain sessions.
//   --host IP: straight over Wi-Fi, where the tablet must refuse anything
//   that isn't an encrypted session from a paired PC (Android 5+).
// Usage: node tools/fuzz-device.mjs [--device SERIAL] [--host IP] [--adb PATH]
// Stop the PC app first: the tablet serves one PC at a time.
import net from 'node:net';
import { execFileSync } from 'node:child_process';
import { createECDH, randomBytes } from 'node:crypto';

const arg = (name) => { const i = process.argv.indexOf(name); return i > 0 ? process.argv[i + 1] : undefined; };
const adb = arg('--adb') ?? `${process.env.LOCALAPPDATA}\\Android\\Sdk\\platform-tools\\adb.exe`;
const device = arg('--device');
const wifiHost = arg('--host');
const target = wifiHost ?? '127.0.0.1';
const adbArgs = (...a) => (device ? ['-s', device, ...a] : a);
const PORT = 27183;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const u16 = (v) => Buffer.from([(v >> 8) & 255, v & 255]);
const u32 = (v) => { const b = Buffer.alloc(4); b.writeUInt32BE(v >>> 0); return b; };
const u64 = (v) => { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt(v)); return b; };
const str = (s) => Buffer.concat([u16(Buffer.byteLength(s)), Buffer.from(s)]);
const msg = (type, payload = Buffer.alloc(0)) => Buffer.concat([Buffer.from([type]), u32(payload.length), payload]);
const hello = (name = 'FUZZ') => msg(1, Buffer.concat([u32(0x53535442), u16(1), str(name)]));
// Hello asking for an encrypted session (Wi-Fi), from a PC the tablet doesn't know
const ecdhKey = () => { const e = createECDH('prime256v1'); e.generateKeys(); return e.getPublicKey(); };
const secureHello = (epk = ecdhKey(), hostId = 'fuzz-' + randomBytes(4).toString('hex')) =>
  msg(1, Buffer.concat([u32(0x53535442), u16(1), str('FUZZ'), Buffer.from([1]), str(hostId), epk, randomBytes(32)]));
const pairStart = (commit = randomBytes(32)) => msg(10, Buffer.concat([str('fuzz-host'), str('FUZZ PC'), commit]));
const start = (w, h, fps = 60) => msg(3, Buffer.concat([Buffer.from([1]), u16(w), u16(h), u16(fps), u32(8e6), Buffer.from([1])]));
const frame = (flags, data) => msg(4, Buffer.concat([Buffer.from([flags]), u64(0), data]));

// Valid H.264 parameter sets for a 64x64 baseline stream, followed by junk:
// gets the decoder configured, then feeds it nonsense.
const sps = Buffer.from('0000000167428015da0d0fd00f0880000003008000000f478c19500', 'hex');
const pps = Buffer.from('0000000168ce3c80', 'hex');

function connect() {
  return new Promise((resolve, reject) => {
    const s = net.connect(PORT, target, () => resolve(s));
    s.on('error', reject);
    s.on('data', () => {}); // drain
  });
}

async function run(name, fn) {
  let s;
  let verdict = true;  // a case returns false when the tablet reacted wrongly
  try {
    s = await connect();
    s.on('error', () => {});
    verdict = (await fn(s)) !== false;
  } catch (e) {
    // a closed connection is an acceptable reaction to bad input
  } finally {
    s?.destroy();
  }
  await sleep(600);
  let pid = '';
  try {
    pid = execFileSync(adb, adbArgs('shell', 'pidof', 'app.screenfleet')).toString().trim();
    if (!/^\d+$/.test(pid)) pid = '';  // Android 4.4: "pidof: not found"
  } catch {}
  if (!pid) {  // Android 4.4 has no pidof
    const ps = execFileSync(adb, adbArgs('shell', 'ps')).toString();
    pid = ps.split('\n').some((l) => l.trim().endsWith('app.screenfleet')) ? 'running' : '';
  }
  const ok = await healthy();
  const pass = ok && !!pid && verdict;
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}${pid ? '' : '  (app not running!)'}${ok ? '' : '  (no longer answers a valid Hello)'}${verdict ? '' : '  (wrong reaction)'}`);
  if (!ok || !pid) {
    // start it again so the remaining cases still get tested
    await sleep(12000);  // a crashed media service takes ~10 s to come back
    execFileSync(adb, adbArgs('shell', 'am', 'force-stop', 'app.screenfleet'));
    execFileSync(adb, adbArgs('shell', 'am', 'start', '-n', 'app.screenfleet/.MainActivity'));
    for (let i = 0; i < 20 && !(await healthy()); i++) await sleep(1000);
  }
  return pass;
}

// A well-behaved PC: over USB Hello gets DeviceInfo; over Wi-Fi an encrypted
// Hello from an unknown PC gets NotPaired.
function healthy() {
  return new Promise((resolve) => {
    const s = net.connect(PORT, target, () => s.write(wifiHost ? secureHello() : hello('HEALTH')));
    let got = Buffer.alloc(0);
    const done = (v) => { s.destroy(); resolve(v); };
    s.on('data', (d) => {
      got = Buffer.concat([got, d]);
      if (got.length >= 5) done(got[0] === (wifiHost ? 15 : 2));
    });
    s.on('error', () => done(false));
    setTimeout(() => done(false), 6000);
  });
}

const cases = [
  ['silent peer (must be dropped after the 5 s handshake timeout)', async (s) => { await sleep(6500); }],
  ['random bytes', async (s) => { s.write(randomBytes(4096)); await sleep(300); }],
  ['wrong magic', async (s) => { s.write(msg(1, Buffer.concat([u32(0xdeadbeef), u16(1)]))); await sleep(300); }],
  ['Hello too short', async (s) => { s.write(msg(1, Buffer.from([1, 2]))); await sleep(300); }],
  ['length 0xFFFFFFFF', async (s) => { s.write(Buffer.concat([Buffer.from([1]), u32(0xffffffff)])); await sleep(300); }],
  ['length just over 8 MiB', async (s) => { s.write(Buffer.concat([Buffer.from([4]), u32(8 * 1024 * 1024 + 1)])); await sleep(300); }],
  ['host name 64 KiB of control characters', async (s) => {
    const name = Buffer.alloc(65535, 7);
    s.write(msg(1, Buffer.concat([u32(0x53535442), u16(1), u16(65535), name])));
    await sleep(500);
  }],
  ['wrong protocol version', async (s) => { s.write(msg(1, Buffer.concat([u32(0x53535442), u16(99)]))); await sleep(300); }],
  ['StreamStart 0 x 0', async (s) => { s.write(Buffer.concat([hello(), start(0, 0)])); await sleep(500); }],
  ['StreamStart 65535 x 65535', async (s) => { s.write(Buffer.concat([hello(), start(65535, 65535)])); await sleep(500); }],
  ['StreamStart truncated', async (s) => { s.write(Buffer.concat([hello(), msg(3, Buffer.from([1, 0]))])); await sleep(500); }],
  ['VideoFrame shorter than its header', async (s) => {
    s.write(Buffer.concat([hello(), start(64, 64), msg(4, Buffer.from([1, 2, 3]))]));
    await sleep(500);
  }],
  ['key frame of random bytes', async (s) => {
    s.write(Buffer.concat([hello(), start(64, 64), frame(1, randomBytes(5000))]));
    await sleep(800);
  }],
  ['valid SPS/PPS then 200 frames of junk', async (s) => {
    s.write(Buffer.concat([hello(), start(64, 64), frame(1, Buffer.concat([sps, pps, randomBytes(2000)]))]));
    for (let i = 0; i < 200; i++) s.write(frame(i % 30 === 0 ? 1 : 0, Buffer.concat([Buffer.from([0, 0, 0, 1, 0x41]), randomBytes(1500)])));
    await sleep(1500);
  }],
  ['unknown message types', async (s) => {
    s.write(hello());
    for (let t = 9; t < 255; t += 7) s.write(msg(t, randomBytes(64)));
    await sleep(500);
  }],
  ['Ping flood with 1 MiB payloads', async (s) => {
    s.write(Buffer.concat([hello(), start(64, 64)]));
    for (let i = 0; i < 20; i++) s.write(msg(6, randomBytes(1024 * 1024)));
    await sleep(1500);
  }],
  ['abrupt close mid-message', async (s) => {
    s.write(Buffer.concat([hello(), start(64, 64), Buffer.from([4]), u32(100000), randomBytes(1000)]));
    await sleep(200);
  }],
  // pairing and encrypted sessions
  ['Sealed message instead of Hello', async (s) => { s.write(msg(18, randomBytes(300))); await sleep(300); }],
  ['PairStart truncated', async (s) => { s.write(msg(10, Buffer.from([0, 9, 1]))); await sleep(500); }],
  ['PairStart, then a reveal that breaks the commitment', async (s) => {
    s.write(pairStart());
    await sleep(800);
    s.write(msg(12, Buffer.concat([ecdhKey(), randomBytes(32)])));
    await sleep(800);
  }],
  ['PairStart, then a reveal with a key off the curve', async (s) => {
    const bad = Buffer.concat([Buffer.from([4]), randomBytes(64)]);
    const nonce = randomBytes(32);
    const { createHash } = await import('node:crypto');
    s.write(pairStart(createHash('sha256').update(bad).update(nonce).digest()));
    await sleep(800);
    s.write(msg(12, Buffer.concat([bad, nonce])));
    await sleep(800);
  }],
  ['PairStart, then a forged PairConfirm', async (s) => { s.write(pairStart()); await sleep(800); s.write(msg(13, randomBytes(32))); await sleep(800); }],
  ['encrypted Hello with a key off the curve', async (s) => {
    s.write(secureHello(Buffer.concat([Buffer.from([4]), randomBytes(64)])));
    await sleep(500);
  }],
  ['encrypted Hello cut short', async (s) => {
    s.write(msg(1, Buffer.concat([u32(0x53535442), u16(1), str('FUZZ'), Buffer.from([1]), str('x'), randomBytes(10)])));
    await sleep(500);
  }],
  ['forged SessionConfirm and Sealed frames', async (s) => {
    s.write(secureHello());
    await sleep(500);
    s.write(msg(17, randomBytes(32)));
    for (let i = 0; i < 20; i++) s.write(msg(18, randomBytes(2000)));
    await sleep(500);
  }],
];
// Over Wi-Fi the tablet must refuse an unencrypted PC outright.
if (wifiHost) {
  cases.unshift(['unencrypted Hello and StreamStart over Wi-Fi (must be refused)', async (s) => {
    s.write(Buffer.concat([hello(), start(64, 64), frame(1, Buffer.concat([sps, pps]))]));
    const reply = await new Promise((r) => { let b = Buffer.alloc(0); s.on('data', (d) => { b = Buffer.concat([b, d]); r(b); }); setTimeout(() => r(b), 2000); });
    return reply.length === 0 || reply[0] === 8;  // closed, or Bye: anything else means it answered
  }]);
}

if (!wifiHost) execFileSync(adb, adbArgs('forward', `tcp:${PORT}`, `tcp:${PORT}`));
if (!(await healthy())) {
  console.log('The app is not answering; open ScreenFleet on the tablet first.');
  process.exit(2);
}
let failed = 0;
for (const [name, fn] of cases) if (!(await run(name, fn))) failed++;
console.log(failed ? `${failed} case(s) FAILED` : `all ${cases.length} cases passed`);
process.exit(failed ? 1 : 0);
