package app.screenfleet;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.pm.ActivityInfo;
import android.content.Context;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.net.wifi.WifiManager;
import android.graphics.Color;
import android.graphics.Region;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.Collections;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * The app's one screen. Before the PC connects it shows how to connect and the
 * settings; while streaming it is only the picture, a stats line and an edge
 * handle that opens the menu, which holds the same settings.
 */
public class MainActivity extends Activity implements SurfaceHolder.Callback, StreamServer.Listener {
    private enum Mode { WAITING, PAUSED, STREAMING }

    private static final String WIFI = "Wi‑Fi";
    private static final String DOT = " · ";
    /** How long a settings change may take to bring the picture back before the menu gives up on it. */
    private static final long RECONNECT_GRACE_MS = 6000;

    private final Handler handler = new Handler(Looper.getMainLooper());

    private Ui ui;
    private Prefs prefs;
    private SurfaceView surfaceView;
    private StreamServer server;
    private WifiManager.MulticastLock broadcastLock;
    private AlertDialog pairDialog;
    private TextView pairedText;
    private Mode mode = Mode.WAITING;
    private StreamServer.Session session;
    /** Until when a settings change keeps the stream screen up while the PC reconnects. */
    private long reconnectingUntil;

    // main screen
    private ScrollView home;
    private LinearLayout columns;
    private TextView headline, sub, homeAction, lastText, foot;
    private LinearLayout wifiCard, lastCard;
    // streaming
    private FrameLayout overlay;
    /** The menu is open or sliding; its area is drawn over the picture. */
    private boolean sheetShown;
    private TextView stats;
    private ScrollView sheet;
    private TextView sheetTitle, sheetSub;
    // the settings appear twice (main screen and menu); these keep both copies in step
    private final List<Ui.Segmented> resolutionControls = new ArrayList<>();
    private final List<Ui.Segmented> fpsControls = new ArrayList<>();
    private final List<Ui.Segmented> orientationControls = new ArrayList<>();
    private final List<Ui.Toggle> keepOnControls = new ArrayList<>();
    private final List<Ui.Toggle> statsControls = new ArrayList<>();
    private final List<Ui.Toggle> touchControls = new ArrayList<>();
    private final List<TextView> resolutionDescs = new ArrayList<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        prefs = new Prefs(this);
        ui = new Ui(this);
        applyKeepScreenOn();
        applyOrientation();

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        surfaceView = new SurfaceView(this);
        root.addView(surfaceView, match());
        root.addView(buildOverlay(), match());
        root.addView(buildHome(), match());
        setContentView(root);
        surfaceView.getHolder().addCallback(this);
        surfaceView.setOnTouchListener((v, e) -> {
            if (touchActive()) send(TouchInput.fromTouch(e, v.getWidth(), v.getHeight()));
            return true;
        });
        surfaceView.setOnHoverListener((v, e) -> {
            if (touchActive()) send(TouchInput.fromHover(e, v.getWidth(), v.getHeight()));
            return true;
        });
        render();
        // adb shell am start -n app.screenfleet/.MainActivity --ez selftest true
        if (Build.VERSION.SDK_INT >= 21 && getIntent().getBooleanExtra("selftest", false)) {
            new Thread(() -> android.util.Log.i("SST.Crypto", "self-test:\n" + Crypto.selfTest()), "SST-selftest").start();
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUi();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        layoutColumns();
        refreshTexts();
    }

    @Override
    public void onBackPressed() {
        if (sheetOpen() && mode == Mode.STREAMING) {
            closeSheet();
        } else {
            super.onBackPressed();
        }
    }

    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    // ------------------------------------------------------------ surface & server

