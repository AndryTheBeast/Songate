// Songate for Android · ApkProvider.java
// An update downloaded by Settings › Updates, handed to Android's own installer (SongateActivity.installApk): the installer
// reads it through this provider, by a permission granted for that one request. It serves nothing else: only a file named
// SongateAND-….apk in Songate's own updates folder (updater.cpp downloads it there and checks its size and checksum first).
package io.github.andrythebeast.songate;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

public class ApkProvider extends ContentProvider {
    public static final String AUTHORITY = "io.github.andrythebeast.songate.apk";
    public static final String MIME = "application/vnd.android.package-archive";

    /** The update's file for this address, or null: its name alone is taken from the address, never a path. */
    private File fileFor(Uri uri) {
        final String name = uri.getLastPathSegment();
        if (name == null || !name.startsWith("SongateAND-") || !name.endsWith(".apk") || name.contains("/") || name.contains("..")) return null;
        final File f = new File(new File(getContext().getFilesDir(), "Songate/updates"), name);
        return f.isFile() ? f : null;
    }

    @Override public boolean onCreate() { return true; }
    @Override public String getType(Uri uri) { return MIME; }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        final File f = fileFor(uri);
        if (f == null || !"r".equals(mode)) throw new FileNotFoundException(String.valueOf(uri));
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        final File f = fileFor(uri);
        final String[] cols = projection != null ? projection : new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE };
        final MatrixCursor c = new MatrixCursor(cols, 1);
        if (f == null) return c;
        final Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) row[i] = f.getName();
            else if (OpenableColumns.SIZE.equals(cols[i])) row[i] = f.length();
        }
        c.addRow(row);
        return c;
    }

    // Read only: nothing is added, changed or removed through it
    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String selection, String[] selectionArgs) { return 0; }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) { return 0; }
}
