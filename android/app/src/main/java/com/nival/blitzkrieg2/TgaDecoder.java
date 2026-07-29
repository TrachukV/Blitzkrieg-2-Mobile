package com.nival.blitzkrieg2;

import android.graphics.Bitmap;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;

final class TgaDecoder {
    private TgaDecoder() {
    }

    static Bitmap decode(File file) {
        if (file == null || !file.isFile()) {
            return null;
        }
        try (FileInputStream input = new FileInputStream(file)) {
            long length = file.length();
            if (length <= 0 || length > Integer.MAX_VALUE) {
                return null;
            }
            byte[] data = new byte[(int) length];
            int offset = 0;
            while (offset < data.length) {
                int count = input.read(data, offset, data.length - offset);
                if (count < 0) {
                    return null;
                }
                offset += count;
            }
            return decode(data);
        } catch (IOException ignored) {
            return null;
        }
    }

    private static int littleEndian16(byte[] data, int offset) {
        return (data[offset] & 0xff) | ((data[offset + 1] & 0xff) << 8);
    }

    private static Bitmap decode(byte[] data) {
        if (data.length < 18) {
            return null;
        }
        int idLength = data[0] & 0xff;
        int colorMapType = data[1] & 0xff;
        int imageType = data[2] & 0xff;
        int width = littleEndian16(data, 12);
        int height = littleEndian16(data, 14);
        int bitsPerPixel = data[16] & 0xff;
        int descriptor = data[17] & 0xff;
        if (colorMapType != 0 ||
                (imageType != 2 && imageType != 10) ||
                (bitsPerPixel != 24 && bitsPerPixel != 32) ||
                width <= 0 ||
                height <= 0) {
            return null;
        }

        int bytesPerPixel = bitsPerPixel / 8;
        int offset = 18 + idLength;
        int pixelCount = width * height;
        int[] source = new int[pixelCount];
        int written = 0;
        while (written < pixelCount && offset < data.length) {
            int count = 1;
            boolean repeated = false;
            if (imageType == 10) {
                int packet = data[offset++] & 0xff;
                repeated = (packet & 0x80) != 0;
                count = (packet & 0x7f) + 1;
            }
            if (repeated) {
                if (offset + bytesPerPixel > data.length) {
                    return null;
                }
                int color = readColor(data, offset, bytesPerPixel);
                offset += bytesPerPixel;
                for (int index = 0;
                     index < count && written < pixelCount;
                     ++index) {
                    source[written++] = color;
                }
            } else {
                for (int index = 0;
                     index < count && written < pixelCount;
                     ++index) {
                    if (offset + bytesPerPixel > data.length) {
                        return null;
                    }
                    source[written++] =
                            readColor(data, offset, bytesPerPixel);
                    offset += bytesPerPixel;
                }
            }
        }
        if (written != pixelCount) {
            return null;
        }

        boolean topOrigin = (descriptor & 0x20) != 0;
        boolean rightOrigin = (descriptor & 0x10) != 0;
        int[] oriented = new int[pixelCount];
        for (int y = 0; y < height; ++y) {
            int sourceY = topOrigin ? y : height - 1 - y;
            for (int x = 0; x < width; ++x) {
                int sourceX = rightOrigin ? width - 1 - x : x;
                oriented[y * width + x] =
                        source[sourceY * width + sourceX];
            }
        }
        return Bitmap.createBitmap(
                oriented,
                width,
                height,
                Bitmap.Config.ARGB_8888);
    }

    private static int readColor(
            byte[] data,
            int offset,
            int bytesPerPixel) {
        int blue = data[offset] & 0xff;
        int green = data[offset + 1] & 0xff;
        int red = data[offset + 2] & 0xff;
        int alpha = bytesPerPixel == 4 ? data[offset + 3] & 0xff : 0xff;
        return (alpha << 24) | (red << 16) | (green << 8) | blue;
    }
}
