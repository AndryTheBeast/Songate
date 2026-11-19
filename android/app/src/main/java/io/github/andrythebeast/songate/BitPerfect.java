/*
 * Songate · BitPerfect.java
 * Bit-perfect output (Android 14's mixer attributes; the user's ROM prompt, 2026-11-15): the device Songate's music plays to is
 * asked whether it takes the song's own rate and bit depth untouched (MIXER_BEHAVIOR_BIT_PERFECT); if so Songate claims it for
 * itself before its stream opens, and lets it go when it is done. Today a USB DAC offers it on any Android 14; SongateOS will
 * offer the HiBy R4's own outputs the same way. Everything else: nothing offered, nothing claimed, the normal path.
 * Called by platform_android.cpp (Player::load) on the app's thread.
 */
package io.github.andrythebeast.songate;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioMixerAttributes;
import android.os.Build;
import android.util.Log;

import java.util.List;

final class BitPerfect {
    private BitPerfect() {}

    private static AudioDeviceInfo claimed;   // the device it is claimed on (null: none)
    private static boolean listening;
    static native void nativeBitPerfect(boolean on);

    private static AudioAttributes attrs() {
        return new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build();
    }

    /** Claims bit-perfect for a song of this rate and bit depth on the output media plays to now: 16 (16-bit), 24 (packed 24-bit) or
     *  32 (32-bit) as the stream must then be opened; 0 when it is not offered (the normal path; anything claimed before let go). */
    static synchronized int begin(int rate, int bits) {
        if (Build.VERSION.SDK_INT < 34 || HibyR4.context == null) return 0;
        try {
            final AudioManager am = HibyR4.context.getSystemService(AudioManager.class);
            if (am == null) return 0;
            final AudioAttributes a = attrs();
            final List<AudioDeviceInfo> devices = am.getAudioDevicesForAttributes(a);
            final AudioDeviceInfo dev = devices == null || devices.isEmpty() ? null : devices.get(0);
            if (dev == null) { end(); return 0; }
            listen(am);
            // the encodings that carry this song whole: its own first, then the larger containers (a 16 or 24-bit sample in 32 bits
            // loses nothing; many USB DACs offer 32-bit alone: the SongateOS session's findings, 2026-11-15)
            final int[] want = bits <= 16 ? new int[] { AudioFormat.ENCODING_PCM_16BIT, AudioFormat.ENCODING_PCM_24BIT_PACKED, AudioFormat.ENCODING_PCM_32BIT }
                             : bits <= 24 ? new int[] { AudioFormat.ENCODING_PCM_24BIT_PACKED, AudioFormat.ENCODING_PCM_32BIT }
                                          : new int[] { AudioFormat.ENCODING_PCM_32BIT };
            final List<AudioMixerAttributes> offered = am.getSupportedMixerAttributes(dev);
            for (int enc : want) {
                for (AudioMixerAttributes m : offered) {
                    final AudioFormat f = m.getFormat();
                    // stereo as a layout, or as the first two channels by index (USB DACs often say it both ways)
                    final boolean stereo = f.getChannelMask() == AudioFormat.CHANNEL_OUT_STEREO || f.getChannelIndexMask() == 0x3;
                    if (m.getMixerBehavior() != AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT || f.getSampleRate() != rate || f.getEncoding() != enc
                        || !stereo) continue;
                    if (claimed != null && claimed.getId() != dev.getId()) end();
                    if (am.setPreferredMixerAttributes(a, dev, m)) {
                        claimed = dev;
                        Log.i("Songate", "SongateAudio: bit-perfect on " + dev.getProductName() + " (type " + dev.getType() + "), " + rate + " Hz, encoding " + enc);
                        return enc == AudioFormat.ENCODING_PCM_16BIT ? 16 : enc == AudioFormat.ENCODING_PCM_24BIT_PACKED ? 24 : 32;
                    }
                }
            }
            Log.i("Songate", "SongateAudio: bit-perfect not offered for " + rate + " Hz " + bits + "-bit on " + dev.getProductName() + " (type " + dev.getType() + ", "
                             + offered.size() + " mixer attributes)");
        } catch (Throwable t) {
            Log.w("Songate", "SongateAudio: bit-perfect: " + t);
        }
        end();
        return 0;
    }

    /** Media still plays to the device it is claimed on: false once it has moved (Bluetooth connected, the wired output's claim left
     *  set: Android's listener says nothing then; the user's R4, 2026-11-17: still "bit-perfect" over Bluetooth). */
    static synchronized boolean routed() {
        if (claimed == null || Build.VERSION.SDK_INT < 34 || HibyR4.context == null) return false;
        try {
            final AudioManager am = HibyR4.context.getSystemService(AudioManager.class);
            if (am == null) return false;
            final List<AudioDeviceInfo> devices = am.getAudioDevicesForAttributes(attrs());
            return devices != null && !devices.isEmpty() && devices.get(0).getId() == claimed.getId();
        } catch (Throwable t) {
            return false;
        }
    }

    /** Lets it go: the device back to Android's mixer for Songate too. */
    static synchronized void end() {
        if (claimed == null || Build.VERSION.SDK_INT < 34 || HibyR4.context == null) { claimed = null; return; }
        try {
            final AudioManager am = HibyR4.context.getSystemService(AudioManager.class);
            if (am != null) am.clearPreferredMixerAttributes(attrs(), claimed);
            Log.i("Songate", "SongateAudio: bit-perfect let go");
        } catch (Throwable t) {
            Log.w("Songate", "SongateAudio: bit-perfect end: " + t);
        }
        claimed = null;
    }

    /** Android may take it away (the device unplugged, another app claiming it): the player hears, and the badge stops. */
    private static void listen(AudioManager am) {
        if (listening || Build.VERSION.SDK_INT < 34) return;
        listening = true;
        am.addOnPreferredMixerAttributesChangedListener(HibyR4.context.getMainExecutor(), (a, dev, mixer) -> {
            final boolean on = mixer != null && mixer.getMixerBehavior() == AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT;
            Log.i("Songate", "SongateAudio: Android says bit-perfect " + (on ? "on" : "off") + " for " + (dev != null ? dev.getProductName() : "?"));
            // only the device it is claimed on now: one let go (the 4.4 mm's route when the 3.5 mm went in mid-song) says "off" after
            // the new claim, which marked the song off though it played bit-perfect (the user's R4, 2026-11-19)
            synchronized (BitPerfect.class) {
                if (dev != null && (claimed == null || claimed.getId() != dev.getId())) return;
            }
            try {
                nativeBitPerfect(on);
            } catch (UnsatisfiedLinkError ignored) {
            }
        });
    }
}
