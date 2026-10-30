package app.screenfleet;

import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * The app's look (the same as the PC app's theme.h): colour tokens, Geist, and
 * the few controls the app needs, built from plain views so they look the same
 * on Android 4.4 and 12.
 */
final class Ui {
    final Context ctx;
    final boolean dark;
    final float density;
    final Typeface regular;
    final Typeface medium;

    final int ground, surface, raised, border, rule, control;
    final int text, muted, primary, primaryInk, secondary;
    final int positive, pillPositiveBg, pillPositiveInk, pillNeutralBg, pillNeutralInk;
    final int selectedSegment;

    Ui(Context ctx) {
        this.ctx = ctx;
        density = ctx.getResources().getDisplayMetrics().density;
        int night = ctx.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK;
        dark = night == Configuration.UI_MODE_NIGHT_YES;
        regular = load("fonts/Geist-Regular.ttf", Typeface.DEFAULT);
        medium = load("fonts/Geist-Medium.ttf", Typeface.DEFAULT_BOLD);
        if (dark) {
            ground = 0xff111111; surface = 0xff1a1a1a; raised = 0xff262626; border = 0xff2e2e2e;
            rule = 0xff242424; control = 0xff4c4c4c; text = 0xffffffff; muted = 0xffb4b4b4;
            primary = 0xffffffff; primaryInk = 0xff0a0a0a; secondary = 0xff2e2e2e;
            positive = 0xff3fc07a; pillPositiveBg = 0xff172a1d; pillPositiveInk = 0xff3fc07a;
            pillNeutralBg = 0xff262626; pillNeutralInk = 0xffd0d0d0; selectedSegment = 0xff3a3a3a;
        } else {
            ground = 0xfff2f4f7; surface = 0xffffffff; raised = 0xffeeeeee; border = 0xffe6e6e6;
            rule = 0xfff0f0f0; control = 0xffd4d4d4; text = 0xff0a0a0a; muted = 0xff5f6368;
            primary = 0xff141414; primaryInk = 0xffffffff; secondary = 0xffe5e5e5;
            positive = 0xff247b50; pillPositiveBg = 0xffdcfae5; pillPositiveInk = 0xff04250e;
            pillNeutralBg = 0xffeeeeee; pillNeutralInk = 0xff3d3d3d; selectedSegment = 0xffffffff;
        }
    }

    private Typeface load(String path, Typeface fallback) {
        try {
            return Typeface.createFromAsset(ctx.getAssets(), path);
        } catch (RuntimeException e) {
            return fallback;
        }
    }

    int dp(float v) {
        return Math.round(v * density);
    }

    TextView text(CharSequence s, float sp, boolean bold, int color) {
        TextView t = new TextView(ctx);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTypeface(bold ? medium : regular);
        t.setTextColor(color);
        t.setIncludeFontPadding(false);
        t.setLineSpacing(sp * 0.35f * density, 1f);
        return t;
    }

