package app.screenfleet;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.Charset;

/** Wire protocol v1, see protocol/PROTOCOL.md. All integers are big-endian. */
final class Protocol {
    static final int MAGIC = 0x53535442; // "SSTB"
    static final int VERSION = 1;
    static final int PORT = 27183;
    static final int MAX_PAYLOAD = 8 * 1024 * 1024;

    static final int HELLO = 1;
    static final int DEVICE_INFO = 2;
    static final int STREAM_START = 3;
    static final int VIDEO_FRAME = 4;
    static final int REQUEST_KEY_FRAME = 5;
    static final int PING = 6;
    static final int PONG = 7;
    static final int BYE = 8;
    static final int DEVICE_STATS = 9;
    // pairing and encrypted sessions over Wi-Fi (PROTOCOL.md)
    static final int PAIR_START = 10;
    static final int PAIR_REPLY = 11;
    static final int PAIR_REVEAL = 12;
    static final int PAIR_CONFIRM = 13;
    static final int PAIR_DONE = 14;
    static final int NOT_PAIRED = 15;
    static final int SESSION_REPLY = 16;
    static final int SESSION_CONFIRM = 17;
    static final int SEALED = 18;
    /** Touch and pen snapshot, tablet -> PC (built by TouchInput). */
    static final int INPUT_EVENT = 19;

    static final int HELLO_SECURE = 0x01;

    static final int FRAME_KEY = 0x01;
    static final int FRAME_CONFIG = 0x02;

    static final int DEVICE_TOUCH = 0x01;
    static final int DEVICE_PEN = 0x02;
    /** The decoder holds frames until more arrive: send a few repeats after each change. */
    static final int DEVICE_FLUSH_FRAMES = 0x04;

    static final int VIDEO_FRAME_HEADER = 1 + 8;

    // Discovery over UDP (Wi-Fi, Android 5+): the PC broadcasts a query, the tablet answers.
    static final int DISCOVERY_QUERY = 0x53535451; // "SSTQ"
    static final int DISCOVERY_REPLY = 0x53535452; // "SSTR"
    /** Queries are padded to this size so a reply is never larger than the query (no amplification). */
    static final int DISCOVERY_QUERY_SIZE = 256;
    static final int DISCOVERY_BUSY = 0x01;
    /** DeviceInfo/discovery value for "the user never asked to connect". */
    static final long NO_REQUEST = 0xFFFFFFFFL;

    private static final Charset UTF8 = Charset.forName("UTF-8");

    private Protocol() {}

    /** A received message. The payload buffer is reused between reads. */
    static final class Message {
        int type;
        int length;
        byte[] payload = new byte[256 * 1024];
    }

    static void read(DataInputStream in, Message m) throws IOException {
        m.type = in.readUnsignedByte();
        int length = in.readInt();
        if (length < 0 || length > MAX_PAYLOAD) throw new IOException("bad message length " + length);
        if (m.payload.length < length) m.payload = new byte[length + length / 2];
        in.readFully(m.payload, 0, length);
        m.length = length;
    }

    static void write(OutputStream out, int type, byte[] payload, int length) throws IOException {
        byte[] head = {
            (byte) type,
            (byte) (length >>> 24), (byte) (length >>> 16), (byte) (length >>> 8), (byte) length
        };
        out.write(head);
        if (length > 0) out.write(payload, 0, length);
        out.flush();
    }

    static final class DeviceInfo {
        int width, height, densityDpi, sdkInt, maxFps, flags;
        String model = "";
        /** Seconds since the user last tapped Connect on the tablet, or NO_REQUEST. */
        long connectRequestAge = NO_REQUEST;
        String deviceId = "";

        byte[] encode() throws IOException {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            DataOutputStream d = new DataOutputStream(bytes);
            d.writeShort(VERSION);
            d.writeShort(width);
            d.writeShort(height);
            d.writeShort(densityDpi);
            d.writeShort(sdkInt);
            d.writeShort(maxFps);
            d.writeInt(flags);
            writeString(d, model);
            d.writeInt((int) connectRequestAge);
            writeString(d, deviceId);
            return bytes.toByteArray();
        }

        /** The discovery reply: who this tablet is and whether it is free. */
        byte[] encodeDiscoveryReply(boolean busy, String host) throws IOException {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            DataOutputStream d = new DataOutputStream(bytes);
            d.writeInt(DISCOVERY_REPLY);
            d.writeShort(VERSION);
            d.writeShort(PORT);
            d.writeByte(busy ? DISCOVERY_BUSY : 0);
            d.writeShort(width);
            d.writeShort(height);
            d.writeShort(sdkInt);
            d.writeInt((int) connectRequestAge);
            writeString(d, model);
            writeString(d, deviceId);
            writeString(d, host);
            return bytes.toByteArray();
        }
    }

