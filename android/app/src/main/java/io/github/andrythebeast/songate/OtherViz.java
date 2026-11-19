package io.github.andrythebeast.songate;

import android.media.audiofx.Visualizer;
import android.util.Log;

/**
 * The spectrum for another app's music (Now playing's bars while Spotify and the like play; the user chose "asked in place",
 * 2026-11-19): Android's whole output, read through a Visualizer on session 0 (the output mix), which needs Songate's audio
 * recording permission. Only while another app's song is shown, Songate is in front and the switch is on; let go otherwise.
 * Songate's own music never comes this way (bit-perfect bypasses the mix): its bars come from its own player.
 * A device that refuses session 0 (the constructor throws, or it will not start) gives nothing: the bars stay down.
 */
final class OtherViz {
    private OtherViz() {}

    private static Visualizer viz;
    private static byte[] buf;
    private static int rate;
    private static long failedAt = -1;

    /** The latest slice of what plays, 8-bit (128 = silence), into the array that is returned (null: nothing). rateHz[0]: its rate */
    static synchronized byte[] wave(int[] rateHz) {
        if (viz == null) {
            if (failedAt >= 0 && android.os.SystemClock.uptimeMillis() - failedAt < 5000) return null;   // (tried again every 5 s)
            try {
                viz = new Visualizer(0);
                viz.setEnabled(false);
                final int[] range = Visualizer.getCaptureSizeRange();
                viz.setCaptureSize(Math.min(1024, range[1]));
                viz.setScalingMode(Visualizer.SCALING_MODE_NORMALIZED);
                if (viz.setEnabled(true) != Visualizer.SUCCESS) throw new IllegalStateException("not started");
                buf = new byte[viz.getCaptureSize()];
                rate = viz.getSamplingRate() / 1000;
                failedAt = -1;
                Log.i("Songate", "other apps' spectrum: on (" + buf.length + " samples at " + rate + " Hz)");
            } catch (Throwable t) {
                Log.w("Songate", "other apps' spectrum: not available: " + t);
                stop();
                failedAt = android.os.SystemClock.uptimeMillis();
                return null;
            }
        }
        try {
            if (viz.getWaveForm(buf) != Visualizer.SUCCESS) return null;
        } catch (Throwable t) { return null; }
        rateHz[0] = rate;
        return buf;
    }

    static synchronized void stop() {
        if (viz == null) return;
        try { viz.setEnabled(false); } catch (Throwable ignored) {}
        try { viz.release(); } catch (Throwable ignored) {}
        viz = null;
        Log.i("Songate", "other apps' spectrum: off");
    }
}
