/*
 * Songate · SongateService.java
 * Music out of sight: a media-playback service with Android's media session, so the app keeps playing when it is not in front, and
 * the system's music controls (the notification, the lock screen, the quick settings' player, headsets and cars) show Songate's song
 * and work it: Previous, Play/Pause, Next, the seek bar, and Songate's own Like and Order (the user, 2026-11-03: "Like and Order").
 * Audio focus as Android asks: a call or another app's sound pauses Songate (and a short one resumes it); unplugging headphones
 * pauses. The app's thread calls the static methods (platform_android.cpp); presses go back to it through nativeCommand.
 */
package io.github.andrythebeast.songate;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;

import android.graphics.drawable.Icon;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

public class SongateService extends Service {
    // platform.hpp's plat::Cmd
    static final int PLAY_PAUSE = 1, PLAY = 2, PAUSE = 3, NEXT = 4, PREVIOUS = 5, LIKE = 8, ORDER = 9, SEEK = 10;
    private static final String CHANNEL = "playing", LIKE_ACTION = "songate.like", ORDER_ACTION = "songate.order";
    private static final int NOTIFICATION = 1;
    // Order's icons and words, as the player bar's (Default order, Repeat playlist, Repeat track, Shuffle)
    private static final int[] ORDER_ICONS = { R.drawable.ic_order, R.drawable.ic_repeat, R.drawable.ic_repeat1, R.drawable.ic_shuffle };
    private static final String[] ORDER_WORDS = { "Default order", "Repeat playlist", "Repeat track", "Shuffle" };

    private static native void nativeCommand(int code, double arg);

    // What plays, as the app last said (the app's thread writes it, Android's reads it: each change is posted to the main thread)
    private static final Handler main = new Handler(Looper.getMainLooper());
    private static SongateService self;
    private static Context app;
    private static String title = "", artist = "", album = "";
    private static Bitmap cover;
    private static boolean has, playing, liked;
    private static int order;
    private static long position, duration, positionAt;

    private MediaSession session;
    private AudioManager audio;
    private AudioFocusRequest focus;
    private boolean resumeOnGain, noisyHeld, foreground, foregroundOnce;

