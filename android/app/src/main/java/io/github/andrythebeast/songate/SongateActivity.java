/*
 * Songate · SongateActivity.java
 * SDL's activity, with what only Java can do for platform/android/platform_android.cpp: the app's own files out of the APK,
 * the device's name, and the network's multicast for Devices and AirPlay.
 */
package io.github.andrythebeast.songate;

import android.Manifest;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.graphics.Rect;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.util.Log;
import android.view.DisplayCutout;
import android.view.RoundedCorner;
import android.view.View;
import android.view.WindowInsets;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.List;

public class SongateActivity extends SDLActivity {
    static SongateActivity self;   // (package-wide: SysApi opens Android's pages from it)
    private WifiManager.MulticastLock multicast;

    // A player's own buttons (pause, next, previous; a headset's button) with Songate in front: Android gives the key to the window
    // first, where SDL took it as an ordinary key and nothing answered, so they worked everywhere but inside Songate (a Hiby R4,
    // 2026-11-13). They go to the media session, as they do when another app is in front.
    @Override
    public boolean dispatchKeyEvent(android.view.KeyEvent event) {
        if (held) return super.dispatchKeyEvent(event);   // (waiting for the first unlock: nothing of Songate runs yet)
        if (android.view.KeyEvent.isMediaSessionKey(event.getKeyCode()) && SongateService.mediaButton(event)) return true;
        // The volume keys with Songate in front, while Settings › Playback › Volume keys show is "Songate": Android's music volume
        // moved here, without Android's panel, and Songate draws its own (App::drawVolumePop). "Android": they go on to Android as
        // before (SDL lets them through), which shows its own panel.
        final int code = event.getKeyCode();
        if (ownVolumeKeys && (code == android.view.KeyEvent.KEYCODE_VOLUME_UP || code == android.view.KeyEvent.KEYCODE_VOLUME_DOWN)) {
            if (event.getAction() == android.view.KeyEvent.ACTION_DOWN) {
                final android.media.AudioManager am = (android.media.AudioManager) getSystemService(Context.AUDIO_SERVICE);
                if (am != null) {
                    try {
                        am.adjustStreamVolume(android.media.AudioManager.STREAM_MUSIC,
                                              code == android.view.KeyEvent.KEYCODE_VOLUME_UP ? android.media.AudioManager.ADJUST_RAISE
                                                                                              : android.media.AudioManager.ADJUST_LOWER, 0);
                    } catch (Throwable t) {
                        Log.w("Songate", "volume key: " + t);
                    }
                }
                reportVolume(true);
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    // ---------------------------------------------------------------- the volume: Android's music volume is Songate's (2.1.0)
    // (the user, 2026-11-15: "make songate volume in android be the android volume"): Songate's slider sets it, the keys move it,
    // and Songate's own level stays at full, so the DAC gets the whole signal.
    private static volatile boolean ownVolumeKeys = true;
    private static native void nativeVolume(int index, int max, boolean fromKey);
    private static native void nativeResumed();
    private android.content.BroadcastReceiver volumeWatch;

    private static void reportVolume(boolean fromKey) {
        final int v = volumeNow();
        if (v < 0) return;
        try {
            nativeVolume(v & 0xffff, v >>> 16, fromKey);
        } catch (UnsatisfiedLinkError e) {   // libmain not loaded yet: the app asks volumeNow itself at its start
        }
    }

    /** The music volume now: (max << 16) | index; −1 when there is no audio service. */
    public static int volumeNow() {
        final Context c = HibyR4.context;
        if (c == null) return -1;
        final android.media.AudioManager am = (android.media.AudioManager) c.getSystemService(Context.AUDIO_SERVICE);
        if (am == null) return -1;
        final int max = Math.max(1, am.getStreamMaxVolume(android.media.AudioManager.STREAM_MUSIC));
        return (max << 16) | Math.max(0, Math.min(max, am.getStreamVolume(android.media.AudioManager.STREAM_MUSIC)));
    }

    /** Songate's slider: the music volume to this step, without Android's panel. */
    public static void volumeSet(int index) {
        final Context c = HibyR4.context;
        if (c == null) return;
        final android.media.AudioManager am = (android.media.AudioManager) c.getSystemService(Context.AUDIO_SERVICE);
        if (am == null) return;
        try {
            am.setStreamVolume(android.media.AudioManager.STREAM_MUSIC, index, 0);
        } catch (Throwable t) {   // (a fixed volume: line out, or Do Not Disturb's rules)
            Log.w("Songate", "volumeSet: " + t);
        }
    }

    // The HiBy R4's settings (HibyR4.java), here because the app's own threads find this class alone (activityClass in
    // platform_android.cpp: a thread the app starts has only the system's class loader)
    public static int hibyState() { return HibyR4.state(); }
    public static boolean hibyDevice() { return HibyR4.device; }
    public static int bitPerfectBegin(int rate, int bits) { return BitPerfect.begin(rate, bits); }
    public static boolean bitPerfectRouted() { return BitPerfect.routed(); }
    public static void bitPerfectEnd() { BitPerfect.end(); }
    public static String hibyGet() { return HibyR4.get(); }
    public static String hibySet(String key, String value) { return HibyR4.set(key, value); }
    public static boolean hibyEqSet(String eq) { return HibyR4.eqSet(eq); }
    public static boolean hibyLedLive(int rgb, int bright, boolean breath) { return HibyR4.ledLive(rgb, bright, breath); }

    /** Settings › Playback › Volume keys show: Songate (true) or Android (false). */
    public static void volumeKeysOwn(boolean own) {
        ownVolumeKeys = own;
    }

    /** SDL opens the main library by its path in the unpacked libraries' folder; loaded from the APK (useLegacyPackaging false: no
     *  such folder's file), by its name: already loaded (getLibraries), the linker hands it back. */
    @Override
    protected String getMainSharedObject() {
        final String path = super.getMainSharedObject();
        return new File(path).exists() ? path : new File(path).getName();
    }

    @Override
    protected String[] getLibraries() {
        // FFmpeg and mpg123 are shared (LGPL): the system's linker finds them beside libmain when it loads
        return new String[] { "SDL2", "main" };
    }

    // ---------------------------------------------------------------- before the first unlock (SongateOS, 2026-11-16)
    // The home activity is direct-boot aware (the manifest): Android shows it at once after the boot animation, with no placeholder
    // home and its black dip between. Before the first unlock the app's own storage cannot be read (settings, the library, the
    // fonts), so the activity only waits, black, as the screen after the boot animation already is; once Android unlocks (a few
    // seconds on the R4, which has no lock screen) it starts again as always, and Songate fades in from that black.
    private boolean held = false;
    private android.content.BroadcastReceiver unlockWatch;

    private boolean userLocked() {
        try {
            final android.os.UserManager um = getSystemService(android.os.UserManager.class);
            return um != null && !um.isUserUnlocked();
        } catch (Throwable t) {
            return false;
        }
    }

    @Override
    protected boolean holdStart() {
        return held;
    }

    private void waitForUnlock() {
        Log.w("Songate", "before the first unlock: waiting");
        try { getWindow().getDecorView().setBackgroundColor(0xff000000); } catch (Throwable ignored) {}
        unlockWatch = new android.content.BroadcastReceiver() {
            @Override
            public void onReceive(Context c, Intent i) {
                startAfterUnlock();
            }
        };
        try {
            final android.content.IntentFilter f = new android.content.IntentFilter(Intent.ACTION_USER_UNLOCKED);
            if (Build.VERSION.SDK_INT >= 33) registerReceiver(unlockWatch, f, Context.RECEIVER_EXPORTED);
            else registerReceiver(unlockWatch, f);
        } catch (Throwable t) {
            Log.w("Songate", "unlock watch: " + t);
        }
        if (!userLocked()) startAfterUnlock();   // (unlocked between the question and the watch)
    }

    private boolean unlockedOnce = false;

    private void startAfterUnlock() {
        if (!held || unlockedOnce) return;
        unlockedOnce = true;
        Log.w("Songate", "unlocked: starting");
        try { if (unlockWatch != null) unregisterReceiver(unlockWatch); } catch (Throwable ignored) {}
        unlockWatch = null;
        // (this waiting screen stays held to its end; the new one is made unheld, the storage there now)
        recreate();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (userLocked()) {   // before the first unlock: SDL is not started (SDLActivity.holdStart), the activity waits
            held = true;
            super.onCreate(savedInstanceState);
            waitForUnlock();
            return;
        }
        // A second copy of the activity in a process where Songate already runs. Android 12 makes one when Songate becomes the
        // home app while it is open as an ordinary app (the home entry is started in the home task, a new activity, in the same
        // process), and at the home button after that. SDL runs once a process: its second start ran the app's main beside the
        // first and crashed (the user's Hiby R4, 2026-11-14: SIGSEGV in SDLThread as "Set as default" was pressed, and Songate
        // was not the home screen after it). So the running one is asked to close as at any quit (what it keeps is saved), and
        // the process ends: Android starts Songate afresh, once, in the place it was asked for. (Newer Androids end the process
        // themselves before the home entry starts.)
        // Only while the other copy is still there: an activity made after the last one was closed (Songate swiped away with its
        // music kept playing, then opened again) joins the running app as it always did, and nothing restarts.
        // (mDetached: SDL's own word that the screen before was closed with the app left running.)
        if (self != null && self != this)
            Log.w("Songate", "an activity made after another: the app's thread " + (mSDLThread == null ? "none" : mSDLThread.isAlive() ? "alive" : "ended")
                             + ", detached " + mDetached);
        if (self != null && self != this && !mDetached && mSDLThread != null && mSDLThread.isAlive()) {
            Log.w("Songate", "a second copy of the activity: closing the running one, starting afresh");
            try {
                nativeSendQuit();
                mSDLThread.join(2500);
            } catch (Throwable t) {
                Log.w("Songate", "closing the running copy: " + t);
            }
            System.exit(0);   // (before SDL's own onCreate: it would set up a second SDL over the first)
        }
        self = this;
        keyLoad(getApplicationContext());   // (the keyboard's sounds' pool; Songate puts its sounds in it at its start: keyWave)
        HibyR4.context = getApplicationContext();
        HibyR4.detect(HibyR4.context);   // (before SDL starts the app's thread: its first frame already knows)
        homeEntryAtStart();
        // (each step on its own: one that fails is logged and the start goes on; the only home app must not end at its start)
        try { SongateService.attach(this); } catch (Throwable t) { Log.w("Songate", "start: service: " + t); }
        try { copyRomfs(); } catch (Throwable t) { Log.w("Songate", "start: romfs: " + t); }   // before SDL starts the app's thread: the fonts are read at launch
        try {
            WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wifi != null) {
                multicast = wifi.createMulticastLock("songate");
                multicast.setReferenceCounted(false);
                multicast.acquire();   // Devices' broadcasts and AirPlay's mDNS reach the app
            }
        } catch (Throwable t) { Log.w("Songate", "start: multicast: " + t); }
        super.onCreate(savedInstanceState);
        // Android's own mark for the view that has the keys' focus is not Songate's: after typing (SDL gives its surface the focus
        // back) and a key from a keyboard or a pad, it drew a green frame round the whole screen (seen 2026-11-11 in the test
        // scripts' frames, which type with keys). Songate draws its own focus ring
        if (Build.VERSION.SDK_INT >= 26) {
            if (mSurface != null) mSurface.setDefaultFocusHighlightEnabled(false);
            if (mLayout != null) mLayout.setDefaultFocusHighlightEnabled(false);
            getWindow().getDecorView().setDefaultFocusHighlightEnabled(false);
        }
        // The camera's cutouts and the rounded corners, to the app each time Android lays the window out (a turn, a fold):
        // Settings › Appearance › Screen edges decides what steps aside (App::updateEdges)
        getWindow().getDecorView().setOnApplyWindowInsetsListener((v, insets) -> {
            reportEdges(insets);
            return v.onApplyWindowInsets(insets);
        });
        // The keys move the music volume even with nothing playing (not the ringer's), and Songate hears every change of it,
        // whoever made it (the keys, Android's panel, another app, line out locking it)
        setVolumeControlStream(android.media.AudioManager.STREAM_MUSIC);
        volumeWatch = new android.content.BroadcastReceiver() {
            @Override
            public void onReceive(Context c, Intent i) {
                reportVolume(false);
            }
        };
        try {
            final android.content.IntentFilter f = new android.content.IntentFilter("android.media.VOLUME_CHANGED_ACTION");
            if (Build.VERSION.SDK_INT >= 33) registerReceiver(volumeWatch, f, Context.RECEIVER_EXPORTED);
            else registerReceiver(volumeWatch, f);
        } catch (Throwable t) {
            Log.w("Songate", "volume watch: " + t);
            volumeWatch = null;
        }
    }

    private static native void nativeEdges(float[] boxes, float cornerRadius, int width, int height);

    /** A turn: the cutouts again, now and once the window has its new size (the user, 2026-11-05: "sometimes when switch from
     *  landscape to portrait the app shows a left side black bar": the sideways cutout kept for the upright screen). */
    @Override
    public void onConfigurationChanged(android.content.res.Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        final View decor = getWindow().getDecorView();
        decor.requestApplyInsets();
        decor.postDelayed(decor::requestApplyInsets, 300);
        decor.postDelayed(decor::requestApplyInsets, 1000);
    }
    private static native void nativeLeave();

    /** Swiped away from the recent apps while music plays: the music goes on (the user, 2026-11-03: "Keep playing"), the app with
     *  it, out of sight; opening Songate again brings its screen back to it (SDLActivity.onCreate). Not playing, or the switch off
     *  (Settings › Playback; the user chose the switch, 2026-11-04): it ends as ever.
     *  Also whenever Android only rebuilds the screen (a change the manifest can't take itself: new system colours, an overlay):
     *  the app goes on and the new screen joins it, playing or not (the Hiby R4, 2026-11-16: a System colours change ended the
     *  process and Songate came back on Now Playing). */
    @Override
    protected boolean keepNativeWhenDestroyed() {
        if (isChangingConfigurations()) return true;
        return SongateService.playingNow() && SongateService.keepOnSwipe;
    }

    /** Leaving the front (home, another app, the screen off): the app writes its settings, session and caches now, on its own
     *  thread, before SDL pauses it; Android may end it later without another word. */
    @Override
    protected void onPause() {
        OtherViz.stop();   // (the spectrum for other apps' music: only while Songate is in front)
        if (held) { super.onPause(); return; }
        try {
            nativeLeave();
        } catch (UnsatisfiedLinkError e) {   // libmain not loaded: nothing to write
        }
        super.onPause();
    }

    /** Every cutout as a box (x, y, w, h in window pixels), whatever its shape: a hole in a corner or the middle, a notch, two. */
    private void reportEdges(WindowInsets insets) {
        float[] boxes = new float[0];
        DisplayCutout cut = insets.getDisplayCutout();
        if (cut != null) {
            List<Rect> rects = cut.getBoundingRects();
            boxes = new float[rects.size() * 4];
            for (int i = 0; i < rects.size(); i++) {
                Rect r = rects.get(i);
                boxes[i * 4] = r.left;
                boxes[i * 4 + 1] = r.top;
                boxes[i * 4 + 2] = r.width();
                boxes[i * 4 + 3] = r.height();
            }
        }
        float radius = 0;
        if (Build.VERSION.SDK_INT >= 31) {   // the corners' own radius; older Androids do not say
            for (int p : new int[] { RoundedCorner.POSITION_TOP_LEFT, RoundedCorner.POSITION_TOP_RIGHT,
                                     RoundedCorner.POSITION_BOTTOM_RIGHT, RoundedCorner.POSITION_BOTTOM_LEFT }) {
                RoundedCorner c = insets.getRoundedCorner(p);
                if (c != null) radius = Math.max(radius, c.getRadius());
            }
        }
        // the window size these boxes are for: the app uses them only while its own is turned the same way
        int width = getWindow().getDecorView().getWidth(), height = getWindow().getDecorView().getHeight();
        if (Build.VERSION.SDK_INT >= 30) {
            Rect b = getWindowManager().getCurrentWindowMetrics().getBounds();
            width = b.width();
            height = b.height();
        }
        try {
            nativeEdges(boxes, radius, width, height);
        } catch (UnsatisfiedLinkError e) {   // before libmain is loaded: the next layout reports them
        }
    }

    @Override
    protected void onDestroy() {
        if (held || unlockWatch != null) {   // (the waiting screen: only its watch to end)
            try { if (unlockWatch != null) unregisterReceiver(unlockWatch); } catch (Throwable ignored) {}
            unlockWatch = null;
            super.onDestroy();
            return;
        }
        if (multicast != null && multicast.isHeld()) multicast.release();
        if (volumeWatch != null) {
            try { unregisterReceiver(volumeWatch); } catch (Throwable ignored) {}
            volumeWatch = null;
        }
        if (!keepNativeWhenDestroyed()) SongateService.end();   // SDL ends the app with its screen: no music left to control
        super.onDestroy();
    }

    /** The APK's romfs into files/romfs, once for each version (the app opens its fonts as files). */
    private void copyRomfs() {
        File dir = new File(getFilesDir(), "romfs");
        File stamp = new File(dir, ".version");
        String version = "";
        try {
            PackageInfo p = getPackageManager().getPackageInfo(getPackageName(), 0);
            version = p.versionName + "/" + p.lastUpdateTime;
        } catch (Exception ignored) {}
        try {
            if (stamp.exists() && version.equals(new String(java.nio.file.Files.readAllBytes(stamp.toPath()), "UTF-8"))) return;
            copyTree(getAssets(), "romfs", dir);
            try (OutputStream o = new FileOutputStream(stamp)) { o.write(version.getBytes("UTF-8")); }
        } catch (Exception e) {
            Log.e("Songate", "copying romfs", e);
        }
    }

    private static void copyTree(AssetManager assets, String from, File to) throws Exception {
        String[] names = assets.list(from);
        if (names == null || names.length == 0) {   // a file
            to.getParentFile().mkdirs();
            try (InputStream in = assets.open(from); OutputStream out = new FileOutputStream(to)) {
                byte[] buf = new byte[65536];
                for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            }
            return;
        }
        to.mkdirs();
        for (String n : names) copyTree(assets, from + "/" + n, new File(to, n));
    }

    /** Songate may read the shared volumes: All files access from Android 11, the storage permission on Android 10. */
    public static boolean hasAllFiles() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return self != null && self.checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    /** Android's own page for All files access, Songate's switch on it (Android 10: its dialog). The app sees the answer as it comes back. */
    public static void askAllFiles() {
        final SongateActivity a = self;
        if (a == null) return;
        a.runOnUiThread(() -> {
            if (Build.VERSION.SDK_INT >= 30) {
                try {
                    a.startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + a.getPackageName())));
                } catch (Exception e) {   // a phone without Songate's own page: the list of apps
                    a.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                }
            } else {
                a.requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE }, 1);
            }
        });
    }

    /** The Canvas opened from a phone held upright (it is sideways' only): the screen turned sideways and kept there, either way
     *  round as the phone is held; given back afterwards to what it was (SDL's: every way, as the phone's own setting allows),
     *  so the sensor turns it upright again when that is how it is held. */
    private static int orientationBefore = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_FULL_USER;
    private static boolean landscapeHeld = false;
    public static void holdLandscape(final boolean on) {
        final SongateActivity a = self;
        if (a == null) return;
        a.runOnUiThread(() -> {
            if (on == landscapeHeld) return;
            if (on) {
                orientationBefore = a.getRequestedOrientation();
                a.setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
            } else {
                a.setRequestedOrientation(orientationBefore);
            }
            landscapeHeld = on;
        });
    }

    /** Settings › Appearance › Screen rotation (2.0.1): 0 Auto, with the device as its own auto-rotate allows (SDL's, as it always
     *  was); 1 Portrait; 2 Landscape, either way round as it is held. Held whatever the device's auto-rotate says. While the Canvas
     *  holds the screen sideways this is what it gives the screen back to. */
    // ---- The keyboard card's keys (plat::keyTap): a soft click of Songate's own and a light tick
    private static volatile android.media.SoundPool keyPool;
    private static final int[] keyIds = { 0, 0, 0, 0 };

    /** The pool the key sounds play from: made once */
    private static synchronized void keyLoad(Context c) {
        if (keyPool != null) return;
        keyPool = new android.media.SoundPool.Builder().setMaxStreams(4)
            .setAudioAttributes(new android.media.AudioAttributes.Builder()
                .setUsage(android.media.AudioAttributes.USAGE_ASSISTANCE_SONIFICATION)
                .setContentType(android.media.AudioAttributes.CONTENT_TYPE_SONIFICATION).build()).build();
    }

    /** One of the keyboard's key sounds (Songate's own, keyclick.hpp: Soft, Tick, Pop, Clack), 16-bit mono: written into the app's
     *  cache as a WAV and loaded into the pool, by its number */
    public static void keyWave(int kind, short[] pcm, int rate) {
        final SongateActivity a = self;
        if (a == null || kind < 0 || kind >= keyIds.length) return;
        try {
            keyLoad(a.getApplicationContext());
            final int n = pcm.length;
            final java.nio.ByteBuffer b = java.nio.ByteBuffer.allocate(44 + n * 2).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            b.put("RIFF".getBytes()).putInt(36 + n * 2).put("WAVE".getBytes()).put("fmt ".getBytes()).putInt(16)
             .putShort((short) 1).putShort((short) 1).putInt(rate).putInt(rate * 2).putShort((short) 2).putShort((short) 16)
             .put("data".getBytes()).putInt(n * 2);
            for (short x : pcm) b.putShort(x);
            final File f = new File(a.getCacheDir(), "key-sound-" + kind + ".wav");
            try (FileOutputStream o = new FileOutputStream(f)) { o.write(b.array()); }
            keyIds[kind] = keyPool.load(f.getPath(), 1);
        } catch (Exception e) { Log.w("Songate", "key sound: " + e); }
    }

    /** click: no music playing in Songate (the app says); none either while another app plays music, unless overMusic (Settings'
     *  While music plays, or a pick of a sound there). kind: which sound. The tick (vibrate) always. */
    public static void keyTap(boolean click, int vibrate, int kind, boolean overMusic) {
        final SongateActivity a = self;
        if (a == null) return;
        try {
            final android.media.AudioManager am = (android.media.AudioManager) a.getSystemService(Context.AUDIO_SERVICE);
            final int id = kind >= 0 && kind < keyIds.length ? keyIds[kind] : 0;
            if (click && keyPool != null && id != 0 && am != null && (overMusic || !am.isMusicActive())
                && am.getRingerMode() == android.media.AudioManager.RINGER_MODE_NORMAL)
                keyPool.play(id, 0.5f, 0.5f, 1, 0, 1f);
            if (vibrate > 0) {   // Light, Medium, Strong: Android's own tick, click and heavy click
                final android.os.Vibrator v = (android.os.Vibrator) a.getSystemService(Context.VIBRATOR_SERVICE);
                final int effect = vibrate >= 3 ? android.os.VibrationEffect.EFFECT_HEAVY_CLICK
                                 : vibrate == 2 ? android.os.VibrationEffect.EFFECT_CLICK : android.os.VibrationEffect.EFFECT_TICK;
                if (v != null && v.hasVibrator()) v.vibrate(android.os.VibrationEffect.createPredefined(effect));
            }
        } catch (Exception e) { Log.w("Songate", "key tap: " + e); }
    }

    public static void screenRotation(final int kind) {
        final SongateActivity a = self;
        if (a == null) return;
        a.runOnUiThread(() -> {
            final int want = kind == 1 ? android.content.pm.ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                           : kind == 2 ? android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                           : android.content.pm.ActivityInfo.SCREEN_ORIENTATION_FULL_USER;
            if (landscapeHeld) orientationBefore = want;
            else if (a.getRequestedOrientation() != want) a.setRequestedOrientation(want);
        });
    }

    /** An update that Settings › Updates downloaded and checked (plat::relaunch): Android's own installer opens on it, asks, and
     *  replaces Songate; its last page offers to open the new one. The first time, Android asks to let Songate install apps.
     *  False when the file is not an update of Songate's or no installer answers. */
    public static boolean installApk(final String path) {
        final SongateActivity a = self;
        if (a == null || path == null) return false;
        final File f = new File(path);
        if (!f.isFile() || !f.getName().startsWith("SongateAND-") || !f.getName().endsWith(".apk")) return false;
        final Intent i = new Intent(Intent.ACTION_VIEW);
        i.setDataAndType(new Uri.Builder().scheme("content").authority(ApkProvider.AUTHORITY).appendPath(f.getName()).build(), ApkProvider.MIME);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
        if (i.resolveActivity(a.getPackageManager()) == null) return false;
        // Not yet allowed to install (the first update): Android's own "Allow from this source" page is opened for Songate, and
        // the installer right after it, once allowed (onActivityResult). Left to the installer, it sent the user to that page
        // and then dropped back to Songate, where Restart now had to be pressed a second time.
        if (Build.VERSION.SDK_INT >= 26 && !a.getPackageManager().canRequestPackageInstalls()) {
            final Intent allow = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + a.getPackageName()));
            if (allow.resolveActivity(a.getPackageManager()) != null) {
                pendingInstall = i;
                a.runOnUiThread(() -> {
                    try { a.startActivityForResult(allow, REQUEST_ALLOW_INSTALL); }
                    catch (Exception e) {   // (no such page on this phone: the installer asks by itself, as before)
                        pendingInstall = null;
                        try { a.startActivity(i); } catch (Exception e2) { Log.w("Songate", "installApk: " + e2); }
                    }
                });
                return true;
            }
        }
        a.runOnUiThread(() -> {
            try { a.startActivity(i); } catch (Exception e) { Log.w("Songate", "installApk: " + e); }
        });
        return true;
    }

    private static final int REQUEST_ALLOW_INSTALL = 4711;
    private static Intent pendingInstall = null;   // the installer's intent, waiting for "Allow from this source"

    // ---------------------------------------------------------------- Songate as the home screen (2.0.1)
    // "Use Songate as the home screen" (the user, 2026-11-13, for players used only for music: Songate is what the device starts
    // into and where its home button returns). Android's own word is the only truth: whether Songate holds the home role. The
    // switch enables the home entry (the manifest's HomeAlias) and Android asks, in its own dialog; turned off, the entry is
    // disabled and Android goes back to its other home app. While it holds, Songate shows over the lock screen, and a lock
    // screen without a PIN or pattern is passed (Android lets no app take a PIN away).
    private static final int REQUEST_HOME_ROLE = 7402;

    private static android.content.ComponentName homeAlias(Context c) {
        return new android.content.ComponentName(c.getPackageName(), "io.github.andrythebeast.songate.HomeAlias");
    }

    // SongateOS's system settings (SysApi.java): for the app's worker thread (callStatic finds them here)
    public static String sysCall(String method, String arg, String extras) { return SysApi.call(method, arg, extras); }
    public static void sysWatch(String area, boolean on) { SysApi.watch(area, on); }
    public static void sysOpen(String which) { SysApi.openAndroid(which); }

    /** The home entry is on in the manifest (SongateOS has no other home app). On the R4 it stays on (turned back on if it was
     *  ever switched off); anywhere else, never chosen (the manifest's default), it is turned off at Songate's first start, as it
     *  was before: a choice made with the Settings switch (on or off) is left as it is. */
    private void homeEntryAtStart() {
        try {
            final PackageManager pm = getPackageManager();
            final android.content.ComponentName c = homeAlias(this);
            final int st = pm.getComponentEnabledSetting(c);
            if (HibyR4.device) {
                if (st == PackageManager.COMPONENT_ENABLED_STATE_DISABLED)
                    pm.setComponentEnabledSetting(c, PackageManager.COMPONENT_ENABLED_STATE_ENABLED, PackageManager.DONT_KILL_APP);
            } else if (st == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT) {
                pm.setComponentEnabledSetting(c, PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP);
            }
        } catch (Throwable t) {
            Log.w("Songate", "home entry at start: " + t);
        }
    }

    public static boolean homeHeld() {
        final SongateActivity a = self;
        if (a == null || Build.VERSION.SDK_INT < 29) return false;
        try {
            final android.app.role.RoleManager rm = a.getSystemService(android.app.role.RoleManager.class);
            return rm != null && rm.isRoleHeld(android.app.role.RoleManager.ROLE_HOME);
        } catch (Exception e) {
            return false;
        }
    }

    public static void homeSet(final boolean on) {
        final SongateActivity a = self;
        if (a == null) return;
        a.runOnUiThread(() -> {
            try {
                final PackageManager pm = a.getPackageManager();
                pm.setComponentEnabledSetting(homeAlias(a),
                                              on ? PackageManager.COMPONENT_ENABLED_STATE_ENABLED : PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                                              PackageManager.DONT_KILL_APP);
                if (on && !homeHeld()) {
                    Intent ask = null;
                    if (Build.VERSION.SDK_INT >= 29) {
                        final android.app.role.RoleManager rm = a.getSystemService(android.app.role.RoleManager.class);
                        if (rm != null && rm.isRoleAvailable(android.app.role.RoleManager.ROLE_HOME))
                            ask = rm.createRequestRoleIntent(android.app.role.RoleManager.ROLE_HOME);
                    }
                    if (ask != null) a.startActivityForResult(ask, REQUEST_HOME_ROLE);
                    else a.startActivity(new Intent(Settings.ACTION_HOME_SETTINGS));   // (no dialog on this Android: its own page)
                }
            } catch (Exception e) {
                Log.w("Songate", "homeSet: " + e);
            }
            a.applyHome();
        });
    }

    /** Over the lock screen while Songate is the home screen; a lock screen with nothing to enter is passed. */
    private void applyHome() {
        try {
            final boolean held = homeHeld();
            if (Build.VERSION.SDK_INT >= 27) setShowWhenLocked(held);
            if (!held) return;
            final android.app.KeyguardManager km = (android.app.KeyguardManager) getSystemService(Context.KEYGUARD_SERVICE);
            if (km != null && km.isKeyguardLocked() && !km.isKeyguardSecure() && Build.VERSION.SDK_INT >= 26) km.requestDismissKeyguard(this, null);
        } catch (Exception e) {
            Log.w("Songate", "applyHome: " + e);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (held) return;
        applyHome();
        reportVolume(false);
        try {
            nativeResumed();   // (the HiBy R4's settings read again: line out ends by itself when its cable comes out)
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    /** The apps a home screen shows, by name: label, then "package/activity", for each (Songate itself left out). */
    /** Of apps(): those that came with the device (FLAG_SYSTEM; an updated one keeps it), as package/activity */
    public static String[] systemApps() {
        final SongateActivity a = self;
        if (a == null) return new String[0];
        try {
            final PackageManager pm = a.getPackageManager();
            final List<android.content.pm.ResolveInfo> found =
                pm.queryIntentActivities(new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER), 0);
            final java.util.ArrayList<String> rows = new java.util.ArrayList<>();
            for (android.content.pm.ResolveInfo r : found) {
                if (r.activityInfo == null || r.activityInfo.applicationInfo == null) continue;
                if ((r.activityInfo.applicationInfo.flags & android.content.pm.ApplicationInfo.FLAG_SYSTEM) != 0)
                    rows.add(r.activityInfo.packageName + "/" + r.activityInfo.name);
            }
            return rows.toArray(new String[0]);
        } catch (Exception e) {
            Log.w("Songate", "systemApps: " + e);
            return new String[0];
        }
    }

    public static String[] apps() {
        final SongateActivity a = self;
        if (a == null) return new String[0];
        try {
            final PackageManager pm = a.getPackageManager();
            final List<android.content.pm.ResolveInfo> found =
                pm.queryIntentActivities(new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER), 0);
            final java.util.ArrayList<String[]> rows = new java.util.ArrayList<>();
            for (android.content.pm.ResolveInfo r : found) {
                if (r.activityInfo == null || a.getPackageName().equals(r.activityInfo.packageName)) continue;
                final CharSequence label = r.loadLabel(pm);
                rows.add(new String[] { label != null ? label.toString() : r.activityInfo.packageName,
                                        r.activityInfo.packageName + "/" + r.activityInfo.name });
            }
            java.util.Collections.sort(rows, (x, y) -> x[0].compareToIgnoreCase(y[0]));
            final String[] out = new String[rows.size() * 2];
            for (int i = 0; i < rows.size(); i++) {
                out[i * 2] = rows.get(i)[0];
                out[i * 2 + 1] = rows.get(i)[1];
            }
            return out;
        } catch (Exception e) {
            Log.w("Songate", "apps: " + e);
            return new String[0];
        }
    }

    // ---- other apps' music (OtherApps.java), for the app
    public static boolean otherAllowed() { final SongateActivity a = self; return a != null && OtherApps.allowed(a); }
    public static void otherAccess() { final SongateActivity a = self; if (a != null) OtherApps.openAccess(a); }
    public static boolean otherWatch(boolean on) { final SongateActivity a = self; if (a == null) return false; OtherApps.watch(a, on); return true; }
    public static String otherState() { return OtherApps.state(); }
    public static byte[] otherArt() { return OtherApps.art(); }
    public static int[] otherIcon(int size) { return OtherApps.icon(size); }
    public static void otherControl(int cmd, long arg) { OtherApps.control(cmd, arg); }
    public static String playSearchApps() { return self == null ? "" : OtherApps.playSearchApps(self); }
    public static void playSearch(String component, byte[] query) {
        if (self != null) OtherApps.playSearch(self, component, new String(query, java.nio.charset.StandardCharsets.UTF_8));
    }
    // The spectrum for other apps' music (OtherViz): Android's audio recording permission, asked in Android's own dialog; its
    // answer kept for the app (1 allowed, 2 refused; 0 none since the last read)
    private static final int ASK_RECORD = 7;
    private static volatile int recordAnswered = 0;
    public static boolean recordAllowed() {
        final SongateActivity a = self;
        return a != null && a.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
    }
    public static void askRecord() {
        final SongateActivity a = self;
        if (a != null) a.runOnUiThread(() -> a.requestPermissions(new String[] { Manifest.permission.RECORD_AUDIO }, ASK_RECORD));
    }
    public static int recordAnswer() { final int r = recordAnswered; recordAnswered = 0; return r; }
    public static byte[] otherViz(int[] rateHz) { return OtherViz.wave(rateHz); }
    public static void otherVizStop() { OtherViz.stop(); }

    @Override
    public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(code, permissions, results);
        if (code == ASK_RECORD) recordAnswered = results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED ? 1 : 2;
    }

    /** An app's icon, size by size pixels of ARGB, or null. */
    public static int[] appIcon(String component, int size) {
        final SongateActivity a = self;
        if (a == null || component == null || size <= 0 || size > 512) return null;
        try {
            final android.content.ComponentName cn = android.content.ComponentName.unflattenFromString(component);
            if (cn == null) return null;
            final android.graphics.drawable.Drawable d = a.getPackageManager().getActivityIcon(cn);
            final android.graphics.Bitmap b = android.graphics.Bitmap.createBitmap(size, size, android.graphics.Bitmap.Config.ARGB_8888);
            final android.graphics.Canvas c = new android.graphics.Canvas(b);
            d.setBounds(0, 0, size, size);
            d.draw(c);
            final int[] px = new int[size * size];
            b.getPixels(px, 0, size, 0, 0, size, size);
            b.recycle();
            return px;
        } catch (Exception e) {
            return null;
        }
    }

    /** Android's own page for an app (its menu's App settings), or its own question whether to uninstall it. */
    public static void appPage(final String component, final boolean uninstall) {
        final SongateActivity a = self;
        if (a == null || component == null) return;
        a.runOnUiThread(() -> {
            try {
                final android.content.ComponentName cn = android.content.ComponentName.unflattenFromString(component);
                if (cn == null) return;
                final Uri pkg = Uri.fromParts("package", cn.getPackageName(), null);
                a.startActivity(new Intent(uninstall ? Intent.ACTION_DELETE : Settings.ACTION_APPLICATION_DETAILS_SETTINGS, pkg)
                                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            } catch (Exception e) {
                Log.w("Songate", "appPage: " + e);
            }
        });
    }

    public static void openApp(final String component) {
        final SongateActivity a = self;
        if (a == null || component == null) return;
        a.runOnUiThread(() -> {
            try {
                final android.content.ComponentName cn = android.content.ComponentName.unflattenFromString(component);
                if (cn == null) return;
                a.startActivity(new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER).setComponent(cn)
                                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED));
            } catch (Exception e) {
                Log.w("Songate", "openApp: " + e);
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_HOME_ROLE) {
            // not given (the dialog left by Cancel): the home entry off again, so Android does not keep offering Songate
            if (!homeHeld()) {
                try {
                    getPackageManager().setComponentEnabledSetting(homeAlias(this), PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                                                                   PackageManager.DONT_KILL_APP);
                } catch (Exception e) { Log.w("Songate", "home role: " + e); }
            }
            applyHome();
            return;
        }
        if (requestCode != REQUEST_ALLOW_INSTALL) return;
        final Intent i = pendingInstall;
        pendingInstall = null;
        // allowed: the installer, at once. Not allowed (the user came back without it): nothing; Restart now asks again
        if (i != null && Build.VERSION.SDK_INT >= 26 && getPackageManager().canRequestPackageInstalls()) {
            try { startActivity(i); } catch (Exception e) { Log.w("Songate", "installApk: " + e); }
        }
    }

    /** The battery beyond its charge (plat::batteryInfo), a "name=value" a line; what the phone does not say has no line:
     *  volts, temp (°C), plugged (0/1), amps (+ into the battery, − out of it), left and full (mAh), health (%), ttf (minutes to
     *  full, Android's own estimate), cycles. */
    public static String batteryInfo() {
        final SongateActivity a = self;
        if (a == null) return "";
        StringBuilder out = new StringBuilder();
        try {
            Intent st = a.registerReceiver(null, new android.content.IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (st == null) return "";
            android.os.BatteryManager bm = (android.os.BatteryManager) a.getSystemService(Context.BATTERY_SERVICE);
            int mv = st.getIntExtra(android.os.BatteryManager.EXTRA_VOLTAGE, 0);
            int t10 = st.getIntExtra(android.os.BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
            int plugged = st.getIntExtra(android.os.BatteryManager.EXTRA_PLUGGED, 0);
            int status = st.getIntExtra(android.os.BatteryManager.EXTRA_STATUS, 0);
            int level = st.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1), scale = st.getIntExtra(android.os.BatteryManager.EXTRA_SCALE, 100);
            if (mv > 2500 && mv < 20000) out.append("volts=").append(mv / 1000.0).append('\n');
            if (t10 > -400 && t10 < 1000 && t10 != 0) out.append("temp=").append(t10 / 10.0).append('\n');
            out.append("plugged=").append(plugged != 0 ? 1 : 0).append('\n');
            if (bm != null) {
                // the current: microamps by the documentation, milliamps on some phones; and its sign is not the same everywhere, so
                // it is set from what the phone is doing (charging: in; no charger: out)
                long now = bm.getLongProperty(android.os.BatteryManager.BATTERY_PROPERTY_CURRENT_NOW);
                if (now != Long.MIN_VALUE && now != 0 && now != Integer.MIN_VALUE) {
                    double amps = Math.abs(now) > 20000 ? now / 1e6 : now / 1e3;
                    if (plugged == 0 && amps > 0) amps = -amps;
                    if (status == android.os.BatteryManager.BATTERY_STATUS_CHARGING && amps < 0) amps = -amps;
                    if (Math.abs(amps) < 12) out.append("amps=").append(amps).append('\n');
                }
                long uah = bm.getLongProperty(android.os.BatteryManager.BATTERY_PROPERTY_CHARGE_COUNTER);
                double designed = 0;
                try {   // what the maker says it holds (no public way: the system's own power profile)
                    Class<?> pp = Class.forName("com.android.internal.os.PowerProfile");
                    Object p = pp.getConstructor(Context.class).newInstance(a);
                    designed = ((Number) pp.getMethod("getBatteryCapacity").invoke(p)).doubleValue();
                } catch (Throwable e) {
                    designed = 0;
                }
                if (uah > 100000 && uah < 30000000 && level > 0 && scale > 0) {
                    double left = uah / 1000.0, pct = level * 100.0 / scale;
                    out.append("left=").append(left).append('\n');
                    if (pct >= 20) {   // (under that, a whole percent is too coarse to tell what full is)
                        double full = left / pct * 100;
                        out.append("full=").append(full).append('\n');
                        if (designed > 500 && full / designed > 0.4 && full / designed < 1.15)
                            out.append("health=").append(Math.min(100.0, full / designed * 100)).append('\n');
                    }
                }
                if (plugged != 0) {
                    long ms = bm.computeChargeTimeRemaining();
                    if (ms > 0) out.append("ttf=").append(ms / 60000).append('\n');
                }
            }
            if (Build.VERSION.SDK_INT >= 34) {
                int cycles = st.getIntExtra("android.os.extra.CYCLE_COUNT", 0);
                if (cycles > 0) out.append("cycles=").append(cycles).append('\n');
            }
        } catch (Throwable e) {
            Log.w("Songate", "batteryInfo: " + e);
        }
        return out.toString();
    }

    /** Real pixels to a dp, as the display has it now (the user's Display size changes it): the layout's scale comes from the
     *  window's size in dp (App::fitWindow). */
    public static float density() {
        final SongateActivity a = self;
        return a != null ? a.getResources().getDisplayMetrics().density : 0f;
    }

    /** The mounted shared volumes, as Android lists them (StorageManager, Android 11 and later: each one's own folder): the phone's
     *  own first, then each card; null on an older Android or when it cannot say (the app folders then). A card mounted after
     *  Songate started (the home app, at a boot) may have no app folder yet: the R4's microSD card was missing (2026-11-19) */
    private static java.util.List<StorageVolume> mountedVolumes() {
        if (android.os.Build.VERSION.SDK_INT < 30 || self == null) return null;
        try {
            final StorageManager sm = (StorageManager) self.getSystemService(Context.STORAGE_SERVICE);
            final java.util.List<StorageVolume> out = new java.util.ArrayList<>();
            for (StorageVolume v : sm.getStorageVolumes()) {
                final String st = v.getState();
                if (v.getDirectory() == null || !(android.os.Environment.MEDIA_MOUNTED.equals(st) || android.os.Environment.MEDIA_MOUNTED_READ_ONLY.equals(st))) continue;
                if (v.isPrimary()) out.add(0, v);
                else out.add(v);
            }
            return out.isEmpty() ? null : out;
        } catch (Throwable t) { return null; }
    }

    /** The shared volumes, one a line: the phone's own first, then each card that is in */
    public static String volumes() {
        StringBuilder out = new StringBuilder();
        final java.util.List<StorageVolume> vs = mountedVolumes();
        if (vs != null) {
            for (StorageVolume v : vs) out.append(v.getDirectory().getAbsolutePath()).append('\n');
            return out.toString();
        }
        for (File f : self.getExternalFilesDirs(null)) {   // (Android 10: their app folders say where they are)
            if (f == null) continue;   // a card being taken out
            String p = f.getAbsolutePath();
            int cut = p.indexOf("/Android/data/");
            if (cut > 0) out.append(p, 0, cut).append('\n');
        }
        return out.toString();
    }

    /** Whether the volume holding path is mounted, as Android says, not merely there: at a boot an SD card's folder is there while
     *  vold still finishes mounting it, and vold ends every app with a file open on it (the user's Hiby R4, 2026-11-15: Songate,
     *  the home app, opened its music and was ended four times; the queue was lost). True when Android does not say (no such
     *  volume known): nothing is held back then. */
    /** Music goes to wired headphones now (the HiBy R4's 3.5 and 4.4 mm): its output rate may follow the song. Not Bluetooth, not USB. */
    public static boolean wiredOut() {
        try {
            if (self == null || android.os.Build.VERSION.SDK_INT < 33) return false;
            android.media.AudioManager am = (android.media.AudioManager) self.getSystemService(AUDIO_SERVICE);
            android.media.AudioAttributes aa = new android.media.AudioAttributes.Builder().setUsage(android.media.AudioAttributes.USAGE_MEDIA)
                .setContentType(android.media.AudioAttributes.CONTENT_TYPE_MUSIC).build();
            boolean wired = false;
            for (android.media.AudioDeviceInfo d : am.getAudioDevicesForAttributes(aa)) {
                final int t = d.getType();
                // (the HiBy R4 with SongateOS: Android knows only the 3.5 mm jack, so the 4.4 mm balanced output is "the speaker", the
                // R4 having none of its own: wired too; the SongateOS session's request, 2026-11-19)
                final boolean r4Balanced = t == android.media.AudioDeviceInfo.TYPE_BUILTIN_SPEAKER && HibyR4.device;
                if (t == android.media.AudioDeviceInfo.TYPE_WIRED_HEADPHONES || t == android.media.AudioDeviceInfo.TYPE_WIRED_HEADSET || r4Balanced) wired = true;
                else return false;   // (anything else in the route: not only wired)
            }
            return wired;
        } catch (Throwable t) { return false; }
    }

    public static boolean volumeReady(String path) {
        final Context c = HibyR4.context;
        if (c == null || path == null) return true;
        try {
            final StorageManager sm = (StorageManager) c.getSystemService(Context.STORAGE_SERVICE);
            final StorageVolume v = sm != null ? sm.getStorageVolume(new File(path)) : null;
            // Not known to Android: right after a boot that is a card not even announced yet (the user's Hiby R4, 2026-11-15:
            // Songate opened the music before vold had said a word about the card, and was ended). Later, an odd place of one's
            // own: never held back for good
            if (v == null) return !path.startsWith("/storage/") || android.os.SystemClock.elapsedRealtime() > 120000;
            final String st = v.getState();
            return Environment.MEDIA_MOUNTED.equals(st) || Environment.MEDIA_MOUNTED_READ_ONLY.equals(st);
        } catch (Throwable t) {
            return true;
        }
    }

    /** Whether each of the same volumes comes out (a card, a USB drive): "1" or "0" a line, in volumes()'s order. Songate names
     *  them by it (Internal storage, SD card): platform_android.cpp volumeLabel. */
    public static String volumesRemovable() {
        StringBuilder out = new StringBuilder();
        final java.util.List<StorageVolume> vs = mountedVolumes();
        if (vs != null) {
            for (StorageVolume v : vs) out.append(v.isRemovable() ? '1' : '0').append('\n');
            return out.toString();
        }
        StorageManager sm = (StorageManager) self.getSystemService(Context.STORAGE_SERVICE);
        for (File f : self.getExternalFilesDirs(null)) {
            if (f == null) continue;
            if (f.getAbsolutePath().indexOf("/Android/data/") <= 0) continue;
            boolean removable = false;
            try {
                StorageVolume v = sm.getStorageVolume(f);
                if (v != null) removable = v.isRemovable();
            } catch (Exception ignored) {}
            out.append(removable ? '1' : '0').append('\n');
        }
        return out.toString();
    }

    /** What the user named this device in its settings, else its model. */
    public static String deviceName() {
        try {
            String n = Settings.Global.getString(self.getContentResolver(), "device_name");
            if (n != null && !n.isEmpty()) return n;
        } catch (Exception ignored) {}
        return Build.MODEL;
    }
}
