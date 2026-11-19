package io.github.andrythebeast.songate;

import android.content.Context;
import android.content.Intent;
import android.database.ContentObserver;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * SongateOS's system settings (session 21, phase 1; SONGATEOS_SYSTEM_API.md): the HiBy R4 service does the work (Wi-Fi, Bluetooth,
 * the audio output, power, updates), Songate draws the pages. One call shape for everything: a method, its arg, its extras as
 * "key=value" lines; the answer is the service's Bundle as JSON (lists and nested Bundles too), read by the app (source/sysapi.cpp).
 * Every call is a binder call: the app makes them on its own worker thread, never the screen's.
 * Passwords pass through once (wifi_connect's extras) and are never logged here: only the method's name goes to the log.
 */
final class SysApi {
    private static final String AUTHORITY = "com.andre.hibyr4.settings";
    private static final Uri URI = Uri.parse("content://" + AUTHORITY);

    private SysApi() {}

    static native void nativeSysChanged(String area);

    /** The answer as JSON; on no answer at all: {"ok":false,"error":"no_answer: …"}. */
    static String call(String method, String arg, String extras) {
        final Context c = HibyR4.context;
        if (c == null) return "{\"ok\":false,\"error\":\"no_answer: not started\"}";
        final Bundle ex = new Bundle();
        if (extras != null && !extras.isEmpty())
            for (String line : extras.split("\n")) {
                final int eq = line.indexOf('=');
                if (eq > 0) ex.putString(line.substring(0, eq), line.substring(eq + 1));
            }
        try {
            final Bundle b = c.getContentResolver().call(URI, method, arg == null || arg.isEmpty() ? null : arg, ex);
            if (b == null) return "{\"ok\":false,\"error\":\"no_answer: the service did not answer\"}";
            final StringBuilder out = new StringBuilder();
            json(out, b);
            return out.toString();
        } catch (SecurityException e) {
            Log.w("Songate", "system " + method + ": no permission");
            return "{\"ok\":false,\"error\":\"no_permission: Songate may not use this\"}";
        } catch (Throwable t) {
            Log.w("Songate", "system " + method + ": " + t.getClass().getSimpleName());
            return "{\"ok\":false,\"error\":\"no_answer: " + esc(String.valueOf(t.getMessage())) + "\"}";
        }
    }

    private static String esc(String s) {
        final StringBuilder o = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            final char ch = s.charAt(i);
            if (ch == '"' || ch == '\\') o.append('\\').append(ch);
            else if (ch < 0x20) o.append(String.format("\\u%04x", (int) ch));
            else o.append(ch);
        }
        return o.toString();
    }

    private static void json(StringBuilder out, Object v) {
        if (v == null) { out.append("null"); return; }
        if (v instanceof Bundle) {
            final Bundle b = (Bundle) v;
            out.append('{');
            boolean first = true;
            for (String k : b.keySet()) {
                if (!first) out.append(',');
                first = false;
                out.append('"').append(esc(k)).append("\":");
                json(out, b.get(k));
            }
            out.append('}');
        } else if (v instanceof Boolean) {
            out.append(((Boolean) v) ? "true" : "false");
        } else if (v instanceof Number) {
            out.append(v.toString());
        } else if (v instanceof List) {
            out.append('[');
            boolean first = true;
            for (Object o : (List<?>) v) { if (!first) out.append(','); first = false; json(out, o); }
            out.append(']');
        } else if (v instanceof String[]) {
            out.append('[');
            final String[] a = (String[]) v;
            for (int i = 0; i < a.length; i++) { if (i > 0) out.append(','); json(out, a[i]); }
            out.append(']');
        } else if (v instanceof int[]) {
            out.append('[');
            final int[] a = (int[]) v;
            for (int i = 0; i < a.length; i++) { if (i > 0) out.append(','); out.append(a[i]); }
            out.append(']');
        } else if (v instanceof Bundle[]) {
            out.append('[');
            final Bundle[] a = (Bundle[]) v;
            for (int i = 0; i < a.length; i++) { if (i > 0) out.append(','); json(out, a[i]); }
            out.append(']');
        } else {
            out.append('"').append(esc(String.valueOf(v))).append('"');
        }
    }

    // ---- change notifications: a ContentObserver on …/system/<area> while a page (or the volume pop-up) shows it
    private static final Map<String, ContentObserver> watches = new HashMap<>();

    static synchronized void watch(final String area, boolean on) {
        final Context c = HibyR4.context;
        if (c == null) return;
        try {
            if (on && !watches.containsKey(area)) {
                final ContentObserver o = new ContentObserver(new Handler(Looper.getMainLooper())) {
                    @Override
                    public void onChange(boolean selfChange) {
                        try { nativeSysChanged(area); } catch (UnsatisfiedLinkError ignored) {}
                    }
                };
                c.getContentResolver().registerContentObserver(Uri.parse("content://" + AUTHORITY + "/system/" + area), true, o);
                watches.put(area, o);
            } else if (!on && watches.containsKey(area)) {
                c.getContentResolver().unregisterContentObserver(watches.remove(area));
            }
        } catch (Throwable t) {
            Log.w("Songate", "system watch " + area + ": " + t);
        }
    }

    /** Android's own page, where Songate's cannot be shown (the service is older, or refuses): wifi, bluetooth, settings. */
    static void openAndroid(String which) {
        final SongateActivity a = SongateActivity.self;
        if (a == null) return;
        final String action = "wifi".equals(which) ? Settings.ACTION_WIFI_SETTINGS
                            : "bluetooth".equals(which) ? Settings.ACTION_BLUETOOTH_SETTINGS
                                                        : Settings.ACTION_SETTINGS;
        a.runOnUiThread(() -> {
            try {
                a.startActivity(new Intent(action).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            } catch (Throwable t) {
                Log.w("Songate", "open " + which + ": " + t);
            }
        });
    }
}
