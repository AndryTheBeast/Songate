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
    private static SongateActivity self;
    private WifiManager.MulticastLock multicast;

    @Override
    protected String[] getLibraries() {
        // FFmpeg and mpg123 are shared (LGPL): the system's linker finds them beside libmain when it loads
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        self = this;
        SongateService.attach(this);
        copyRomfs();   // before SDL starts the app's thread: the fonts are read at launch
        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi != null) {
            multicast = wifi.createMulticastLock("songate");
            multicast.setReferenceCounted(false);
            multicast.acquire();   // Devices' broadcasts and AirPlay's mDNS reach the app
        }
        super.onCreate(savedInstanceState);
        // The camera's cutouts and the rounded corners, to the app each time Android lays the window out (a turn, a fold):
        // Settings › Appearance › Screen edges decides what steps aside (App::updateEdges)
        getWindow().getDecorView().setOnApplyWindowInsetsListener((v, insets) -> {
            reportEdges(insets);
            return v.onApplyWindowInsets(insets);
        });
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
     *  (Settings › Playback; the user chose the switch, 2026-11-04): it ends as ever. */
    @Override
    protected boolean keepNativeWhenDestroyed() {
        return SongateService.playingNow() && SongateService.keepOnSwipe;
    }

    /** Leaving the front (home, another app, the screen off): the app writes its settings, session and caches now, on its own
     *  thread, before SDL pauses it; Android may end it later without another word. */
    @Override
    protected void onPause() {
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
        if (multicast != null && multicast.isHeld()) multicast.release();
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

    /** The shared volumes, one a line: the phone's own first, then each card that is in. Their app folders say where they are. */
    public static String volumes() {
        StringBuilder out = new StringBuilder();
        for (File f : self.getExternalFilesDirs(null)) {
            if (f == null) continue;   // a card being taken out
            String p = f.getAbsolutePath();
            int cut = p.indexOf("/Android/data/");
            if (cut > 0) out.append(p, 0, cut).append('\n');
        }
        return out.toString();
    }

    /** Whether each of the same volumes comes out (a card, a USB drive): "1" or "0" a line, in volumes()'s order. Songate names
     *  them by it (Internal storage, SD card): platform_android.cpp volumeLabel. */
    public static String volumesRemovable() {
        StringBuilder out = new StringBuilder();
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
