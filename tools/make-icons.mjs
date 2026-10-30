// Draws the product mark (a monitor with the tablet beside it) and writes:
//   android/app/src/main/res/mipmap-*/ic_launcher.png   (48..192 px)
//   host-windows/res/sst.ico                            (16..256 px, PNG entries)
// Usage: node tools/make-icons.mjs   (Node 18+, no packages)
import { deflateSync } from 'node:zlib';
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');

// The mark in its 22 x 16 design grid (same as the app and the mockup).
function inMark(x, y) {
  const t = 1.6;
  const inside = (x0, y0, x1, y1) => x >= x0 && x < x1 && y >= y0 && y < y1;
  const outer = inside(1 - t / 2, 1.5 - t / 2, 14 + t / 2, 11.5 + t / 2);
  const inner = inside(1 + t / 2, 1.5 + t / 2, 14 - t / 2, 11.5 - t / 2);
  const stand = inside(5.5 - t / 2, 14.5 - t / 2, 9.5 + t / 2, 14.5 + t / 2);
  const tablet = inside(16, 5.5, 21, 12.5);
  return (outer && !inner) || stand || tablet;
}

// A rounded square of `bg` with the mark in `fg`; markScale is the mark's
// width as a share of the icon. bg = null gives a transparent background.
function draw(size, { bg, fg, markScale, radius }) {
  const px = new Uint8Array(size * size * 4);
  const ss = 4;
  const scale = (size * markScale) / 22;
  const offX = (size - 22 * scale) / 2;
  const offY = (size - 16 * scale) / 2;
  const r = radius * size;
  const inRounded = (x, y) => {
    const cx = Math.min(Math.max(x, r), size - r), cy = Math.min(Math.max(y, r), size - r);
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r;
  };
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      let bgHits = 0, fgHits = 0;
      for (let sy = 0; sy < ss; sy++)
        for (let sx = 0; sx < ss; sx++) {
          const fx = x + (sx + 0.5) / ss, fy = y + (sy + 0.5) / ss;
          if (!bg || inRounded(fx, fy)) bgHits++;
          if (inMark((fx - offX) / scale, (fy - offY) / scale)) fgHits++;
        }
      const n = ss * ss, a = bgHits / n, m = fgHits / n;
      const base = bg ?? [0, 0, 0];
      const i = (y * size + x) * 4;
      // mark over background, both with coverage
      const alpha = bg ? a : m;
      for (let c = 0; c < 3; c++) {
        const v = bg ? base[c] * (1 - m) + fg[c] * m : fg[c];
        px[i + c] = Math.round(v);
      }
      px[i + 3] = Math.round(255 * alpha);
    }
  }
  return px;
}

function crc32(buf) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function png(size, rgba) {
  const chunk = (type, data) => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const body = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(body));
    return Buffer.concat([len, body, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0);
  ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8; ihdr[9] = 6; // 8-bit RGBA
  const raw = Buffer.alloc(size * (size * 4 + 1));
  for (let y = 0; y < size; y++) {
    raw[y * (size * 4 + 1)] = 0;
    Buffer.from(rgba.buffer, y * size * 4, size * 4).copy(raw, y * (size * 4 + 1) + 1);
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0)),
  ]);
}

function ico(images) {
  const header = Buffer.alloc(6);
  header.writeUInt16LE(0, 0); header.writeUInt16LE(1, 2); header.writeUInt16LE(images.length, 4);
  const dir = Buffer.alloc(16 * images.length);
  let offset = 6 + dir.length;
  images.forEach(({ size, data }, i) => {
    const e = i * 16;
    dir[e] = size >= 256 ? 0 : size; dir[e + 1] = size >= 256 ? 0 : size;
    dir.writeUInt16LE(1, e + 4); dir.writeUInt16LE(32, e + 6);
    dir.writeUInt32LE(data.length, e + 8); dir.writeUInt32LE(offset, e + 12);
    offset += data.length;
  });
  return Buffer.concat([header, dir, ...images.map((i) => i.data)]);
}

const ink = [0x14, 0x14, 0x14], white = [0xff, 0xff, 0xff];

// Android launcher: white mark on a near-black rounded square.
for (const [dpi, size] of [['mdpi', 48], ['hdpi', 72], ['xhdpi', 96], ['xxhdpi', 144], ['xxxhdpi', 192]]) {
  const file = join(root, 'android/app/src/main/res', `mipmap-${dpi}`, 'ic_launcher.png');
  mkdirSync(dirname(file), { recursive: true });
  writeFileSync(file, png(size, draw(size, { bg: ink, fg: white, markScale: 0.62, radius: 0.22 })));
  console.log('wrote', file);
}

// Windows: the same tile, from 16 to 256 px (Explorer, Alt+Tab, taskbar).
const icoFile = join(root, 'host-windows/res/sst.ico');
writeFileSync(icoFile, ico([16, 20, 24, 32, 40, 48, 64, 256].map((s) => ({
  size: s, data: png(s, draw(s, { bg: ink, fg: white, markScale: s <= 24 ? 0.78 : 0.66, radius: 0.2 })),
}))));
console.log('wrote', icoFile);
