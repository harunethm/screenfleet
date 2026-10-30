package app.screenfleet;

import android.os.Build;
import android.os.SystemClock;
import android.util.Log;
import android.view.Surface;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;

/**
 * Waits for the PC to connect and runs one streaming session at a time.
 *
 * Android 4.x: listens on 127.0.0.1 only. The PC reaches it through the USB
 * cable with `adb forward`; nothing on the network can connect.
 * Android 5+: listens on all interfaces, so Wi-Fi works too (and USB still does),
 * and answers the PC's discovery query on UDP so it shows up in the PC's list.
 *
 * Over Wi-Fi only encrypted sessions from paired PCs are accepted (USB stays
 * plain: the cable and adb are private). Pairing: the PC starts it, this
 * tablet shows a code that the user types on the PC and taps Pair here.
 *
 * Tapping Connect on the tablet is remembered (requestConnect) and sent to the
 * PC with every DeviceInfo, so a PC that was told to disconnect from this
 * tablet knows the tablet now wants it back.
 */
final class StreamServer {
    /** Called on the server thread. */
    interface Listener {
        void onWaiting();
        void onStreaming(Session session);
        void onStats(int fps, float decodeMs, float mbps);
        void onSessionEnded(Session session);
        /** The decoder failed before showing anything at this size. */
        void onDecoderFailed(int width, int height);
        /** A PC wants to pair: show the code; answer with answerPairing(). */
        void onPairCode(String hostName, String code);
        /** The PC's user typed the code correctly: only now may Pair be tapped. */
        void onPairConfirmed();
        /** Pairing finished: paired, or not (cancelled, wrong code, timed out). */
        void onPairEnded(String hostName, boolean paired);
    }

    /** Where pairing secrets are kept (Prefs). */
    interface PairStore {
        byte[] key(String hostId);
        void save(String hostId, String hostName, byte[] key);
    }

    /** One connection: plain, or Sealed once the session keys are set. */
    static final class Channel {
        final DataInputStream in;
        final OutputStream out;
        private Crypto.SealKey tx, rx;

        Channel(DataInputStream in, OutputStream out) {
            this.in = in;
            this.out = out;
        }

        void secure(Crypto.SealKey tx, Crypto.SealKey rx) {
            this.tx = tx;
            this.rx = rx;
        }

        void read(Protocol.Message m) throws Exception {
            Protocol.read(in, m);
            if (rx == null) return;
            if (m.type != Protocol.SEALED) throw new IOException("expected an encrypted message");
            byte[] plain = rx.open(m.payload, 0, m.length);
            if (plain.length < 1) throw new IOException("empty message");
            m.type = plain[0] & 0xFF;
            m.length = plain.length - 1;
            if (m.payload.length < m.length) m.payload = new byte[m.length + m.length / 2];
            System.arraycopy(plain, 1, m.payload, 0, m.length);
        }

        /** Callers hold the server's send lock. */
        void write(int type, byte[] payload, int length) throws Exception {
            if (tx == null) {
                Protocol.write(out, type, payload, length);
                return;
            }
            byte[] plain = new byte[1 + length];
            plain[0] = (byte) type;
            if (length > 0) System.arraycopy(payload, 0, plain, 1, length);
            byte[] sealed = tx.seal(plain, 0, plain.length);
            Protocol.write(out, Protocol.SEALED, sealed, sealed.length);
        }
    }

    /** What the screens show about the current connection. */
    static final class Session {
        String host = "";
        String link = "USB";
        int width, height, fps;
        long startedAt = System.currentTimeMillis();
        long endedAt;
    }

    /** Supplies the DeviceInfo for each new connection (settings may change between them). */
    interface DeviceInfoSource {
        Protocol.DeviceInfo get();
    }

    private static final String TAG = "SST.Server";

    private final Surface surface;
    private final DeviceInfoSource deviceInfo;
    private final PairStore pairs;
    private final Object pairLock = new Object();
    private Boolean pairAnswer; // the user's tap during a pairing
    /** elapsedRealtime until which new pairing requests are turned away (after one failed). */
    private long pairCooldownUntil;
    private final Listener listener;
    private final boolean wifiEnabled = Build.VERSION.SDK_INT >= 21;

