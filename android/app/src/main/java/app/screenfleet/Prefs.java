package app.screenfleet;

import android.content.Context;
import android.content.SharedPreferences;

import android.util.Base64;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.UUID;

/** The app's settings and the last session, kept in SharedPreferences. */
final class Prefs {
    static final String RES_NATIVE = "native";
    static final String RES_SMALLER = "smaller";
    static final String ORIENT_LANDSCAPE = "landscape";
    static final String ORIENT_PORTRAIT = "portrait";

    private final SharedPreferences p;

    Prefs(Context context) {
        p = context.getSharedPreferences("settings", Context.MODE_PRIVATE);
    }

    /** A random id made on first use, so the PC can tell two tablets of the same model apart. */
    String deviceId() {
        String id = p.getString("deviceId", null);
        if (id == null) {
            id = UUID.randomUUID().toString();
            p.edit().putString("deviceId", id).apply();
        }
        return id;
    }

    String resolution() { return p.getString("resolution", RES_NATIVE); }
    int maxFps() { return p.getInt("maxFps", 60); }
    String orientation() { return p.getString("orientation", ORIENT_LANDSCAPE); }
    boolean keepScreenOn() { return p.getBoolean("keepScreenOn", true); }
    boolean showStats() { return p.getBoolean("showStats", true); }
    /** Touches and the pen on the picture go to the PC (off for tablets with a broken touch screen). */
    boolean touchControlsPc() { return p.getBoolean("touchControlsPc", true); }
    /** Largest picture (pixels) to ask for, learnt when the decoder failed at a bigger one; 0 = none. */
    int decoderMaxPixels() { return p.getInt("decoderMaxPixels", 0); }
    void setDecoderMaxPixels(int v) { p.edit().putInt("decoderMaxPixels", v).apply(); }

    void setResolution(String v) { p.edit().putString("resolution", v).apply(); }
    void setMaxFps(int v) { p.edit().putInt("maxFps", v).apply(); }
    void setOrientation(String v) { p.edit().putString("orientation", v).apply(); }
    void setKeepScreenOn(boolean v) { p.edit().putBoolean("keepScreenOn", v).apply(); }
    void setShowStats(boolean v) { p.edit().putBoolean("showStats", v).apply(); }
    void setTouchControlsPc(boolean v) { p.edit().putBoolean("touchControlsPc", v).apply(); }

    /** The Wi-Fi pairing secret shared with a PC, or null if not paired. */
    byte[] pairKey(String hostId) {
        String v = p.getString("pair." + hostId, null);
        return v == null ? null : Base64.decode(v, Base64.NO_WRAP);
    }

    void savePairing(String hostId, String hostName, byte[] key) {
        p.edit().putString("pair." + hostId, Base64.encodeToString(key, Base64.NO_WRAP))
                .putString("pairName." + hostId, hostName).apply();
    }

    /** Names of the paired PCs. */
    List<String> pairedHosts() {
        List<String> names = new ArrayList<>();
        for (Map.Entry<String, ?> e : p.getAll().entrySet()) {
            if (e.getKey().startsWith("pairName.")) names.add(String.valueOf(e.getValue()));
        }
        return names;
    }

    void forgetPairings() {
        SharedPreferences.Editor e = p.edit();
        for (String k : p.getAll().keySet()) {
            if (k.startsWith("pair.") || k.startsWith("pairName.")) e.remove(k);
        }
        e.apply();
    }

    /** "DESKTOP-PC · today 13:41–13:52 · USB", or null before the first session. */
    String lastSession() { return p.getString("lastSession", null); }
    void setLastSession(String v) { p.edit().putString("lastSession", v).apply(); }
    void forgetLastSession() { p.edit().remove("lastSession").apply(); }
}