    /** At most 64 bytes of UTF-8, so a discovery reply stays well under the query size. */
    private static void writeString(DataOutputStream d, String s) throws IOException {
        byte[] b = s.getBytes(UTF8);
        int n = Math.min(b.length, 64);
        d.writeShort(n);
        d.write(b, 0, n);
    }

    static final class StreamStart {
        int codec, width, height, fps, bitrate, flags;

        static StreamStart decode(Message m) throws IOException {
            if (m.length < 11) throw new IOException("short StreamStart");
            byte[] p = m.payload;
            StreamStart s = new StreamStart();
            s.codec = p[0] & 0xFF;
            s.width = u16(p, 1);
            s.height = u16(p, 3);
            s.fps = u16(p, 5);
            s.bitrate = (int) u32(p, 7);
            s.flags = m.length > 11 ? p[11] & 0xFF : 0;
            return s;
        }
    }

    /** Hello, with the fields for an encrypted session when the host sends them. */
    static final class Hello {
        int version;
        String hostName = "";
        int flags;
        String hostId = "";
        byte[] epk;   // 65 bytes
        byte[] nonce; // 32 bytes

        static Hello decode(Message m) throws IOException {
            Reader r = new Reader(m.payload, m.length);
            if (r.u32() != (MAGIC & 0xFFFFFFFFL)) throw new IOException("not a ScreenFleet host");
            Hello h = new Hello();
            h.version = r.u16();
            if (r.left() > 0) h.hostName = r.str();
            if (r.left() > 0) {
                h.flags = r.u8();
                h.hostId = r.str();
                h.epk = r.raw(65);
                h.nonce = r.raw(32);
            }
            return h;
        }
    }

    /** Reads big-endian fields; throws on short input. */
    static final class Reader {
        private final byte[] p;
        private final int n;
        private int pos;

        Reader(byte[] p, int n) {
            this.p = p;
            this.n = n;
        }

        int left() {
            return n - pos;
        }

        private void need(int k) throws IOException {
            if (k < 0 || n - pos < k) throw new IOException("short message");
        }

        int u8() throws IOException {
            need(1);
            return p[pos++] & 0xFF;
        }

        int u16() throws IOException {
            need(2);
            int v = Protocol.u16(p, pos);
            pos += 2;
            return v;
        }

        long u32() throws IOException {
            need(4);
            long v = Protocol.u32(p, pos);
            pos += 4;
            return v;
        }

        byte[] raw(int k) throws IOException {
            need(k);
            byte[] out = new byte[k];
            System.arraycopy(p, pos, out, 0, k);
            pos += k;
            return out;
        }

        String str() throws IOException {
            return new String(raw(u16()), UTF8);
        }
    }

    /** Builds a payload: strings as u16 length + UTF-8 (at most 64 bytes), raw bytes as they are. */
    static final class Writer {
        private final ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        private final DataOutputStream d = new DataOutputStream(bytes);

        Writer str(String s) throws IOException {
            writeString(d, s);
            return this;
        }

        Writer raw(byte[] b) throws IOException {
            d.write(b);
            return this;
        }

        byte[] bytes() {
            return bytes.toByteArray();
        }
    }

    /** The host name appended to Hello after the first v1 release; "" if absent. */
    static String helloHostName(Message m) {
        if (m.length < 8) return "";
        int len = u16(m.payload, 6);
        if (8 + len > m.length) return "";
        return new String(m.payload, 8, len, UTF8);
    }

    static byte[] deviceStats(int decodeUs, int framesShown) {
        return new byte[] {
            (byte) (decodeUs >>> 24), (byte) (decodeUs >>> 16), (byte) (decodeUs >>> 8), (byte) decodeUs,
            (byte) (framesShown >>> 8), (byte) framesShown
        };
    }

    static int u16(byte[] p, int off) {
        return ((p[off] & 0xFF) << 8) | (p[off + 1] & 0xFF);
    }

    static long u32(byte[] p, int off) {
        return ((long) u16(p, off) << 16) | u16(p, off + 2);
    }

    static long u64(byte[] p, int off) {
        return (u32(p, off) << 32) | u32(p, off + 4);
    }
}