    private volatile boolean running;
    private Thread thread;
    private ServerSocket serverSocket;
    private Thread discoveryThread;
    private DatagramSocket discoverySocket;
    /** elapsedRealtime of the last Connect tap; starting the app counts as one. */
    private volatile long connectRequestedAt = SystemClock.elapsedRealtime();
    private volatile boolean streaming;
    private volatile String streamingTo = "";
    private Socket current;
    private Channel currentChannel;
    private volatile InputSender input;
    private final Object sendLock = new Object();

    StreamServer(Surface surface, DeviceInfoSource deviceInfo, PairStore pairs, Listener listener) {
        this.surface = surface;
        this.deviceInfo = deviceInfo;
        this.pairs = pairs;
        this.listener = listener;
    }

    void start() {
        if (running) return;
        running = true;
        thread = new Thread(this::serve, "SST-server");
        thread.start();
        if (wifiEnabled) {
            discoveryThread = new Thread(this::answerDiscovery, "SST-discovery");
            discoveryThread.start();
        }
    }

    /** The user asked to connect from the tablet; a PC holding off reconnects on its next try. */
    void requestConnect() {
        connectRequestedAt = SystemClock.elapsedRealtime();
    }

    private Protocol.DeviceInfo currentInfo() {
        Protocol.DeviceInfo info = deviceInfo.get();
        info.connectRequestAge = Math.min(Protocol.NO_REQUEST - 1,
                (SystemClock.elapsedRealtime() - connectRequestedAt) / 1000);
        return info;
    }

