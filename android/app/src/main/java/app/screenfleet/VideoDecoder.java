package app.screenfleet;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;
import android.os.SystemClock;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;

/**
 * Hardware H.264 decoder that renders straight to a Surface. Frames are shown
 * the moment they're decoded (no presentation-time scheduling): lowest latency.
 */
final class VideoDecoder {
    interface Listener {
        /** The stream can't continue until a new key frame arrives. */
        void onKeyFrameNeeded();
    }

    private static final String TAG = "SST.Decoder";
    private static final String MIME = "video/avc";

    private final Surface surface;
    private final int width;
    private final int height;
    private final Listener listener;

    private MediaCodec codec;
    private ByteBuffer[] inputBuffers;
    private Thread outputThread;
    private volatile boolean running;
    private volatile boolean anyOutput;
    private volatile boolean failed;
    private boolean waitingForKeyFrame = true;
    private boolean keyFrameRequested;

    // Decode latency statistics (queue -> output available).
    private final long[] queueTimes = new long[256];
    private final long[] queuePts = new long[256];
    private int queueIndex;
    private int statFrames;
    private int statTimed; // frames whose decode time is known
    private long statLatencyNs;
    private static final long STALE_NS = 3_000_000_000L;
    private long statStart = SystemClock.elapsedRealtime();

    VideoDecoder(Surface surface, int width, int height, Listener listener) {
        this.surface = surface;
        this.width = width;
        this.height = height;
        this.listener = listener;
    }

    String name() {
        return codec != null ? codec.getName() : "";
    }

    /** Feeds one access unit (Annex-B). Called from the network thread. */
    void submit(byte[] data, int offset, int length, boolean keyFrame, long ptsUs) throws Exception {
        if (waitingForKeyFrame) {
            if (!keyFrame) {
                requestKeyFrameOnce();
                return;
            }
            if (codec == null) start(data, offset, length);
            waitingForKeyFrame = false;
            keyFrameRequested = false;
        }

        int index = codec.dequeueInputBuffer(500_000);
        if (index < 0) {
            // Decoder is stuck or far behind. Dropping this frame breaks the
            // picture chain, so wait for the next key frame.
            Log.w(TAG, "no input buffer, dropping frame");
            waitingForKeyFrame = true;
            requestKeyFrameOnce();
            return;
        }
        ByteBuffer buffer = inputBuffers[index];
        buffer.clear();
        if (buffer.capacity() < length) {
            Log.w(TAG, "frame of " + length + " bytes exceeds input buffer " + buffer.capacity());
            codec.queueInputBuffer(index, 0, 0, ptsUs, 0);
            waitingForKeyFrame = true;
            requestKeyFrameOnce();
            return;
        }
        buffer.put(data, offset, length);
        synchronized (queueTimes) {
            int slot = queueIndex++ & (queueTimes.length - 1);
            queueTimes[slot] = System.nanoTime();
            queuePts[slot] = ptsUs;
        }
        codec.queueInputBuffer(index, 0, length, ptsUs, 0);
    }

    private void requestKeyFrameOnce() {
        if (!keyFrameRequested) {
            keyFrameRequested = true;
            listener.onKeyFrameNeeded();
        }
    }

