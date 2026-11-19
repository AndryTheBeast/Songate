package io.github.andrythebeast.songate;

import android.app.NotificationManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.media.MediaMetadata;
import android.app.SearchManager;
import android.media.Rating;
import android.os.Bundle;
import android.provider.MediaStore;
import android.media.session.MediaController;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.Settings;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.util.List;

/**
 * Other apps' music in Songate (Settings › Playback › Other apps; the user, 2026-11-18): Android's own media sessions, seen through
 * Songate's notification listener (MediaListener) once Notification access allows it. One session is followed: the one playing, else
 * the latest. What it says (its app, song, cover, where it is, which buttons it has) is read by the app (platform_android.cpp), which
 * is told of each change (nativeOtherChanged); its buttons are pressed through its own controls. Songate's own session is left out.
 */
final class OtherApps {
    private OtherApps() {}

    private static native void nativeOtherChanged();

    private static final Handler main = new Handler(Looper.getMainLooper());
    private static Context ctx;
    private static MediaSessionManager msm;
    private static MediaController ctl;
    private static boolean watching;
    private static volatile int artGen;   // one more each time the song's picture may have changed

    private static ComponentName listener(Context c) { return new ComponentName(c, MediaListener.class); }

    static boolean allowed(Context c) {
        try {
            final NotificationManager nm = c.getSystemService(NotificationManager.class);
            return nm != null && nm.isNotificationListenerAccessGranted(listener(c));
        } catch (Throwable t) { return false; }
    }

