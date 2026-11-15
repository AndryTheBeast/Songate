/*
 * Songate · HibyR4.java
 * The HiBy R4's settings, on the user's own ROM: its service (com.andre.hibyr4.service) answers through a ContentProvider, the
 * same path as the ROM's own Settings pages (docs: the ROM's "HiBy R4 settings API"). Called by source/hibyr4.cpp's worker
 * thread through platform_android.cpp, never on the screen's thread: each call is IPC.
 * On any other device the provider is not there: state() says 0 and nothing else is ever called.
 */
package io.github.andrythebeast.songate;

import android.content.Context;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;

final class HibyR4 {
    static final String AUTHORITY = "com.andre.hibyr4.settings";
    static final String PERMISSION = "com.andre.hibyr4.permission.HIBY_SETTINGS";
    private static final Uri URI = Uri.parse("content://" + AUTHORITY);

    static volatile Context context;   // the application's (SongateActivity.onCreate): outlives a closed screen while music plays

    private HibyR4() {}

    /** The HiBy R4 with SongateOS (its settings provider, its boot logo and boot animation ship together): found once in
     *  SongateActivity.onCreate, before the app's first frame, so Songate's own opening is never played after the boot animation.
     *  Permission is not needed for this; anything that fails means "not the R4" (the opening plays, the safe side). */
    static volatile boolean device = false;
    static void detect(Context c) {
        try {
            device = c.getPackageManager().resolveContentProvider(AUTHORITY, 0) != null;
        } catch (Throwable t) {
            device = false;
        }
    }

    /** 0 not a HiBy R4 with the service; 1 the service is there but Songate was installed before it (no permission: reinstall);
     *  2 ready. */
    static int state() {
        final Context c = context;
        if (c == null) return 0;
        try {
            final ProviderInfo p = c.getPackageManager().resolveContentProvider(AUTHORITY, 0);
            if (p == null) return 0;
            return c.checkSelfPermission(PERMISSION) == PackageManager.PERMISSION_GRANTED ? 2 : 1;
        } catch (Throwable t) {
            return 0;
        }
    }

    private static Bundle call(String method, String arg, Bundle extras) {
        final Context c = context;
        if (c == null) return null;
        try {
            return c.getContentResolver().call(URI, method, arg, extras);
        } catch (Throwable t) {
            Log.w("Songate", "HiBy R4 " + method + (arg != null ? " " + arg : "") + ": " + t);
            return null;
        }
    }

    /** Every setting and status key, "key=value" a line (the EQ as "eq=…"); "" when the service did not answer. */
    static String get() {
        final Bundle b = call("get", null, null);
        if (b == null) return "";
        final StringBuilder out = new StringBuilder();
        for (String k : b.keySet()) {
            final Object v = b.get(k);
            if (v == null) continue;
            out.append(k).append('=').append(String.valueOf(v).replace('\n', ' ')).append('\n');
        }
        return out.toString();
    }

    /** "" accepted; otherwise the service's own reason (or "No answer"). */
    static String set(String key, String value) {
        final Bundle extras = new Bundle();
        extras.putString("value", value);
        final Bundle b = call("set", key, extras);
        if (b == null) return "No answer from the HiBy service";
        if (b.getBoolean("ok", false)) return "";
        final String e = b.getString("error");
        return e != null && !e.isEmpty() ? e : "Not accepted";
    }

    /** "on,autoPreamp,preamp,preset,g0,…,g9". */
    static boolean eqSet(String eq) {
        final Bundle extras = new Bundle();
        extras.putString("eq", eq);
        final Bundle b = call("eq_set", null, extras);
        return b != null && b.getBoolean("ok", false);
    }

    /** The custom colour shown at once, nothing saved (only while st_led is custom). */
    static boolean ledLive(int rgb, int bright, boolean breath) {
        final Bundle extras = new Bundle();
        extras.putString("color", String.format("%06x", rgb & 0xffffff));
        extras.putInt("bright", Math.max(0, Math.min(100, bright)));
        extras.putString("effect", breath ? "breath" : "solid");
        final Bundle b = call("led_live", null, extras);
        return b != null && b.getBoolean("ok", false);
    }
}