    // The server lives as long as the surface it draws on.
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (server == null && mode != Mode.PAUSED) startServer();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        stopServer();
    }

    private void startServer() {
        server = new StreamServer(surfaceView.getHolder().getSurface(), this::describeDevice,
                new StreamServer.PairStore() {
                    @Override public byte[] key(String hostId) {
                        return prefs.pairKey(hostId);
                    }

                    @Override public void save(String hostId, String hostName, byte[] key) {
                        prefs.savePairing(hostId, hostName, key);
                    }
                }, this);
        server.start();
        if (Build.VERSION.SDK_INT >= 21 && broadcastLock == null) {
            WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wifi != null) {
                broadcastLock = wifi.createMulticastLock("SST-discovery");
                broadcastLock.setReferenceCounted(false);
                broadcastLock.acquire();
            }
        }
    }

    private void stopServer() {
        if (server != null) {
            server.stop();
            server = null;
        }
        if (broadcastLock != null) {
            broadcastLock.release();
            broadcastLock = null;
        }
    }

    @Override
    public void onWaiting() {
        runOnUiThread(() -> {
            session = null;
            long left = reconnectingUntil - SystemClock.uptimeMillis();
            if (mode == Mode.STREAMING && left > 0) {
                // a settings change: the PC comes straight back, keep the picture and menu up
                handler.postDelayed(this::reconnectTimedOut, left);
            } else if (mode == Mode.STREAMING) {
                mode = Mode.WAITING;
                closeSheetNow();
            }
            render();
        });
    }

    private void reconnectTimedOut() {
        if (session == null && mode == Mode.STREAMING && SystemClock.uptimeMillis() >= reconnectingUntil) {
            mode = Mode.WAITING;
            closeSheetNow();
            render();
        }
    }

    @Override
    public void onStreaming(StreamServer.Session s) {
        runOnUiThread(() -> {
            session = s;
            reconnectingUntil = 0;
            mode = Mode.STREAMING;
            stats.setText("");
            render();
        });
    }

    @Override
    public void onStats(int fps, float decodeMs, float mbps) {
        runOnUiThread(() -> stats.setText(String.format(Locale.US, "%d fps   %s decode   %.1f Mbit/s", fps,
                decodeMs > 0 ? Math.round(decodeMs) + " ms" : "–", mbps)));
    }

    @Override
    public void onSessionEnded(StreamServer.Session s) {
        SimpleDateFormat time = new SimpleDateFormat("HH:mm", Locale.getDefault());
        Calendar started = Calendar.getInstance();
        started.setTimeInMillis(s.startedAt);
        Calendar today = Calendar.getInstance();
        boolean sameDay = started.get(Calendar.YEAR) == today.get(Calendar.YEAR)
                && started.get(Calendar.DAY_OF_YEAR) == today.get(Calendar.DAY_OF_YEAR);
        String day = sameDay ? "today " : new SimpleDateFormat("d MMM ", Locale.getDefault()).format(new Date(s.startedAt));
        String text = (s.host.isEmpty() ? "PC" : s.host) + DOT + day + time.format(new Date(s.startedAt)) + "–"
                + time.format(new Date(s.endedAt)) + DOT + s.link;
        prefs.setLastSession(text);
        runOnUiThread(this::refreshTexts);
    }

    /** What this tablet tells the PC, from the current settings. */
    private Protocol.DeviceInfo describeDevice() {
        Display display = getWindowManager().getDefaultDisplay();
        DisplayMetrics metrics = new DisplayMetrics();
        display.getRealMetrics(metrics);
        boolean portrait = Prefs.ORIENT_PORTRAIT.equals(prefs.orientation());
        int longSide = Math.max(metrics.widthPixels, metrics.heightPixels);
        int shortSide = Math.min(metrics.widthPixels, metrics.heightPixels);

        Protocol.DeviceInfo info = new Protocol.DeviceInfo();
        int[] size = streamSize(portrait ? shortSide : longSide, portrait ? longSide : shortSide);
        info.width = size[0];
        info.height = size[1];
        if (Prefs.RES_SMALLER.equals(prefs.resolution())) {
            info.width = info.width * 3 / 4 & ~1;
            info.height = info.height * 3 / 4 & ~1;
        }
        info.densityDpi = metrics.densityDpi;
        info.sdkInt = Build.VERSION.SDK_INT;
        info.maxFps = Math.min(prefs.maxFps(), Math.max(30, Math.round(display.getRefreshRate())));
        info.flags = hasTouchScreen() && prefs.touchControlsPc() ? Protocol.DEVICE_TOUCH : 0;
        // Decoders before Android 5 (the e-tab5's MediaTek) keep the last frames
        // until newer ones push them out, ignoring the stream's "no reordering".
        if (Build.VERSION.SDK_INT < 21) info.flags |= Protocol.DEVICE_FLUSH_FRAMES;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device != null && prefs.touchControlsPc()
                    && (device.getSources() & InputDevice.SOURCE_STYLUS) == InputDevice.SOURCE_STYLUS) {
                info.flags |= Protocol.DEVICE_PEN;
            }
        }
        info.model = deviceName();
        info.deviceId = prefs.deviceId();
        return info;
    }

    /** The screen size, made to fit what the decoder can show. */
    private int[] streamSize(int width, int height) {
        return VideoDecoder.fitSize(width, height, prefs.decoderMaxPixels());
    }

    @Override
    public void onPairCode(String hostName, String code) {
        runOnUiThread(() -> {
            if (pairDialog != null) pairDialog.dismiss();
            final StreamServer s = server;
            pairDialog = new AlertDialog.Builder(this)
                    .setTitle("Pair with " + (hostName.isEmpty() ? "this PC" : hostName) + "?")
                    .setMessage(pairMessage(code, false))
                    .setPositiveButton("Pair", (d, w) -> {
                        if (s != null) s.answerPairing(true);
                    })
                    .setNegativeButton("Cancel", (d, w) -> {
                        if (s != null) s.answerPairing(false);
                    })
                    .setCancelable(false)
                    .create();
            pairDialog.show();
            pairDialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
            pairCode = code;
        });
    }

    private String pairCode = "";

    private static String pairMessage(String code, boolean confirmed) {
        return "Type this code on your PC:\n\n" + code.substring(0, 3) + " " + code.substring(3) + "\n\n"
                + (confirmed ? "Your PC took the code. Tap Pair if you just typed it there yourself."
                             : "Pair becomes available once your PC has taken the code.");
    }

    @Override
    public void onPairConfirmed() {
        runOnUiThread(() -> {
            if (pairDialog == null) return;
            pairDialog.setMessage(pairMessage(pairCode, true));
            pairDialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
        });
    }

    @Override
    public void onPairEnded(String hostName, boolean paired) {
        runOnUiThread(() -> {
            if (pairDialog != null) {
                pairDialog.dismiss();
                pairDialog = null;
            }
            Toast.makeText(this, paired ? "Paired with " + hostName : "Not paired", Toast.LENGTH_SHORT).show();
            refreshTexts();
        });
    }

    @Override
    public void onDecoderFailed(int width, int height) {
        // Ask for a smaller picture next time (each side 80 %); the PC reconnects on its own.
        int max = Math.max(320 * 200, width * height * 16 / 25);
        prefs.setDecoderMaxPixels(max);
        android.util.Log.w("SST", "decoder failed at " + width + "x" + height + "; asking for at most " + max + " pixels");
        runOnUiThread(this::refreshTexts);
    }

    private static String deviceName() {
        String model = Build.MODEL;
        return model.toLowerCase(Locale.ROOT).startsWith(Build.MANUFACTURER.toLowerCase(Locale.ROOT))
                ? model : Build.MANUFACTURER + " " + model;
    }

    // ------------------------------------------------------------ the main screen

    private View buildHome() {
        home = new ScrollView(this);
        home.setFillViewport(true);
        home.setBackgroundColor(ui.ground);
        FrameLayout centre = new FrameLayout(this);
        columns = new LinearLayout(this);
        columns.setPadding(ui.dp(24), ui.dp(24), ui.dp(24), ui.dp(24));
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER_VERTICAL);
        centre.addView(columns, lp);
        home.addView(centre);

        // left: what this is and how to connect
        LinearLayout left = column();
        LinearLayout brand = new LinearLayout(this);
        brand.setGravity(Gravity.CENTER_VERTICAL);
        brand.addView(new Ui.Mark(ui, ui.text));
        TextView name = ui.text("ScreenFleet", 15, true, ui.text);
        name.setPadding(ui.dp(10), 0, 0, 0);
        brand.addView(name);
        left.addView(brand);
        headline = ui.text("", 28, true, ui.text);
        headline.setPadding(0, ui.dp(16), 0, 0);
        left.addView(headline);
        sub = ui.text("", 15, false, ui.muted);
        sub.setPadding(0, ui.dp(4), 0, 0);
        left.addView(sub);
        homeAction = ui.button("", true, v -> onHomeAction());
        LinearLayout.LayoutParams alp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        alp.topMargin = ui.dp(12);
        left.addView(homeAction, alp);

        LinearLayout usb = ui.card();
        usb.setPadding(ui.dp(16), ui.dp(16), ui.dp(16), ui.dp(16));
        usb.addView(titleRow("USB cable", ui.pill("Ready", ui.pillPositiveBg, ui.pillPositiveInk)));
        TextView usbText = ui.text("Plug this tablet into the PC. The first time, allow USB debugging when Android asks.",
                15, false, ui.muted);
        usbText.setPadding(0, ui.dp(6), 0, 0);
        usb.addView(usbText);
        left.addView(usb, spaced());

        wifiCard = ui.card();
        wifiCard.setPadding(ui.dp(16), ui.dp(16), ui.dp(16), ui.dp(16));
        left.addView(wifiCard, spaced());

        // right: settings and the last session
        LinearLayout right = column();
        LinearLayout settings = ui.card();
        addSettings(settings, false);
        if (Build.VERSION.SDK_INT >= 21) {
            settings.addView(ui.rule());
            pairedText = ui.text("", 12, false, ui.muted);
            TextView forgetPcs = ui.text("Forget", 15, true, ui.text);
            forgetPcs.setPaintFlags(forgetPcs.getPaintFlags() | android.graphics.Paint.UNDERLINE_TEXT_FLAG);
            forgetPcs.setPadding(ui.dp(8), ui.dp(12), 0, ui.dp(12));
            forgetPcs.setOnClickListener(v -> {
                prefs.forgetPairings();
                refreshTexts();
            });
            settings.addView(row("Paired PCs", pairedText, forgetPcs, false));
        }
        right.addView(settings);

        lastCard = ui.card();
        lastCard.setPadding(ui.dp(16), ui.dp(16), ui.dp(16), ui.dp(16));
        TextView forget = ui.text("Forget", 15, true, ui.text);
        forget.setPaintFlags(forget.getPaintFlags() | android.graphics.Paint.UNDERLINE_TEXT_FLAG);
        forget.setOnClickListener(v -> {
            prefs.forgetLastSession();
            refreshTexts();
        });
        lastCard.addView(titleRow("Last connected", forget));
        lastText = ui.text("", 15, false, ui.muted);
        lastText.setPadding(0, ui.dp(6), 0, 0);
        lastCard.addView(lastText);
        right.addView(lastCard, spaced());

        foot = ui.text("", 12, false, ui.muted);
        foot.setPadding(0, ui.dp(12), 0, 0);
        right.addView(foot);

        LinearLayout.LayoutParams l = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.15f);
        LinearLayout.LayoutParams r = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        columns.addView(left, l);
        columns.addView(right, r);
        layoutColumns();
        return home;
    }

    /** Two columns in landscape, one under the other in portrait. */
    private void layoutColumns() {
        boolean portrait = getResources().getConfiguration().orientation == Configuration.ORIENTATION_PORTRAIT;
        columns.setOrientation(portrait ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        for (int i = 0; i < columns.getChildCount(); ++i) {
            LinearLayout.LayoutParams lp = (LinearLayout.LayoutParams) columns.getChildAt(i).getLayoutParams();
            lp.width = portrait ? ViewGroup.LayoutParams.MATCH_PARENT : 0;
            lp.weight = portrait ? 0 : (i == 0 ? 1.15f : 1f);
            lp.leftMargin = !portrait && i > 0 ? ui.dp(16) : 0;
            lp.topMargin = portrait && i > 0 ? ui.dp(16) : 0;
            columns.getChildAt(i).setLayoutParams(lp);
        }
    }

    private LinearLayout column() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        return c;
    }

    private LinearLayout.LayoutParams spaced() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = ui.dp(12);
        return lp;
    }

    private View titleRow(String title, View right) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(ui.text(title, 15, true, ui.text), new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        row.addView(right);
        return row;
    }

    /**
     * The settings rows, used on the main screen and in the in-stream menu.
     * The menu is narrow, so there each choice sits under its label.
     */
    private void addSettings(LinearLayout target, boolean stacked) {
        TextView resDesc = ui.text("", 12, false, ui.muted);
        resolutionDescs.add(resDesc);
        Ui.Segmented res = new Ui.Segmented(ui, new String[] {"Native", "Smaller"},
                Prefs.RES_SMALLER.equals(prefs.resolution()) ? 1 : 0, i -> {
                    prefs.setResolution(i == 1 ? Prefs.RES_SMALLER : Prefs.RES_NATIVE);
                    syncSettings();
                    reconnectForNewSettings();
                });
        resolutionControls.add(res);
        target.addView(row("Resolution", resDesc, res, stacked));
        target.addView(ui.rule());

        Ui.Segmented fps = new Ui.Segmented(ui, new String[] {"60 fps", "30 fps"}, prefs.maxFps() == 30 ? 1 : 0, i -> {
            prefs.setMaxFps(i == 1 ? 30 : 60);
            syncSettings();
            reconnectForNewSettings();
        });
        fpsControls.add(fps);
        target.addView(row("Frame rate", ui.text("The PC sends no more than this.", 12, false, ui.muted), fps, stacked));
        target.addView(ui.rule());

        Ui.Segmented orient = new Ui.Segmented(ui, new String[] {"Landscape", "Portrait"},
                Prefs.ORIENT_PORTRAIT.equals(prefs.orientation()) ? 1 : 0, i -> {
                    prefs.setOrientation(i == 1 ? Prefs.ORIENT_PORTRAIT : Prefs.ORIENT_LANDSCAPE);
                    syncSettings();
                    applyOrientation();
                    reconnectForNewSettings();
                });
        orientationControls.add(orient);
        target.addView(row("Orientation", ui.text("Rotate to match how the tablet stands.", 12, false, ui.muted),
                orient, stacked));
        target.addView(ui.rule());

        Ui.Toggle keepOn = new Ui.Toggle(ui, prefs.keepScreenOn(), on -> {
            prefs.setKeepScreenOn(on);
            syncSettings();
            applyKeepScreenOn();
        });
        keepOnControls.add(keepOn);
        target.addView(row("Keep screen on", ui.text("While the app is open.", 12, false, ui.muted), keepOn, false,
                stacked));
        target.addView(ui.rule());

        Ui.Toggle showStats = new Ui.Toggle(ui, prefs.showStats(), this::setShowStats);
        statsControls.add(showStats);
        target.addView(row(stacked ? "Show stats" : "Show stats while streaming",
                ui.text("Frame rate, decode time and bitrate in a corner.", 12, false, ui.muted), showStats, false,
                stacked));
        target.addView(ui.rule());

        Ui.Toggle touch = new Ui.Toggle(ui, prefs.touchControlsPc(), this::setTouchControlsPc);
        touchControls.add(touch);
        target.addView(row("Touch controls the PC",
                ui.text("Fingers and pen on the picture. Turn off if the touch screen misbehaves.", 12, false,
                        ui.muted), touch, false, stacked));
    }

    /** Puts both copies of every setting on the stored value. */
    private void syncSettings() {
        for (Ui.Segmented c : resolutionControls) c.setSelected(Prefs.RES_SMALLER.equals(prefs.resolution()) ? 1 : 0);
        for (Ui.Segmented c : fpsControls) c.setSelected(prefs.maxFps() == 30 ? 1 : 0);
        for (Ui.Segmented c : orientationControls) c.setSelected(Prefs.ORIENT_PORTRAIT.equals(prefs.orientation()) ? 1 : 0);
        for (Ui.Toggle c : keepOnControls) c.setOn(prefs.keepScreenOn());
        for (Ui.Toggle c : statsControls) c.setOn(prefs.showStats());
        for (Ui.Toggle c : touchControls) c.setOn(prefs.touchControlsPc());
        refreshTexts();
    }

    private View row(String label, TextView desc, View control, boolean stacked) {
        return row(label, desc, control, stacked, stacked);
    }

    /** flush: no side padding (the menu has its own). */
    private View row(String label, TextView desc, View control, boolean stacked, boolean flush) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setMinimumHeight(ui.dp(64));
        int side = flush ? 0 : ui.dp(16);
        row.setPadding(side, ui.dp(8), side, ui.dp(8));
        LinearLayout texts = column();
        texts.addView(ui.text(label, 15, true, ui.text));
        desc.setPadding(0, ui.dp(2), ui.dp(12), 0);
        texts.addView(desc);
        if (stacked) {
            row.setOrientation(LinearLayout.VERTICAL);
            row.setGravity(Gravity.LEFT);
            row.setPadding(0, ui.dp(12), 0, ui.dp(12));
            row.addView(texts);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = ui.dp(8);
            row.addView(control, lp);
        } else {
            row.addView(texts, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            row.addView(control);
        }
        return row;
    }

    // ------------------------------------------------------------ streaming overlay

    private View buildOverlay() {
        overlay = new Overlay();
        overlay.setOnClickListener(v -> closeSheet());
        overlay.setClickable(false);

        stats = ui.text("", 12, true, Color.WHITE);
        stats.setPadding(ui.dp(10), ui.dp(6), ui.dp(10), ui.dp(6));
        Ui.bg(stats, ui.rounded(0xD10A0A0A, 6, 0));
        FrameLayout.LayoutParams slp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.RIGHT);
        slp.setMargins(0, ui.dp(12), ui.dp(12), 0);
        overlay.addView(stats, slp);

        // a thin handle on the left edge opens the menu; it reacts well beyond the
        // bar so a finger finds it on small, low-density tablets
        FrameLayout handle = new FrameLayout(this);
        handle.setContentDescription("Open menu");
        View bar = new View(this);
        Ui.bg(bar, ui.rounded(0x8CFFFFFF, 3, 0));
        handle.addView(bar, new FrameLayout.LayoutParams(ui.dp(6), ui.dp(72), Gravity.CENTER_VERTICAL | Gravity.LEFT));
        handle.setOnClickListener(v -> openSheet());
        overlay.addView(handle, new FrameLayout.LayoutParams(ui.dp(56), ui.dp(220), Gravity.CENTER_VERTICAL | Gravity.LEFT));

        sheet = new ScrollView(this);
        sheet.setBackgroundColor(ui.surface);
        sheet.setClickable(true);
        if (Build.VERSION.SDK_INT >= 21) sheet.setElevation(ui.dp(16));
        LinearLayout content = column();
        content.setPadding(ui.dp(20), ui.dp(20), ui.dp(20), ui.dp(20));
        sheet.addView(content);
        sheetTitle = ui.text("", 17, true, ui.text);
        content.addView(sheetTitle);
        sheetSub = ui.text("", 15, false, ui.muted);
        sheetSub.setPadding(0, ui.dp(4), 0, ui.dp(12));
        content.addView(sheetSub);
        content.addView(ui.rule());
        addSettings(content, true);
        content.addView(ui.rule());
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.topMargin = ui.dp(16);
        content.addView(ui.button("Disconnect", true, v -> disconnect()), blp);
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        rlp.topMargin = ui.dp(8);
        content.addView(ui.button("Reconnect", false, v -> reconnect()), rlp);
        LinearLayout.LayoutParams blp2 = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        blp2.topMargin = ui.dp(8);
        content.addView(ui.button("Back to the picture", false, v -> closeSheet()), blp2);
        overlay.addView(sheet, new FrameLayout.LayoutParams(ui.dp(320), ViewGroup.LayoutParams.MATCH_PARENT,
                Gravity.LEFT));
        sheet.setTranslationX(-ui.dp(340));
        return overlay;
    }

    private boolean sheetOpen() {
        return overlay.isClickable();
    }

    /**
     * Over the video SurfaceView, Android composes only the parts of this
     * window that were opaque at the last layout. The menu slides in without a
     * layout, so it was never shown: while it is out, it claims its area here,
     * and opening and closing ask for a layout so Android looks again.
     */
    private final class Overlay extends FrameLayout {
        private final int[] location = new int[2];

        Overlay() {
            super(MainActivity.this);
        }

        @Override
        public boolean gatherTransparentRegion(Region region) {
            boolean opaque = super.gatherTransparentRegion(region);
            if (region != null && sheetShown) {
                getLocationInWindow(location);
                region.op(location[0], location[1], location[0] + sheet.getWidth(), location[1] + getHeight(),
                        Region.Op.DIFFERENCE);
            }
            return opaque;
        }
    }

    private void openSheet() {
        send(new byte[] {(byte) TouchInput.CANCELED, 0}); // nothing stays pressed on the PC behind the menu
        refreshTexts();
        overlay.setClickable(true);
        sheetShown = true;
        overlay.requestLayout();
        sheet.animate().translationX(0).setDuration(180).withEndAction(null).start();
    }

    private void closeSheet() {
        overlay.setClickable(false);
        sheet.animate().translationX(-ui.dp(340)).setDuration(180).withEndAction(this::sheetHidden).start();
    }

    private void closeSheetNow() {
        overlay.setClickable(false);
        sheet.animate().cancel();
        sheet.setTranslationX(-ui.dp(340));
        sheetHidden();
    }

    private void sheetHidden() {
        if (overlay.isClickable()) return; // opened again meanwhile
        sheetShown = false;
        overlay.requestLayout();
    }

    // ------------------------------------------------------------ actions

    /** Connect from the tablet: the PC picks it up on its next look (about every second). */
    private void onHomeAction() {
        if (mode == Mode.PAUSED) {
            mode = Mode.WAITING;
            if (server == null) startServer();
        } else if (server != null) {
            server.requestConnect();
        }
        Toast.makeText(this, "Asked the PC to connect. ScreenFleet must be running on it.", Toast.LENGTH_SHORT).show();
        render();
    }

    /** A fresh session with the same PC, for a stuck or garbled picture. */
    private void reconnect() {
        if (server == null) return;
        server.requestConnect();
        reconnectingUntil = SystemClock.uptimeMillis() + RECONNECT_GRACE_MS;
        server.restartSession();
        refreshTexts();
    }

    /** Ends the stream; the PC waits until this tablet is ready again. */
    private void disconnect() {
        closeSheetNow();
        mode = Mode.PAUSED;
        stopServer();
        render();
    }

    private void reconnectForNewSettings() {
        if (server == null) return;
        if (mode == Mode.STREAMING) reconnectingUntil = SystemClock.uptimeMillis() + RECONNECT_GRACE_MS;
        server.restartSession();
    }

    private void setShowStats(boolean on) {
        prefs.setShowStats(on);
        syncSettings();
        render();
    }

    private void setTouchControlsPc(boolean on) {
        // let go of anything still pressed on the PC
        if (!on) send(new byte[] {(byte) TouchInput.CANCELED, 0});
        prefs.setTouchControlsPc(on);
        syncSettings();
    }

    /** Touches on the picture go to the PC: streaming, the menu closed, and allowed here. */
    private boolean touchActive() {
        return mode == Mode.STREAMING && !sheetOpen() && prefs.touchControlsPc();
    }

    private void send(byte[] snapshot) {
        if (snapshot != null && server != null) server.sendInput(snapshot);
    }

    private boolean hasTouchScreen() {
        return getPackageManager().hasSystemFeature(PackageManager.FEATURE_TOUCHSCREEN);
    }

    private void applyKeepScreenOn() {
        if (prefs.keepScreenOn()) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    private void applyOrientation() {
        setRequestedOrientation(Prefs.ORIENT_PORTRAIT.equals(prefs.orientation())
                ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
                : ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    // ------------------------------------------------------------ rendering state

    private void render() {
        boolean streaming = mode == Mode.STREAMING;
        home.setVisibility(streaming ? View.GONE : View.VISIBLE);
        overlay.setVisibility(streaming ? View.VISIBLE : View.GONE);
        stats.setVisibility(prefs.showStats() ? View.VISIBLE : View.GONE);
        refreshTexts();
    }

    private void refreshTexts() {
        switch (mode) {
            case PAUSED:
                headline.setText("Disconnected");
                sub.setText("The PC won’t reconnect until you connect from here.");
                homeAction.setText("Connect");
                homeAction.setVisibility(View.VISIBLE);
                break;
            default:
                headline.setText("Waiting for your PC");
                sub.setText("Start ScreenFleet on the PC. This tablet becomes its extra monitor.");
                homeAction.setText("Connect");
                homeAction.setVisibility(View.VISIBLE);
                break;
        }

        wifiCard.removeAllViews();
        if (Build.VERSION.SDK_INT >= 21) {
            String ip = wifiAddress();
            wifiCard.addView(titleRow(WIFI, ui.pill(ip != null ? "Ready" : "Not connected", ui.pillNeutralBg,
                    ui.pillNeutralInk)));
            if (ip != null) {
                TextView addr = ui.text(ip, 30, false, ui.text);
                addr.setPadding(0, ui.dp(8), 0, 0);
                wifiCard.addView(addr);
                TextView how = ui.text("On the PC it shows under Tablets, Network: press Pair there.",
                        15, false, ui.muted);
                how.setPadding(0, ui.dp(6), 0, 0);
                wifiCard.addView(how);
            } else {
                TextView how = ui.text("Join the same " + WIFI + " network as the PC to connect without a cable.", 15,
                        false, ui.muted);
                how.setPadding(0, ui.dp(6), 0, 0);
                wifiCard.addView(how);
            }
        } else {
            wifiCard.addView(titleRow(WIFI, ui.pill("Needs Android 5", ui.pillNeutralBg, ui.pillNeutralInk)));
            TextView how = ui.text("This tablet runs Android " + Build.VERSION.RELEASE
                    + ". Use the USB cable; it is also the faster link.", 15, false, ui.muted);
            how.setPadding(0, ui.dp(6), 0, 0);
            wifiCard.addView(how);
        }

        Protocol.DeviceInfo native_ = nativeSize();
        boolean smaller = Prefs.RES_SMALLER.equals(prefs.resolution());
        String resText = smaller
                ? (native_.width * 3 / 4 & ~1) + " × " + (native_.height * 3 / 4 & ~1) + ", lighter for older tablets"
                : native_.width + " × " + native_.height + ", the sharpest";
        for (TextView d : resolutionDescs) d.setText(resText);

        if (pairedText != null) {
            java.util.List<String> paired = prefs.pairedHosts();
            pairedText.setText(paired.isEmpty() ? "None yet. Pair from the PC to connect over Wi‑Fi."
                    : android.text.TextUtils.join(", ", paired) + ". Wi‑Fi needs pairing; USB doesn't.");
        }
        String last = prefs.lastSession();
        lastText.setText(last != null ? last : "No PC yet");

        foot.setText("ScreenFleet 1.0.0" + DOT + deviceName() + DOT + "Android " + Build.VERSION.RELEASE);

        if (session != null) {
            sheetTitle.setText(session.host.isEmpty() ? "Connected" : "Connected to " + session.host);
            sheetSub.setText(session.link + DOT + "since "
                    + new SimpleDateFormat("HH:mm", Locale.getDefault()).format(new Date(session.startedAt)));
        } else if (mode == Mode.STREAMING) {
            sheetSub.setText("Reconnecting…");
        }
    }

    private Protocol.DeviceInfo nativeSize() {
        DisplayMetrics metrics = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
        boolean portrait = Prefs.ORIENT_PORTRAIT.equals(prefs.orientation());
        Protocol.DeviceInfo d = new Protocol.DeviceInfo();
        int longSide = Math.max(metrics.widthPixels, metrics.heightPixels);
        int shortSide = Math.min(metrics.widthPixels, metrics.heightPixels);
        int[] size = streamSize(portrait ? shortSide : longSide, portrait ? longSide : shortSide);
        d.width = size[0];
        d.height = size[1];
        return d;
    }

    private static String wifiAddress() {
        try {
            for (NetworkInterface nif : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!nif.isUp() || nif.isLoopback() || !nif.getName().startsWith("wlan")) continue;
                for (InetAddress addr : Collections.list(nif.getInetAddresses())) {
                    if (addr instanceof Inet4Address && !addr.isLoopbackAddress()) return addr.getHostAddress();
                }
            }
        } catch (Exception ignored) {
        }
        return null;
    }

    private static FrameLayout.LayoutParams match() {
        return new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
    }
}
