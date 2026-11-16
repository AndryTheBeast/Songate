/*
 * Songate · MediaButtonReceiver.java
 * The HiBy R4 (SongateOS; the user's choice A, 2026-11-15): the media buttons' receiver Android keeps for the app that last played,
 * also across a reboot. While Songate runs, the press goes to its session; before it does, Songate (the R4's home app) is opened.
 */
package io.github.andrythebeast.songate;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.view.KeyEvent;

public class MediaButtonReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context c, Intent i) {
        if (!Intent.ACTION_MEDIA_BUTTON.equals(i.getAction())) return;
        final KeyEvent e = i.getParcelableExtra(Intent.EXTRA_KEY_EVENT);
        if (e != null && SongateService.mediaButton(e)) return;
        try {
            c.startActivity(new Intent(c, SongateActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP));
        } catch (RuntimeException ignored) {}
    }
}