    /** Stops listening; the PC is told goodbye so it doesn't report an error. */
    void stop() {
        running = false;
        closeQuietly(serverSocket);
        if (discoverySocket != null) discoverySocket.close();
        endSession();
        if (discoveryThread != null) {
            try {
                discoveryThread.join(1000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            discoveryThread = null;
        }
        if (thread != null) {
            try {
                thread.join(2000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            thread = null;
        }
    }

    /** The user tapped Pair (true) or Cancel (false) on the pairing code. */
    void answerPairing(boolean pair) {
        synchronized (pairLock) {
            pairAnswer = pair;
            pairLock.notifyAll();
        }
    }

    /** Sends a touch/pen snapshot (TouchInput) while streaming; dropped otherwise. Any thread. */
    void sendInput(byte[] snapshot) {
        InputSender sender = input;
        if (sender != null) sender.offer(snapshot);
    }

    /**
     * Sends input from its own thread: the UI thread may not touch the
     * network. Each snapshot holds every pointer, so when the connection
     * falls behind the older ones can be dropped and only the latest sent.
     */
    private final class InputSender implements Runnable {
        private final java.util.concurrent.LinkedBlockingQueue<byte[]> queue =
                new java.util.concurrent.LinkedBlockingQueue<>(32);
        private final Channel channel;
        private final Thread thread;
        private volatile boolean stopped;

        InputSender(Channel channel) {
            this.channel = channel;
            thread = new Thread(this, "SST-input");
            thread.start();
        }

        void offer(byte[] snapshot) {
            if (stopped) return;
            if (!queue.offer(snapshot)) {
                queue.clear();
                queue.offer(snapshot);
            }
        }

        void stop() {
            stopped = true;
            thread.interrupt();
        }

        @Override
        public void run() {
            try {
                while (!stopped) {
                    byte[] p = queue.take();
                    synchronized (sendLock) {
                        channel.write(Protocol.INPUT_EVENT, p, p.length);
                    }
                }
            } catch (InterruptedException ignored) {
                // stopped
            } catch (Exception e) {
                stopped = true; // the read loop notices the broken connection
            }
        }
    }

    /** Ends the current session politely; the PC reconnects with the new settings. */
    void restartSession() {
        endSession();
    }

    // Called from the UI thread, where Android forbids network I/O: the
    // goodbye is sent from a short-lived thread (waited for up to 1 s).
    private void endSession() {
        final Socket socket;
        final Channel channel;
        synchronized (this) {
            socket = current;
            channel = currentChannel;
        }
        if (socket == null) return;
        Thread bye = new Thread(() -> {
            synchronized (sendLock) {
                try {
                    if (channel != null) channel.write(Protocol.BYE, null, 0);
                } catch (Exception ignored) {
                }
            }
            closeQuietly(socket);
        }, "SST-bye");
        bye.start();
        try {
            bye.join(1000);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
        closeQuietly(socket);
    }

    private void serve() {
        try {
            serverSocket = new ServerSocket();
            serverSocket.setReuseAddress(true);
            InetAddress bindAddress = wifiEnabled ? null : InetAddress.getByName("127.0.0.1");
            serverSocket.bind(new InetSocketAddress(bindAddress, Protocol.PORT));
        } catch (IOException e) {
            Log.e(TAG, "cannot listen on port " + Protocol.PORT, e);
            return;
        }
        Log.i(TAG, "listening on " + (wifiEnabled ? "all interfaces" : "127.0.0.1") + ":" + Protocol.PORT);

        while (running) {
            listener.onWaiting();
            Socket socket;
            try {
                socket = serverSocket.accept();
            } catch (IOException e) {
                if (running) Log.w(TAG, "accept failed", e);
                continue;
            }
            synchronized (this) {
                current = socket;
            }
            Session session = null;
            try {
                session = new Session();
                runSession(socket, session);
            } catch (Exception e) {
                if (running) Log.i(TAG, "session ended: " + e);
            } finally {
                synchronized (this) {
                    closeQuietly(socket);
                    current = null;
                    currentChannel = null;
                }
                streaming = false;
                if (session != null && session.width > 0) {
                    session.endedAt = System.currentTimeMillis();
                    listener.onSessionEnded(session);
                }
            }
        }
        closeQuietly(serverSocket);
    }

    private void runSession(Socket socket, Session session) throws Exception {
        socket.setTcpNoDelay(true);
        socket.setReceiveBufferSize(1024 * 1024);
        // Whoever connects must finish the handshake quickly, or they would
        // hold the only session (the listener serves one PC at a time).
        socket.setSoTimeout(5000);
        DataInputStream in = new DataInputStream(new BufferedInputStream(socket.getInputStream(), 256 * 1024));
        final OutputStream out = new BufferedOutputStream(socket.getOutputStream(), 16 * 1024);
        final Channel channel = new Channel(in, out);
        synchronized (this) {
            currentChannel = channel;
        }
        Protocol.Message msg = new Protocol.Message();
        final boolean loopback = socket.getInetAddress().isLoopbackAddress();

        channel.read(msg);
        if (msg.type == Protocol.PAIR_START) {
            if (wifiEnabled && SystemClock.elapsedRealtime() >= pairCooldownUntil) {
                runPairing(socket, channel, msg);
            } else {
                synchronized (sendLock) {
                    channel.write(Protocol.BYE, null, 0);
                }
            }
            return;
        }
        if (msg.type != Protocol.HELLO) throw new IOException("not a ScreenFleet host");
        Protocol.Hello hello = Protocol.Hello.decode(msg);
        session.host = cleanName(hello.hostName);
        session.link = loopback ? "USB" : "Wi‑Fi";
        Protocol.DeviceInfo device = currentInfo();
        if (!loopback) {
            // Wi-Fi: only encrypted, only paired
            if ((hello.flags & Protocol.HELLO_SECURE) == 0 || hello.epk == null) {
                synchronized (sendLock) {
                    channel.write(Protocol.BYE, null, 0);
                }
                throw new IOException("refused an unencrypted connection over Wi-Fi");
            }
            if (!secureSession(channel, msg, hello, device)) return;
        }
        byte[] info = device.encode();
        synchronized (sendLock) {
            channel.write(Protocol.DEVICE_INFO, info, info.length);
        }
        if (hello.version != Protocol.VERSION) throw new IOException("host protocol v" + hello.version);

        channel.read(msg);
        if (msg.type == Protocol.BYE) return; // the PC only looked (it holds off until asked, or is busy)
        if (msg.type != Protocol.STREAM_START) throw new IOException("expected StreamStart");
        Protocol.StreamStart stream = Protocol.StreamStart.decode(msg);
        session.width = stream.width;
        session.height = stream.height;
        session.fps = stream.fps;
        if (stream.width < 16 || stream.height < 16 || stream.width > 4096 || stream.height > 4096) {
            throw new IOException("bad stream size " + stream.width + "x" + stream.height);
        }
        // The PC pings every 2 s even when the screen is still; silence for
        // 10 s means it is gone (crashed, asleep, cable pulled).
        socket.setSoTimeout(10000);
        Log.i(TAG, "stream " + stream.width + "x" + stream.height + " @ " + stream.fps + " fps, "
                + stream.bitrate / 1000 + " kbit/s from " + session.host + " over " + session.link);

        VideoDecoder decoder = new VideoDecoder(surface, stream.width, stream.height, () -> {
            synchronized (sendLock) {
                try {
                    channel.write(Protocol.REQUEST_KEY_FRAME, null, 0);
                } catch (Exception ignored) {
                    // the read loop notices the broken connection
                }
            }
        });

        InputSender sender = new InputSender(channel);
        input = sender;
        boolean announced = false;
        long statsAt = SystemClock.elapsedRealtime();
        long bytes = 0;
        try {
            while (running) {
                channel.read(msg);
                switch (msg.type) {
                    case Protocol.VIDEO_FRAME: {
                        if (msg.length < Protocol.VIDEO_FRAME_HEADER) break;
                        int flags = msg.payload[0] & 0xFF;
                        long ptsUs = Protocol.u64(msg.payload, 1);
                        decoder.submit(msg.payload, Protocol.VIDEO_FRAME_HEADER,
                                msg.length - Protocol.VIDEO_FRAME_HEADER,
                                (flags & Protocol.FRAME_KEY) != 0, ptsUs);
                        bytes += msg.length;
                        if (!announced) {
                            announced = true;
                            streamingTo = session.host;
                            streaming = true;
                            listener.onStreaming(session);
                        }
                        break;
                    }
                    case Protocol.PING:
                        synchronized (sendLock) {
                            channel.write(Protocol.PONG, msg.payload, msg.length);
                        }
                        break;
                    case Protocol.BYE:
                        Log.i(TAG, "host said goodbye");
                        return;
                    default:
                        break; // unknown messages are ignored (forward compatibility)
                }

                // Once a second: tell the PC (and the stats line) how decoding goes.
                // Frames only arrive when the PC's screen changes, so a still
                // screen reports 0 fps, the same as the PC's own chart.
                long now = SystemClock.elapsedRealtime();
                if (now - statsAt >= 1000) {
                    int[] s = decoder.takeStats();
                    float seconds = (now - statsAt) / 1000f;
                    byte[] stats = Protocol.deviceStats(s[1], s[0]);
                    synchronized (sendLock) {
                        channel.write(Protocol.DEVICE_STATS, stats, stats.length);
                    }
                    listener.onStats(Math.round(s[0] / seconds), s[1] / 1000f, bytes * 8 / 1e6f / seconds);
                    bytes = 0;
                    statsAt = now;
                }
            }
        } finally {
            input = null;
            sender.stop();
            streaming = false;
            boolean failedAtStart = decoder.failedAtStart();
            decoder.release();
            if (failedAtStart) listener.onDecoderFailed(stream.width, stream.height);
        }
    }

    /**
     * Answers "who is there?" from PCs on the network with this tablet's
     * name, size and whether it is streaming. Only well-formed, full-size
     * queries get an answer, and the answer is smaller than the query.
     */
    private void answerDiscovery() {
        try {
            discoverySocket = new DatagramSocket(null);
            discoverySocket.setReuseAddress(true);
            discoverySocket.bind(new InetSocketAddress(Protocol.PORT));
        } catch (IOException e) {
            Log.w(TAG, "discovery unavailable", e);
            return;
        }
        byte[] buf = new byte[Protocol.DISCOVERY_QUERY_SIZE + 1];
        long windowStart = 0;
        int answered = 0;
        while (running) {
            DatagramPacket packet = new DatagramPacket(buf, buf.length);
            try {
                discoverySocket.receive(packet);
            } catch (IOException e) {
                if (running) Log.w(TAG, "discovery receive failed", e);
                break;
            }
            if (packet.getLength() != Protocol.DISCOVERY_QUERY_SIZE
                    || (int) Protocol.u32(buf, 0) != Protocol.DISCOVERY_QUERY) continue;
            long now = SystemClock.elapsedRealtime();
            if (now - windowStart > 1000) {
                windowStart = now;
                answered = 0;
            }
            if (++answered > 20) continue; // a flood: don't become a reflector
            try {
                byte[] reply = currentInfo().encodeDiscoveryReply(streaming, streamingTo);
                discoverySocket.send(new DatagramPacket(reply, reply.length, packet.getAddress(), packet.getPort()));
            } catch (Exception e) {
                Log.w(TAG, "discovery reply failed", e);
            }
        }
        discoverySocket.close();
    }

    /**
     * The encrypted-session handshake (PROTOCOL.md). False if this PC isn't
     * paired (it is told so and the connection ends).
     */
    private boolean secureSession(Channel channel, Protocol.Message msg, Protocol.Hello hello, Protocol.DeviceInfo device)
            throws Exception {
        byte[] k = pairs.key(hello.hostId);
        if (k == null) {
            byte[] reply = new Protocol.Writer().str(device.deviceId).str(device.model).bytes();
            synchronized (sendLock) {
                channel.write(Protocol.NOT_PAIRED, reply, reply.length);
            }
            Log.i(TAG, "not paired with " + cleanName(hello.hostName));
            return false;
        }
        Crypto.Ecdh eph = new Crypto.Ecdh();
        byte[] nD = Crypto.random(32);
        byte[] z = eph.agree(hello.epk);
        byte[] t = Crypto.sha256(Crypto.utf8("SST session v1"), Crypto.str(hello.hostId), Crypto.str(device.deviceId),
                hello.epk, eph.publicKey, hello.nonce, nD);
        byte[] ikm = new byte[k.length + z.length];
        System.arraycopy(k, 0, ikm, 0, k.length);
        System.arraycopy(z, 0, ikm, k.length, z.length);
        byte[] keys = Crypto.hkdf(t, ikm, "SST session keys", 96);
        java.util.Arrays.fill(ikm, (byte) 0);
        byte[] kConfirm = java.util.Arrays.copyOfRange(keys, 64, 96);
        byte[] macD = Crypto.hmac(kConfirm, Crypto.utf8("device"), t);
        byte[] reply = new Protocol.Writer().str(device.deviceId).raw(eph.publicKey).raw(nD).raw(macD).bytes();
        synchronized (sendLock) {
            channel.write(Protocol.SESSION_REPLY, reply, reply.length);
        }
        channel.read(msg);
        if (msg.type != Protocol.SESSION_CONFIRM || msg.length != 32) throw new IOException("expected SessionConfirm");
        byte[] macH = java.util.Arrays.copyOf(msg.payload, 32);
        if (!Crypto.equal(macH, Crypto.hmac(kConfirm, Crypto.utf8("host"), t))) {
            throw new SecurityException("the PC failed the session check");
        }
        channel.secure(new Crypto.SealKey(java.util.Arrays.copyOfRange(keys, 32, 64), 2),
                new Crypto.SealKey(java.util.Arrays.copyOfRange(keys, 0, 32), 1));
        java.util.Arrays.fill(keys, (byte) 0);
        return true;
    }

    /** Pairing (PROTOCOL.md), started by the PC; the user sees the code and taps Pair or Cancel. */
    private void runPairing(Socket socket, Channel channel, Protocol.Message msg) throws Exception {
        Protocol.Reader r = new Protocol.Reader(msg.payload, msg.length);
        String hostId = r.str();
        String hostName = cleanName(r.str());
        byte[] commit = r.raw(32);
        Protocol.DeviceInfo device = currentInfo();
        Crypto.Ecdh eph = new Crypto.Ecdh();
        byte[] nD = Crypto.random(32);
        byte[] reply = new Protocol.Writer().str(device.deviceId).str(device.model).raw(eph.publicKey).raw(nD).bytes();
        synchronized (sendLock) {
            channel.write(Protocol.PAIR_REPLY, reply, reply.length);
        }
        socket.setSoTimeout(15000);
        channel.read(msg);
        if (msg.type != Protocol.PAIR_REVEAL || msg.length < 97) throw new IOException("expected PairReveal");
        byte[] pkH = java.util.Arrays.copyOfRange(msg.payload, 0, 65);
        byte[] nH = java.util.Arrays.copyOfRange(msg.payload, 65, 97);
        if (!Crypto.equal(commit, Crypto.sha256(pkH, nH))) throw new SecurityException("the PC changed its key");
        byte[] z = eph.agree(pkH);
        byte[] t = Crypto.sha256(Crypto.utf8("SST pair v1"), Crypto.str(hostId), Crypto.str(device.deviceId), pkH,
                eph.publicKey, nH, nD);
        byte[] h = Crypto.sha256(Crypto.utf8("SST code"), t);
        long value = (((h[0] & 0xFFL) << 24) | ((h[1] & 0xFFL) << 16) | ((h[2] & 0xFFL) << 8) | (h[3] & 0xFFL)) % 1000000;
        String code = String.format(java.util.Locale.ROOT, "%06d", value);
        byte[] k = Crypto.hkdf(t, z, "SST pairing key", 32);
        java.util.Arrays.fill(z, (byte) 0);

        synchronized (pairLock) {
            pairAnswer = null;
        }
        listener.onPairCode(hostName, code);
        boolean paired = false;
        try {
            // the PC's user types the code (then PairConfirm comes), or cancels
            socket.setSoTimeout(120000);
            channel.read(msg);
            if (msg.type != Protocol.PAIR_CONFIRM || msg.length != 32) return;
            byte[] confirm = java.util.Arrays.copyOf(msg.payload, 32);
            if (!Crypto.equal(confirm, Crypto.hmac(k, Crypto.utf8("host"), t))) return;
            // this tablet's user taps Pair (only possible from here on)
            listener.onPairConfirmed();
            boolean yes;
            synchronized (pairLock) {
                pairAnswer = Boolean.FALSE.equals(pairAnswer) ? pairAnswer : null;
                long until = android.os.SystemClock.elapsedRealtime() + 120000;
                while (pairAnswer == null && running) {
                    long left = until - android.os.SystemClock.elapsedRealtime();
                    if (left <= 0) break;
                    pairLock.wait(left);
                }
                yes = Boolean.TRUE.equals(pairAnswer);
            }
            synchronized (sendLock) {
                if (yes) {
                    byte[] done = Crypto.hmac(k, Crypto.utf8("device"), t);
                    channel.write(Protocol.PAIR_DONE, done, done.length);
                } else {
                    channel.write(Protocol.BYE, null, 0);
                }
            }
            if (yes) {
                pairs.save(hostId, hostName, k);
                paired = true;
                Log.i(TAG, "paired with " + hostName);
            }
        } finally {
            java.util.Arrays.fill(k, (byte) 0);
            // a refused or failed pairing: turn new requests away for a while (no dialog spam)
            if (!paired) pairCooldownUntil = SystemClock.elapsedRealtime() + 10000;
            listener.onPairEnded(hostName, paired);
        }
    }

    /** The PC's name comes from the network: one short line of printable text. */
    private static String cleanName(String name) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < name.length() && b.length() < 64; ++i) {
            char c = name.charAt(i);
            if (!Character.isISOControl(c)) b.append(c);
        }
        return b.toString();
    }

    private static void closeQuietly(ServerSocket s) {
        if (s == null) return;
        try {
            s.close();
        } catch (IOException ignored) {
        }
    }

    private static void closeQuietly(Socket s) {
        if (s == null) return;
        try {
            s.close();
        } catch (IOException ignored) {
        }
    }
}