    private void start(byte[] data, int offset, int length) throws Exception {
        MediaFormat format = MediaFormat.createVideoFormat(MIME, width, height);
        // Some (older) decoders only start when SPS/PPS come via the format.
        ByteBuffer sps = findNal(data, offset, length, 7);
        ByteBuffer pps = findNal(data, offset, length, 8);
        if (sps != null && pps != null) {
            format.setByteBuffer("csd-0", sps);
            format.setByteBuffer("csd-1", pps);
        }
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, Math.max(width * height * 3 / 2, 512 * 1024));
        // MediaTek "ClearMotion" interpolates streams of up to 35 fps by holding
        // each frame until the next one arrives: on a still desktop that is
        // seconds. Declaring a high rate keeps it out of the way.
        format.setInteger(MediaFormat.KEY_FRAME_RATE, 60);
        if (Build.VERSION.SDK_INT >= 23) format.setInteger(MediaFormat.KEY_PRIORITY, 0); // real time
        if (Build.VERSION.SDK_INT >= 30) format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);

        codec = MediaCodec.createDecoderByType(MIME);
        codec.configure(format, surface, null, 0);
        codec.start();
        inputBuffers = codec.getInputBuffers();
        Log.i(TAG, "decoder " + name() + " started, " + width + "x" + height);

        running = true;
        outputThread = new Thread(new Runnable() {
            @Override public void run() {
                drainOutput();
            }
        }, "SST-decoder-output");
        outputThread.start();
    }

    private void drainOutput() {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        try {
            while (running) {
                int index = codec.dequeueOutputBuffer(info, 100_000);
                if (index >= 0) {
                    anyOutput = true;
                    recordLatency(info.presentationTimeUs);
                    codec.releaseOutputBuffer(index, true); // render now
                } else if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    Log.i(TAG, "output format " + codec.getOutputFormat());
                }
            }
        } catch (IllegalStateException e) {
            if (running) {
                failed = true;
                Log.w(TAG, "decoder output stopped", e);
            }
        }
    }

    /** The decoder broke before showing a single frame: most likely the size is too big for it. */
    boolean failedAtStart() {
        return failed && !anyOutput;
    }

    private static int[] fitted;
    private static long fittedFor;

    /**
     * The largest size with the same shape as width x height that this
     * device's H.264 decoder handles. Some decoders (the e-tab5's MT6592)
     * stop at 1080p and have no fallback, so asking for the full screen
     * would show nothing at all.
     */
    static synchronized int[] fitSize(int width, int height, int maxPixels) {
        long key = (long) width * 65536 + height + ((long) maxPixels << 32);
        if (fitted != null && fittedFor == key) return fitted.clone();
        MediaCodecInfo info = avcDecoder();
        MediaCodecInfo.CodecCapabilities caps = null;
        try {
            if (info != null) caps = info.getCapabilitiesForType(MIME);
        } catch (RuntimeException ignored) {
        }
        int maxFs = 8192; // H.264 level 4.x (1080p) when the decoder doesn't say
        if (caps != null && caps.profileLevels != null) {
            int best = 0;
            for (MediaCodecInfo.CodecProfileLevel pl : caps.profileLevels) best = Math.max(best, maxFrameMacroblocks(pl.level));
            if (best > 0) maxFs = best;
        }
        int w = width, h = height;
        for (int step = 0; step < 60; ++step) {
            int mbs = ((w + 15) / 16) * ((h + 15) / 16);
            int longSide = Math.max(w, h), shortSide = Math.min(w, h);
            // Level 4.x decoders tend to check "1080p" by side, not by area. Before
            // Android 5 decoders also overstate their level (the e-tab5 claims more
            // than its 1080p), so there 1080p is the limit whatever they say.
            boolean sideLimit = maxFs <= 8704 || Build.VERSION.SDK_INT < 21;
            boolean fits = mbs <= maxFs && (!sideLimit || (longSide <= 1920 && shortSide <= 1088))
                    && (maxPixels <= 0 || w * h <= maxPixels);
            if (fits && Build.VERSION.SDK_INT >= 21 && caps != null && caps.getVideoCapabilities() != null) {
                fits = caps.getVideoCapabilities().isSizeSupported(w, h);
            }
            if (fits) break;
            w = (int) (w * 0.97f) & ~1;
            h = (int) (h * 0.97f) & ~1;
        }
        if (w != width) Log.i(TAG, "decoder limit: " + width + "x" + height + " -> " + w + "x" + h);
        fitted = new int[] {w, h};
        fittedFor = key;
        return fitted.clone();
    }

    /** The decoder MediaCodec.createDecoderByType picks: the first that takes H.264. */
    @SuppressWarnings("deprecation")
    private static MediaCodecInfo avcDecoder() {
        try {
            for (int i = 0; i < MediaCodecList.getCodecCount(); ++i) {
                MediaCodecInfo info = MediaCodecList.getCodecInfoAt(i);
                if (info.isEncoder()) continue;
                for (String type : info.getSupportedTypes()) {
                    if (type.equalsIgnoreCase(MIME)) return info;
                }
            }
        } catch (RuntimeException ignored) {
        }
        return null;
    }

    /** MaxFS from the H.264 level table (Annex A). */
    private static int maxFrameMacroblocks(int level) {
        switch (level) {
            case MediaCodecInfo.CodecProfileLevel.AVCLevel3: return 1620;
            case MediaCodecInfo.CodecProfileLevel.AVCLevel31: return 3600;
            case MediaCodecInfo.CodecProfileLevel.AVCLevel32: return 5120;
            case MediaCodecInfo.CodecProfileLevel.AVCLevel4:
            case MediaCodecInfo.CodecProfileLevel.AVCLevel41: return 8192;
            case MediaCodecInfo.CodecProfileLevel.AVCLevel42: return 8704;
            case MediaCodecInfo.CodecProfileLevel.AVCLevel5: return 22080;
            case MediaCodecInfo.CodecProfileLevel.AVCLevel51:
            case MediaCodecInfo.CodecProfileLevel.AVCLevel52: return 36864;
            default: return level > MediaCodecInfo.CodecProfileLevel.AVCLevel52 ? 139264 : 0;
        }
    }

    private void recordLatency(long ptsUs) {
        long now = System.nanoTime();
        synchronized (queueTimes) {
            ++statFrames;
            for (int i = 0; i < queueTimes.length; ++i) {
                if (queueTimes[i] == 0) continue;
                long age = now - queueTimes[i];
                if (age > STALE_NS) {
                    // a frame the decoder dropped, or gave back with another timestamp
                    // (older MediaTek decoders do): it would read as seconds of delay
                    queueTimes[i] = 0;
                } else if (queuePts[i] == ptsUs) {
                    statLatencyNs += age;
                    ++statTimed;
                    queueTimes[i] = 0;
                }
            }
        }
    }

    /**
     * Frames shown and their average decode time (µs, 0 if none) since the
     * last call. Called about once a second.
     */
    int[] takeStats() {
        synchronized (queueTimes) {
            int[] out = {statFrames, statTimed > 0 ? (int) (statLatencyNs / 1000 / statTimed) : 0};
            if (statFrames > 0) {
                Log.i(TAG, String.format("decoded %d frames in %d ms, decode latency %.1f ms", statFrames,
                        SystemClock.elapsedRealtime() - statStart, statTimed > 0 ? statLatencyNs / 1e6 / statTimed : 0));
            }
            statFrames = 0;
            statTimed = 0;
            statLatencyNs = 0;
            statStart = SystemClock.elapsedRealtime();
            return out;
        }
    }

    void release() {
        running = false;
        if (outputThread != null) {
            try {
                outputThread.join(500);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }
        if (codec != null) {
            // On a separate thread with a time limit: when the media service
            // dies under a decoder (the e-tab5's MediaTek driver crashes on
            // some broken streams), stop() waits forever for an answer, and
            // this tablet would stop listening for the PC.
            final MediaCodec dying = codec;
            codec = null;
            Thread stopper = new Thread(() -> {
                try {
                    dying.stop();
                } catch (Exception ignored) {
                }
                try {
                    dying.release();
                } catch (Exception ignored) {
                }
            }, "SST-decoder-release");
            stopper.start();
            try {
                stopper.join(2000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            if (stopper.isAlive()) Log.w(TAG, "decoder didn't stop (media service gone?); leaving it behind");
        }
    }

    /** Returns the NAL unit of the given type, including its start code. */
    private static ByteBuffer findNal(byte[] d, int off, int len, int type) {
        int end = off + len;
        int start = -1;
        for (int i = off; i + 3 < end; ++i) {
            boolean sc4 = d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1;
            boolean sc3 = d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1;
            if (!sc3 && !sc4) continue;
            if (start >= 0) return copy(d, start, i - start);
            int header = i + (sc4 ? 4 : 3);
            if (header < end && (d[header] & 0x1F) == type) start = i;
            i = header - 1;
        }
        return start >= 0 ? copy(d, start, end - start) : null;
    }

    // The network buffer is reused, so the codec config gets its own copy.
    private static ByteBuffer copy(byte[] d, int off, int len) {
        ByteBuffer b = ByteBuffer.allocate(len);
        b.put(d, off, len);
        b.flip();
        return b;
    }
}
