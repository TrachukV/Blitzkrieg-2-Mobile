package com.nival.blitzkrieg2;

import android.graphics.Bitmap;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;

/**
 * Reads the uncompressed DDS the shipped unit icons are stored as.
 *
 * <p>Every icon_texture.dds beside a unit's stats is written by the original
 * asset pipeline as TF_8888 with no mip chain and no block compression, so
 * only the straight 32 and 24 bit layouts are handled here. Anything else is
 * left to the native texture path, which the world renderer already uses.
 */
final class DdsDecoder {
    private static final int HEADER_SIZE = 128;
    private static final int MAGIC = 0x20534444; // "DDS "
    private static final int PIXEL_FORMAT_RGB = 0x40;

    private DdsDecoder() {
    }

    static Bitmap decode(File file) {
        if (file == null || !file.isFile()) {
            return null;
        }
        long length = file.length();
        if (length <= HEADER_SIZE || length > Integer.MAX_VALUE) {
            return null;
        }
        byte[] data = new byte[(int) length];
        try (FileInputStream input = new FileInputStream(file)) {
            int offset = 0;
            while (offset < data.length) {
                int count = input.read(data, offset, data.length - offset);
                if (count < 0) {
                    return null;
                }
                offset += count;
            }
        } catch (IOException ignored) {
            return null;
        }
        return decode(data);
    }

    static Bitmap decode(byte[] data) {
        if (data == null || data.length <= HEADER_SIZE) {
            return null;
        }
        if (readInt(data, 0) != MAGIC) {
            return null;
        }
        int height = readInt(data, 12);
        int width = readInt(data, 16);
        int pixelFormatFlags = readInt(data, 80);
        int bitCount = readInt(data, 88);
        int redMask = readInt(data, 92);
        int blueMask = readInt(data, 100);
        int alphaMask = readInt(data, 104);
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
            return null;
        }
        if ((pixelFormatFlags & PIXEL_FORMAT_RGB) == 0) {
            return null;
        }
        int bytesPerPixel = bitCount / 8;
        if (bytesPerPixel != 4 && bytesPerPixel != 3) {
            return null;
        }
        int required = HEADER_SIZE + width * height * bytesPerPixel;
        if (data.length < required) {
            return null;
        }
        // The masks say which byte is which; the pipeline writes BGRA, but a
        // file that says otherwise is honoured rather than assumed.
        boolean blueFirst = blueMask == 0x000000ff || blueMask == 0;
        boolean hasAlpha = bytesPerPixel == 4 && alphaMask != 0;
        int[] pixels = new int[width * height];
        int source = HEADER_SIZE;
        for (int index = 0; index < pixels.length; ++index) {
            int first = data[source] & 0xff;
            int green = data[source + 1] & 0xff;
            int third = data[source + 2] & 0xff;
            int alpha = hasAlpha ? data[source + 3] & 0xff : 0xff;
            source += bytesPerPixel;
            int red = blueFirst ? third : first;
            int blue = blueFirst ? first : third;
            pixels[index] = (alpha << 24) | (red << 16) | (green << 8) | blue;
        }
        if (redMask == 0 && blueMask == 0) {
            // A file with no masks at all is not something to guess about.
            return null;
        }
        return Bitmap.createBitmap(
                pixels, width, height, Bitmap.Config.ARGB_8888);
    }

    private static int readInt(byte[] data, int offset) {
        return (data[offset] & 0xff)
                | ((data[offset + 1] & 0xff) << 8)
                | ((data[offset + 2] & 0xff) << 16)
                | ((data[offset + 3] & 0xff) << 24);
    }
}
