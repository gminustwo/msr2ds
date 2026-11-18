package io.github.gminustwo.msrnative;

import android.content.ContentResolver;
import android.net.Uri;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * GPU driver packages for the game (the launcher's GPU driver: launcher.cpp, vk_load.c): a .zip as Azahar, Eden and other
 * emulators take them, with meta.json (its libraryName: the driver's library) and the driver's libraries, unpacked into
 * the app's private folder (files/gpu_drivers/name), where Android lets the app load code from. libadrenotools loads
 * the driver from there in place of the phone's own.
 */
final class GpuDrivers {
    private static final long MAX_BYTES = 512L << 20;   // unpacked: drivers are tens of MB

    private GpuDrivers() {}

    /** The package at {@code uri} unpacked into {@code root}: {its folder's name, ""}, or {"", why not}. */
    static String[] install(ContentResolver resolver, Uri uri, File root) {
        final File unpacked = new File(root, ".installing");
        deleteTree(unpacked);
        if (!unpacked.mkdirs()) {
            return new String[] {"", "Could not make a folder for the driver in the app's storage."};
        }
        try (InputStream in = resolver.openInputStream(uri);
             ZipInputStream zip = new ZipInputStream(new BufferedInputStream(in))) {
            final byte[] buf = new byte[1 << 16];
            long total = 0;
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null; ) {
                if (entry.isDirectory()) {
                    continue;
                }
                final String name = new File(entry.getName()).getName();   // flat: no folders, nothing outside
                if (name.isEmpty() || name.startsWith(".")) {
                    continue;
                }
                try (OutputStream out = new FileOutputStream(new File(unpacked, name))) {
                    for (int n; (n = zip.read(buf)) > 0; ) {
                        total += n;
                        if (total > MAX_BYTES) {
                            throw new IOException("over 512 MB unpacked");
                        }
                        out.write(buf, 0, n);
                    }
                }
            }
        } catch (IOException | SecurityException | IllegalArgumentException e) {
            deleteTree(unpacked);
            return new String[] {"", "That file could not be read as a .zip (" + e.getMessage() + ")."};
        }
        final File meta = new File(unpacked, "meta.json");
        String name, version, library;
        try {
            final JSONObject json = new JSONObject(readText(meta));
            name = json.optString("name").trim();
            version = json.optString("driverVersion").trim();
            library = json.optString("libraryName").trim();
        } catch (IOException | JSONException e) {
            deleteTree(unpacked);
            return new String[] {"", "That .zip has no readable meta.json: it is not a driver package made for emulators."};
        }
        if (library.isEmpty() || library.contains("/") || !new File(unpacked, library).isFile()) {
            deleteTree(unpacked);
            return new String[] {"", "That driver package has no driver library (" +
                                 (library.isEmpty() ? "its meta.json names none" : library + " is not in it") + ")."};
        }
        final String id = folderName(name.isEmpty() ? library : version.isEmpty() ? name : name + "-" + version);
        final File folder = new File(root, id);
        deleteTree(folder);   // the same driver again: in place of the one there
        if (!unpacked.renameTo(folder)) {
            deleteTree(unpacked);
            return new String[] {"", "Could not put the driver in place in the app's storage."};
        }
        final File[] files = folder.listFiles();
        if (files != null) {
            for (File f : files) {
                f.setWritable(false, false);   // code loaded at run time is kept read-only (Android 14's rule)
            }
        }
        return new String[] {id, ""};
    }

    /** A folder name from the driver's name and version: letters, digits, '.', '-' and '_' only */
    private static String folderName(String text) {
        final StringBuilder out = new StringBuilder();
        for (int i = 0; i < text.length() && out.length() < 64; i++) {
            final char c = text.charAt(i);
            final boolean plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                  c == '.' || c == '-' || c == '_';
            out.append(plain ? c : '_');
        }
        while (out.length() > 0 && out.charAt(0) == '.') {
            out.deleteCharAt(0);
        }
        return out.length() > 0 ? out.toString() : "driver";
    }

    private static String readText(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) {
            final byte[] data = new byte[(int) Math.min(file.length(), 1 << 20)];
            int n = 0;
            for (int r; n < data.length && (r = in.read(data, n, data.length - n)) > 0; ) {
                n += r;
            }
            return new String(data, 0, n, StandardCharsets.UTF_8);
        }
    }

    private static void deleteTree(File file) {
        final File[] files = file.listFiles();
        if (files != null) {
            for (File f : files) {
                deleteTree(f);
            }
        }
        file.delete();
    }
}