    private final BroadcastReceiver noisy = new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent i) {
            if (AudioManager.ACTION_AUDIO_BECOMING_NOISY.equals(i.getAction()) && playing) nativeCommand(PAUSE, 0);   // headphones out
        }
    };

    /** SongateActivity, as it starts: the context the service is started from. */
    static void attach(Context activity) {
        app = activity.getApplicationContext();
        ended = false;
    }

    /** Songate ended (end()): no state still on its way may start the service again. */
    private static volatile boolean ended = false;

    // ---- from the app's thread

    /** rgba: the cover's pixels, w × h, up to 1024 px (Android scales it to what its player shows). */
    public static void mediaInfo(byte[] t, byte[] ar, byte[] al, byte[] rgba, int w, int h) {
        final String nt = new String(t, StandardCharsets.UTF_8), nar = new String(ar, StandardCharsets.UTF_8), nal = new String(al, StandardCharsets.UTF_8);
        Bitmap made = null;
        if (rgba != null && w > 0 && h > 0 && rgba.length == w * h * 4) {
            made = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);   // its memory is R, G, B, A, as the app's
            made.copyPixelsFromBuffer(ByteBuffer.wrap(rgba));
        }
        final Bitmap bmp = made;
        main.post(() -> {
            title = nt;
            artist = nar;
            album = nal;
            cover = bmp;
            if (self != null) self.refresh();
        });
    }

    public static void mediaState(boolean hasTrack, boolean isPlaying) {
        main.post(() -> {
            has = hasTrack;
            playing = isPlaying;
            if (has && playing && self == null && app != null && !ended) {
                // started while Songate is in front (Android allows a service in front only then), it stays until nothing plays
                Intent i = new Intent(app, SongateService.class);
                app.startForegroundService(i);
            } else if (self != null) {
                if (!has) self.stopSelf();
                else self.refresh();
            }
        });
    }

    public static void mediaProgress(long pos, long dur, boolean isLiked, int ord) {
        final long at = SystemClock.elapsedRealtime();
        main.post(() -> {
            position = pos;
            duration = dur;
            positionAt = at;
            liked = isLiked;
            order = Math.max(0, Math.min(3, ord));
            if (self != null) self.refresh();
        });
    }

    /** Music playing now, as the app last said (main thread). */
    /** Settings › Playback › Keep playing when swiped away (the app sets it at launch and when it changes): off, swiping Songate
     *  away ends the music with it. */
    static volatile boolean keepOnSwipe = true;
    static void setKeepOnSwipe(boolean on) { keepOnSwipe = on; }

    static boolean playingNow() {
        return has && playing;
    }

    /** Songate is ending (its screen closed for good): no music to control. Stopped now, not later: the app's thread ends the
     *  process right after, and a service still started then (or with a start still on its way) was restarted by Android in a
     *  new, empty process ("Scheduling restart of crashed service"; found 2026-11-04 with the swipe-away switch off). */
    static void end() {
        ended = true;
        has = playing = false;
        Runnable stop = () -> {
            if (app != null) app.stopService(new Intent(app, SongateService.class));   // its pending starts too
            else if (self != null) self.stopSelf();
        };
        if (Looper.myLooper() == Looper.getMainLooper()) stop.run();
        else main.post(stop);
    }

    // ---- the service

    @Override
    public void onCreate() {
        super.onCreate();
        self = this;
        audio = (AudioManager) getSystemService(AUDIO_SERVICE);
        NotificationManager nm = getSystemService(NotificationManager.class);
        NotificationChannel ch = new NotificationChannel(CHANNEL, "Playing", NotificationManager.IMPORTANCE_LOW);
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
        session = new MediaSession(this, "Songate");
        session.setCallback(new MediaSession.Callback() {
            @Override public void onPlay() { nativeCommand(PLAY, 0); }
            @Override public void onPause() { nativeCommand(PAUSE, 0); }
            @Override public void onSkipToNext() { nativeCommand(NEXT, 0); }
            @Override public void onSkipToPrevious() { nativeCommand(PREVIOUS, 0); }
            @Override public void onStop() { nativeCommand(PAUSE, 0); }
            @Override public void onSeekTo(long ms) { nativeCommand(SEEK, ms / 1000.0); }
            @Override public void onCustomAction(String action, android.os.Bundle extras) {
                if (LIKE_ACTION.equals(action)) nativeCommand(LIKE, 0);
                else if (ORDER_ACTION.equals(action)) nativeCommand(ORDER, 0);
            }
        });
        Intent open = new Intent(this, SongateActivity.class).setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        session.setSessionActivity(PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE));
        session.setActive(true);
        focus = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setOnAudioFocusChangeListener(this::focusChanged, main)
                .build();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // the notification's own buttons (Android 12 and before draw these; later ones use the session's)
        String a = intent != null ? intent.getAction() : null;
        if (a != null) switch (a) {
            case "songate.prev": nativeCommand(PREVIOUS, 0); break;
            case "songate.playpause": nativeCommand(PLAY_PAUSE, 0); break;
            case "songate.next": nativeCommand(NEXT, 0); break;
            case LIKE_ACTION: nativeCommand(LIKE, 0); break;
            case ORDER_ACTION: nativeCommand(ORDER, 0); break;
        }
        refresh();
        return START_NOT_STICKY;
    }

    /** Swiped away from the recent apps: the music goes on (the user's choice), with its controls. */
    @Override
    public void onTaskRemoved(Intent rootIntent) {}

    @Override
    public void onDestroy() {
        if (noisyHeld) { unregisterReceiver(noisy); noisyHeld = false; }
        audio.abandonAudioFocusRequest(focus);
        session.setActive(false);
        session.release();
        stopForeground(STOP_FOREGROUND_REMOVE);
        self = null;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    private void focusChanged(int change) {
        switch (change) {
            case AudioManager.AUDIOFOCUS_LOSS:   // another app plays: Songate stops for good
                resumeOnGain = false;
                if (playing) nativeCommand(PAUSE, 0);
                break;
            case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT:   // a call, a voice message: back after it
                resumeOnGain = playing;
                if (playing) nativeCommand(PAUSE, 0);
                break;
            case AudioManager.AUDIOFOCUS_GAIN:
                if (resumeOnGain) nativeCommand(PLAY, 0);
                resumeOnGain = false;
                break;
            default: break;   // a short sound over it: Android lowers Songate itself
        }
    }

    private PendingIntent action(String a, int code) {
        return PendingIntent.getService(this, code, new Intent(this, SongateService.class).setAction(a), PendingIntent.FLAG_IMMUTABLE);
    }

    /** Everything the controls show, from what the app last said. */
    private void refresh() {
        // the session: Android 13 and later draw the controls from it
        MediaMetadata.Builder md = new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, title)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, artist)
                .putString(MediaMetadata.METADATA_KEY_ALBUM, album)
                .putLong(MediaMetadata.METADATA_KEY_DURATION, duration);
        if (cover != null) md.putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, cover);
        session.setMetadata(md.build());
        final long elapsed = SystemClock.elapsedRealtime() - positionAt;
        PlaybackState.Builder ps = new PlaybackState.Builder()
                .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_PLAY_PAUSE | PlaybackState.ACTION_SKIP_TO_NEXT
                            | PlaybackState.ACTION_SKIP_TO_PREVIOUS | PlaybackState.ACTION_SEEK_TO | PlaybackState.ACTION_STOP)
                .setState(playing ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED, position + (playing ? elapsed : 0), playing ? 1f : 0f)
                .addCustomAction(new PlaybackState.CustomAction.Builder(LIKE_ACTION, liked ? "Remove from Liked" : "Add to Liked",
                                                                         liked ? R.drawable.ic_heart_f : R.drawable.ic_heart).build())
                .addCustomAction(new PlaybackState.CustomAction.Builder(ORDER_ACTION, ORDER_WORDS[order], ORDER_ICONS[order]).build());
        session.setPlaybackState(ps.build());

        // audio focus and the headphones, while playing
        if (playing) {
            if (audio.requestAudioFocus(focus) == AudioManager.AUDIOFOCUS_REQUEST_FAILED) nativeCommand(PAUSE, 0);
            if (!noisyHeld) { registerReceiver(noisy, new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY)); noisyHeld = true; }
        } else if (noisyHeld) {
            unregisterReceiver(noisy);
            noisyHeld = false;
        }

        // the notification: the same five buttons for Android 12 and before; the session's for later ones
        Intent open = new Intent(this, SongateActivity.class).setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        Notification.Builder nb = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_notify)
                .setContentTitle(title)
                .setContentText(artist)
                .setSubText(album)
                .setLargeIcon(cover)
                .setOngoing(playing)
                .setShowWhen(false)
                .setVisibility(Notification.VISIBILITY_PUBLIC)
                .setContentIntent(PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE))
                .addAction(new Notification.Action.Builder(Icon.createWithResource(this, R.drawable.ic_prev), "Previous", action("songate.prev", 1)).build())
                .addAction(new Notification.Action.Builder(Icon.createWithResource(this, playing ? R.drawable.ic_pause : R.drawable.ic_play),
                                                           playing ? "Pause" : "Play", action("songate.playpause", 2)).build())
                .addAction(new Notification.Action.Builder(Icon.createWithResource(this, R.drawable.ic_next), "Next", action("songate.next", 3)).build())
                .addAction(new Notification.Action.Builder(Icon.createWithResource(this, liked ? R.drawable.ic_heart_f : R.drawable.ic_heart),
                                                           liked ? "Remove from Liked" : "Add to Liked", action(LIKE_ACTION, 4)).build())
                .addAction(new Notification.Action.Builder(Icon.createWithResource(this, ORDER_ICONS[order]), ORDER_WORDS[order], action(ORDER_ACTION, 5)).build())
                .setStyle(new Notification.MediaStyle().setMediaSession(session.getSessionToken()).setShowActionsInCompactView(0, 1, 2));
        Notification n = nb.build();
        if (playing || !foregroundOnce) {   // a service started in front must be in front within seconds, even if paused meanwhile
            foregroundOnce = true;
            if (Build.VERSION.SDK_INT >= 29) startForeground(NOTIFICATION, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
            else startForeground(NOTIFICATION, n);
            foreground = true;
            if (!playing) { stopForeground(STOP_FOREGROUND_DETACH); foreground = false; }
        } else {
            // paused: the controls stay, and can be swiped away; Android may end the app later, as with any paused player
            if (foreground) { stopForeground(STOP_FOREGROUND_DETACH); foreground = false; }
            getSystemService(NotificationManager.class).notify(NOTIFICATION, n);
        }
    }
}
