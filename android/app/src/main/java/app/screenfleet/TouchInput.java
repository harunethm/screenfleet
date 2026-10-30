package app.screenfleet;

import android.view.MotionEvent;

/**
 * Turns touches and pen events on the picture into InputEvent snapshots
 * (PROTOCOL.md): every pointer that is down, plus a hovering pen. The PC
 * works out down / move / up from one snapshot to the next.
 */
final class TouchInput {
    static final int MAX_POINTERS = 10;
    static final int TOOL_FINGER = 1;
    static final int TOOL_PEN = 2;
    static final int TOUCHING = 0x01;
    static final int BARREL = 0x02;
    static final int ERASER = 0x04;
    static final int CANCELED = 0x01;

    private static final int POINTER_BYTES = 9;

    private TouchInput() {}

    /** The snapshot after a touch event on a view of the given size, or null to send nothing. */
    static byte[] fromTouch(MotionEvent e, int viewWidth, int viewHeight) {
        int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_CANCEL) return new byte[] {(byte) CANCELED, 0};
        // the pointer going up is already gone from the snapshot
        int lifted = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP ? e.getActionIndex() : -1;
        return snapshot(e, lifted, true, viewWidth, viewHeight);
    }

    /** The snapshot after a hover event (a pen over the screen), or null if it isn't a pen. */
    static byte[] fromHover(MotionEvent e, int viewWidth, int viewHeight) {
        if (e.getPointerCount() < 1 || toolOf(e, 0) != TOOL_PEN) return null;
        int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_HOVER_EXIT) return new byte[] {0, 0};
        if (action != MotionEvent.ACTION_HOVER_ENTER && action != MotionEvent.ACTION_HOVER_MOVE) return null;
        return snapshot(e, -1, false, viewWidth, viewHeight);
    }

    private static byte[] snapshot(MotionEvent e, int skip, boolean touching, int viewWidth, int viewHeight) {
        int n = e.getPointerCount();
        byte[] out = new byte[2 + Math.min(n, MAX_POINTERS) * POINTER_BYTES];
        int count = 0;
        boolean pen = false;
        int pos = 2;
        for (int i = 0; i < n && count < MAX_POINTERS; i++) {
            if (i == skip) continue;
            int id = e.getPointerId(i);
            if (id < 0 || id > 31) continue;
            int tool = toolOf(e, i);
            if (tool == TOOL_PEN) {
                if (pen) continue; // one pen at a time
                pen = true;
            } else if (!touching) {
                continue; // only a pen hovers
            }
            int state = touching ? TOUCHING : 0;
            int pressure = 0;
            if (tool == TOOL_PEN) {
                if (e.getToolType(i) == MotionEvent.TOOL_TYPE_ERASER) state |= ERASER;
                // the barrel button: BUTTON_SECONDARY on Android 4.x/5.x pens, STYLUS_PRIMARY from Android 6
                if ((e.getButtonState() & (MotionEvent.BUTTON_SECONDARY | MotionEvent.BUTTON_STYLUS_PRIMARY)) != 0) {
                    state |= BARREL;
                }
                if (touching) pressure = Math.round(clamp(e.getPressure(i)) * 1024);
            }
            out[pos] = (byte) id;
            out[pos + 1] = (byte) tool;
            out[pos + 2] = (byte) state;
            put16(out, pos + 3, scale(e.getX(i), viewWidth));
            put16(out, pos + 5, scale(e.getY(i), viewHeight));
            put16(out, pos + 7, pressure);
            pos += POINTER_BYTES;
            count++;
        }
        out[1] = (byte) count;
        if (pos == out.length) return out;
        byte[] shorter = new byte[pos];
        System.arraycopy(out, 0, shorter, 0, pos);
        return shorter;
    }

    private static int toolOf(MotionEvent e, int i) {
        int t = e.getToolType(i);
        return t == MotionEvent.TOOL_TYPE_STYLUS || t == MotionEvent.TOOL_TYPE_ERASER ? TOOL_PEN : TOOL_FINGER;
    }

    /** A view coordinate as 0..65535 across the picture (it fills the view). */
    private static int scale(float v, int size) {
        if (size <= 1) return 0;
        return Math.round(clamp(v / (size - 1)) * 65535);
    }

    private static float clamp(float v) {
        return v < 0 ? 0 : v > 1 ? 1 : v;
    }

    private static void put16(byte[] p, int off, int v) {
        p[off] = (byte) (v >>> 8);
        p[off + 1] = (byte) v;
    }
}
