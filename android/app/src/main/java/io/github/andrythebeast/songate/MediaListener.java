package io.github.andrythebeast.songate;

import android.service.notification.NotificationListenerService;

/**
 * Songate's notification listener: it reads no notification. Allowed in Android's Notification access, it is what lets Songate
 * see other apps' media sessions (MediaSessionManager.getActiveSessions), for Settings › Playback › Other apps (OtherApps.java).
 */
public class MediaListener extends NotificationListenerService {
}