    GradientDrawable rounded(int color, float radiusDp, int strokeColor) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radiusDp));
        if (strokeColor != 0) d.setStroke(Math.max(1, dp(1)), strokeColor);
        return d;
    }

    /** A white card with a 1px edge, radius 8. */
    LinearLayout card() {
        LinearLayout c = new LinearLayout(ctx);
        c.setOrientation(LinearLayout.VERTICAL);
        bg(c, rounded(surface, 8, border));
        return c;
    }

    TextView pill(String s, int bg, int ink) {
        TextView t = text(s, 12, true, ink);
        t.setGravity(Gravity.CENTER);
        t.setMaxLines(1);
        t.setPadding(dp(10), dp(3), dp(10), dp(3));
        fitWidth(t);
        bg(t, rounded(bg, 11, 0));
        return t;
    }

    /**
     * Short labels in Geist Medium came out a few pixels narrower than they
     * draw, clipping the last letter ("Ready", "60 fps"); size them from the
     * paint's own measurement with a little slack.
     */
    void fitWidth(TextView t) {
        float w = t.getPaint().measureText(t.getText().toString());
        t.setMinWidth((int) Math.ceil(w) + t.getPaddingLeft() + t.getPaddingRight() + dp(3));
    }

    View rule() {
        View v = new View(ctx);
        v.setBackgroundColor(rule);
        v.setLayoutParams(new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, Math.max(1, dp(1))));
        return v;
    }

    TextView button(String s, boolean primaryStyle, View.OnClickListener click) {
        TextView b = text(s, 15, true, primaryStyle ? primaryInk : text);
        b.setGravity(Gravity.CENTER);
        b.setMinHeight(dp(44));
        b.setPadding(dp(16), 0, dp(16), 0);
        bg(b, rounded(primaryStyle ? primary : secondary, 6, 0));
        b.setClickable(true);
        b.setFocusable(true);
        b.setOnClickListener(click);
        return b;
    }

    @SuppressWarnings("deprecation")
    static void bg(View v, android.graphics.drawable.Drawable d) {
        v.setBackgroundDrawable(d); // setBackground() is API 16+, this works everywhere
    }

    /** A row of options, one selected (the mockup's segmented control). */
    static final class Segmented extends LinearLayout {
        interface Listener {
            void onSelect(int index);
        }

        private final Ui ui;
        private final TextView[] items;
        private int selected;

        Segmented(Ui ui, String[] options, int selected, Listener listener) {
            super(ui.ctx);
            this.ui = ui;
            this.selected = selected;
            setOrientation(HORIZONTAL);
            setPadding(ui.dp(2), ui.dp(2), ui.dp(2), ui.dp(2));
            bg(this, ui.rounded(ui.raised, 6, 0));
            items = new TextView[options.length];
            for (int i = 0; i < options.length; ++i) {
                final int index = i;
                TextView t = ui.text(options[i], 15, false, ui.muted);
                t.setGravity(Gravity.CENTER);
                t.setMinHeight(ui.dp(36));
                t.setPadding(ui.dp(14), 0, ui.dp(14), 0);
                t.setClickable(true);
                t.setFocusable(true);
                t.setOnClickListener(v -> {
                    setSelected(index);
                    listener.onSelect(index);
                });
                LayoutParams lp = new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
                if (i > 0) lp.leftMargin = ui.dp(2);
                addView(t, lp);
                items[i] = t;
            }
            refresh();
        }

        void setSelected(int index) {
            selected = index;
            refresh();
        }

        private void refresh() {
            for (int i = 0; i < items.length; ++i) {
                boolean on = i == selected;
                items[i].setTextColor(on ? ui.text : ui.muted);
                items[i].setTypeface(on ? ui.medium : ui.regular);
                ui.fitWidth(items[i]);
                bg(items[i], on ? ui.rounded(ui.selectedSegment, 5, 0) : null);
            }
        }
    }

    /** An on/off switch drawn the same on every Android version. */
    static final class Toggle extends View {
        interface Listener {
            void onChange(boolean on);
        }

        private final Ui ui;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF track = new RectF();
        private boolean on;

        Toggle(Ui ui, boolean on, Listener listener) {
            super(ui.ctx);
            this.ui = ui;
            this.on = on;
            setClickable(true);
            setFocusable(true);
            setOnClickListener(v -> {
                setOn(!this.on);
                listener.onChange(this.on);
            });
            setMinimumWidth(ui.dp(48));
            setMinimumHeight(ui.dp(44));
        }

        void setOn(boolean value) {
            on = value;
            setContentDescription(on ? "On" : "Off");
            invalidate();
        }

        @Override
        protected void onMeasure(int w, int h) {
            setMeasuredDimension(ui.dp(48), ui.dp(44));
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = ui.dp(48), h = ui.dp(26), top = (getHeight() - h) / 2f;
            track.set(0, top, w, top + h);
            paint.setColor(on ? ui.primary : ui.control);
            c.drawRoundRect(track, h / 2, h / 2, paint);
            float r = ui.dp(10);
            float cx = on ? w - ui.dp(13) : ui.dp(13);
            paint.setColor(on ? ui.primaryInk : Color.WHITE);
            c.drawCircle(cx, top + h / 2, r, paint);
        }
    }

    /** The product mark: a monitor with the tablet beside it. */
    static final class Mark extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF r = new RectF();
        private final Ui ui;

        Mark(Ui ui, int color) {
            super(ui.ctx);
            this.ui = ui;
            paint.setColor(color);
        }

        @Override
        protected void onMeasure(int w, int h) {
            setMeasuredDimension(ui.dp(22), ui.dp(16));
        }

        @Override
        protected void onDraw(Canvas c) {
            float u = getWidth() / 22f; // the 22 x 16 design grid
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(1.6f * u);
            r.set(1 * u, 1.5f * u, 14 * u, 11.5f * u);
            c.drawRoundRect(r, 1.5f * u, 1.5f * u, paint);
            paint.setStrokeCap(Paint.Cap.ROUND);
            c.drawLine(5.5f * u, 14.5f * u, 9.5f * u, 14.5f * u, paint);
            paint.setStyle(Paint.Style.FILL);
            r.set(16 * u, 5.5f * u, 21 * u, 12.5f * u);
            c.drawRoundRect(r, u, u, paint);
        }
    }
}