    /** Android's own page for Songate's Notification access (that one switch where Android has it, else the list) */
    static void openAccess(Context c) {
        try {
            final Intent i = new Intent(Settings.ACTION_NOTIFICATION_LISTENER_DETAIL_SETTINGS)
                .putExtra(Settings.EXTRA_NOTIFICATION_LISTENER_COMPONENT_NAME, listener(c).flattenToString()).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            c.startActivity(i);
        } catch (Throwable t) {
            try { c.startActivity(new Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)); }
            catch (Throwable ignored) {}
        }
    }

    private static void changed() { try { nativeOtherChanged(); } catch (UnsatisfiedLinkError ignored) {} }

    private static final MediaController.Callback cb = new MediaController.Callback() {
        @Override public void onMetadataChanged(MediaMetadata m) { artGen++; changed(); }
        @Override public void onPlaybackStateChanged(PlaybackState s) { pick(); changed(); }
        @Override public void onSessionDestroyed() { main.post(() -> { pick(); changed(); }); }
    };

    private static final MediaSessionManager.OnActiveSessionsChangedListener sessions = list -> { pick(); changed(); };

    /** The session to follow: one playing (not Songate's), else the first that is not Songate's */
    private static synchronized void pick() {
        if (msm == null || ctx == null) return;
        List<MediaController> list;
        try { list = msm.getActiveSessions(listener(ctx)); }
        catch (Throwable t) { list = null; Log.w("Songate", "other apps: sessions not readable: " + t); }   // (not allowed: none)
        MediaController best = null;
        if (list != null) {
            for (MediaController c : list) {
                if (ctx.getPackageName().equals(c.getPackageName())) continue;
                final PlaybackState s = c.getPlaybackState();
                if (s != null && s.getState() == PlaybackState.STATE_PLAYING) { best = c; break; }
                if (best == null) best = c;
            }
        }
        final boolean same = best != null && ctl != null && best.getSessionToken().equals(ctl.getSessionToken());
        if (same) return;
        if (ctl != null) try { ctl.unregisterCallback(cb); } catch (Throwable ignored) {}
        Log.i("Songate", "other apps: " + (list == null ? "no list" : list.size() + " session(s)") + ", following "
            + (best == null ? "none" : best.getPackageName() + " (state " + (best.getPlaybackState() == null ? "none" : best.getPlaybackState().getState()) + ")"));
        ctl = best;
        artGen++;
        if (ctl != null) try { ctl.registerCallback(cb, main); } catch (Throwable ignored) {}
    }

    /** Followed while Settings' switch is on (and Songate runs) */
    static void watch(final Context c, final boolean on) {
        main.post(() -> {
            synchronized (OtherApps.class) {
                if (on == watching) { if (on) pick(); return; }
                watching = on;
                ctx = c.getApplicationContext();
                Log.i("Songate", "other apps: watching " + (on ? "on" : "off") + ", access " + (allowed(ctx) ? "allowed" : "not allowed"));
                if (msm == null) msm = ctx.getSystemService(MediaSessionManager.class);
                if (msm == null) return;
                if (on) {
                    try { msm.addOnActiveSessionsChangedListener(sessions, listener(ctx), main); } catch (Throwable t) { Log.w("Songate", "other apps: " + t); }
                    pick();
                } else {
                    try { msm.removeOnActiveSessionsChangedListener(sessions); } catch (Throwable ignored) {}
                    if (ctl != null) try { ctl.unregisterCallback(cb); } catch (Throwable ignored) {}
                    ctl = null;
                }
            }
            changed();
        });
    }

    private static String clean(CharSequence s) { return s == null ? "" : s.toString().replace('\n', ' ').replace('\r', ' '); }

    /** The app's own like, where it has one (the user, 2026-11-19: Songate's heart presses it): a heart rating (ACTION_SET_RATING,
     *  RATING_HEART), else one of its own buttons (custom actions, its notification's) whose name or action says so: "Add to Liked
     *  Songs", "Like", "Favourite", "Save"... Liked: the rating's heart, or that button saying it would take it away ("Remove...", "Un..."). */
    private static final class Like { PlaybackState.CustomAction custom; boolean liked; }

    private static Like like(MediaController c, PlaybackState s) {
        if (s == null) return null;
        try {
            if ((s.getActions() & PlaybackState.ACTION_SET_RATING) != 0 && c.getRatingType() == Rating.RATING_HEART) {
                final Like l = new Like();
                final MediaMetadata m = c.getMetadata();
                final Rating r = m == null ? null : m.getRating(MediaMetadata.METADATA_KEY_USER_RATING);
                l.liked = r != null && r.isRated() && r.hasHeart();
                return l;
            }
        } catch (Throwable ignored) {}
        final List<PlaybackState.CustomAction> list = s.getCustomActions();
        if (list == null) return null;
        for (PlaybackState.CustomAction a : list) {
            final String n = (clean(a.getName()) + " " + a.getAction()).toLowerCase(java.util.Locale.ROOT);
            if (n.contains("dislike") || n.contains("thumbs down") || n.contains("thumb_down") || n.contains("thumbs_down")) continue;
            final boolean is = n.contains("like") || n.contains("love") || n.contains("heart") || n.contains("favorite") || n.contains("favourite")
                || n.contains("save") || n.contains("collection") || n.contains("thumbs up") || n.contains("thumb_up") || n.contains("thumbs_up");
            if (!is) continue;
            final String name = clean(a.getName()).toLowerCase(java.util.Locale.ROOT).trim();
            final String act = a.getAction().toLowerCase(java.util.Locale.ROOT);
            final Like l = new Like();
            l.custom = a;
            l.liked = name.startsWith("remove") || name.contains("remove from") || name.startsWith("un") || name.equals("liked")
                || name.equals("saved") || name.equals("loved") || name.equals("favorited") || name.equals("favourited")
                || act.contains("remove") || act.contains("unlike") || act.contains("unsave") || act.contains("unfav");
            return l;
        }
        return null;
    }

    /** What plays, as "key=value" lines (none: empty): pkg, label, title, artist, album, duration (ms), pos (ms, now), playing, actions,
     *  like (1: the app has its own, Songate's heart presses it) and liked, art (its picture's generation) */
    static synchronized String state() {
        final MediaController c = ctl;
        if (c == null) return "";
        final StringBuilder o = new StringBuilder();
        final String pkg = c.getPackageName();
        String label = pkg;
        try {
            final PackageManager pm = ctx.getPackageManager();
            label = pm.getApplicationLabel(pm.getApplicationInfo(pkg, 0)).toString();
        } catch (Throwable ignored) {}
        o.append("pkg=").append(pkg).append('\n').append("label=").append(clean(label)).append('\n');
        final MediaMetadata m = c.getMetadata();
        if (m != null) {
            o.append("title=").append(clean(m.getText(MediaMetadata.METADATA_KEY_TITLE))).append('\n');
            CharSequence ar = m.getText(MediaMetadata.METADATA_KEY_ARTIST);
            if (ar == null || ar.length() == 0) ar = m.getText(MediaMetadata.METADATA_KEY_ALBUM_ARTIST);
            o.append("artist=").append(clean(ar)).append('\n');
            o.append("album=").append(clean(m.getText(MediaMetadata.METADATA_KEY_ALBUM))).append('\n');
            o.append("duration=").append(m.getLong(MediaMetadata.METADATA_KEY_DURATION)).append('\n');
        }
        final PlaybackState s = c.getPlaybackState();
        if (s != null) {
            final boolean playing = s.getState() == PlaybackState.STATE_PLAYING;
            long pos = s.getPosition();
            if (playing && s.getLastPositionUpdateTime() > 0)
                pos += (long) ((SystemClock.elapsedRealtime() - s.getLastPositionUpdateTime()) * s.getPlaybackSpeed());
            o.append("pos=").append(Math.max(0, pos)).append('\n').append("playing=").append(playing ? 1 : 0).append('\n');
            o.append("actions=").append(s.getActions()).append('\n');
            final Like l = like(c, s);
            if (l != null) o.append("like=1\n").append("liked=").append(l.liked ? 1 : 0).append('\n');
        }
        o.append("art=").append(artGen).append('\n');
        return o.toString();
    }

    /** The song's picture as JPEG (at most 600 px a side), or null */
    static byte[] art() {
        final MediaController c = ctl;
        if (c == null || c.getMetadata() == null) return null;
        final MediaMetadata m = c.getMetadata();
        Bitmap b = m.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART);
        if (b == null) b = m.getBitmap(MediaMetadata.METADATA_KEY_ART);
        if (b == null) b = m.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON);
        if (b == null) return null;
        try {
            final int side = Math.max(b.getWidth(), b.getHeight());
            if (side > 600) b = Bitmap.createScaledBitmap(b, b.getWidth() * 600 / side, b.getHeight() * 600 / side, true);
            final ByteArrayOutputStream out = new ByteArrayOutputStream();
            b.compress(Bitmap.CompressFormat.JPEG, 90, out);
            return out.toByteArray();
        } catch (Throwable t) { return null; }
    }

    /** The app's own icon, size by size, ARGB (the player bar's badge), or null */
    static int[] icon(int size) {
        final MediaController c = ctl;
        if (c == null || ctx == null || size <= 0 || size > 256) return null;
        try {
            final Drawable d = ctx.getPackageManager().getApplicationIcon(c.getPackageName());
            final Bitmap b = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
            d.setBounds(0, 0, size, size);
            d.draw(new Canvas(b));
            final int[] px = new int[size * size];
            b.getPixels(px, 0, size, 0, 0, size, size);
            b.recycle();
            return px;
        } catch (Throwable t) { return null; }
    }

    /** Search › In your other music apps (the user, 2026-11-19): the apps that answer Android's play-from-search
     *  (MEDIA_PLAY_FROM_SEARCH), as "component<TAB>label" lines, one an app; Songate left out. Each finds and plays the words itself:
     *  Songate never sees what it has */
    private static boolean learntGone;

    static String playSearchApps(Context c) {
        if (!learntGone) {   // (the apps learnt to only search, builds 2010016-17: every app only searches now)
            learntGone = true;
            try { c.deleteSharedPreferences("play_search"); } catch (Throwable ignored) {}
        }
        final StringBuilder o = new StringBuilder();
        try {
            final PackageManager pm = c.getPackageManager();
            final Intent i = new Intent(MediaStore.INTENT_ACTION_MEDIA_PLAY_FROM_SEARCH);
            final java.util.Set<String> seen = new java.util.HashSet<>();
            for (android.content.pm.ResolveInfo r : pm.queryIntentActivities(i, 0)) {
                if (r.activityInfo == null) continue;
                final String pkg = r.activityInfo.packageName;
                if (pkg.equals(c.getPackageName()) || !seen.add(pkg)) continue;
                o.append(pkg).append('/').append(r.activityInfo.name).append('\t').append(clean(r.loadLabel(pm))).append('\n');
            }
        } catch (Throwable t) { Log.w("Songate", "play from search: " + t); }
        return o.toString();
    }

    /** The words handed to that app: its own search screen with them (Android's play-from-search, which opens the app on its
     *  results). Only a search: no app plays what Songate asks (the user, 2026-11-19: "no app will have play / only search") */
    static void playSearch(Context c, String component, String query) {
        try {
            final Intent i = new Intent(MediaStore.INTENT_ACTION_MEDIA_PLAY_FROM_SEARCH)
                .setComponent(ComponentName.unflattenFromString(component))
                .putExtra(SearchManager.QUERY, query)
                .putExtra(MediaStore.EXTRA_MEDIA_FOCUS, "vnd.android.cursor.item/*")
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            c.getApplicationContext().startActivity(i);
            Log.i("Songate", "search in another app: its search screen");
        } catch (Throwable t) { Log.w("Songate", "search in another app: " + t); }
    }

    /** cmd: 1 play, 2 pause, 3 next, 4 previous, 5 seek to arg ms, 6 its own like pressed (the heart: like(), as its notification's) */
    static void control(int cmd, long arg) {
        final MediaController c = ctl;
        if (c == null) return;
        try {
            final MediaController.TransportControls t = c.getTransportControls();
            switch (cmd) {
                case 1: t.play(); break;
                case 2: t.pause(); break;
                case 3: t.skipToNext(); break;
                case 4: t.skipToPrevious(); break;
                case 5: t.seekTo(Math.max(0, arg)); break;
                case 6: {
                    final Like l = like(c, c.getPlaybackState());
                    if (l == null) break;
                    if (l.custom != null) t.sendCustomAction(l.custom.getAction(), l.custom.getExtras());
                    else t.setRating(Rating.newHeartRating(!l.liked));
                    Log.i("Songate", "other apps: like pressed (" + (l.custom != null ? "its button" : "a heart rating") + ", was " + (l.liked ? "liked" : "not liked") + ")");
                    break;
                }
                default: break;
            }
        } catch (Throwable t) { Log.w("Songate", "other apps: " + t); }
    }
}
