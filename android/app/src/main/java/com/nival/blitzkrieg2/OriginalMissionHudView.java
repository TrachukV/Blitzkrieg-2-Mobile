package com.nival.blitzkrieg2;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.View;

import java.io.File;

final class OriginalMissionHudView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG |
            Paint.FILTER_BITMAP_FLAG);
    private final Bitmap panel;
    private final Bitmap minimapFrame;
    private Bitmap minimap;

    OriginalMissionHudView(Context context, File dataRoot) {
        super(context);
        setClickable(false);
        panel = TgaDecoder.decode(
                new File(dataRoot, "Complete/UI/Panels/MissionMain.tga"));
        minimapFrame = TgaDecoder.decode(
                new File(dataRoot, "Complete/UI/MiniMap/foreground.tga"));
    }

    void setMinimapPixels(int[] pixels, int width, int height) {
        if (pixels == null || pixels.length != width * height) {
            return;
        }
        minimap = Bitmap.createBitmap(
                pixels,
                width,
                height,
                Bitmap.Config.ARGB_8888);
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (panel == null || minimapFrame == null) {
            canvas.drawColor(0xd91e241d);
            return;
        }

        float scale = getHeight() / 180.0f;
        float leftWidth = 280.0f * scale;
        float rightWidth = 244.0f * scale;
        float centerRight = Math.max(leftWidth, getWidth() - rightWidth);
        canvas.drawBitmap(
                panel,
                new Rect(280, 0, 780, 180),
                new RectF(leftWidth, 0.0f, centerRight, getHeight()),
                paint);
        canvas.drawBitmap(
                panel,
                new Rect(780, 0, 1024, 180),
                new RectF(centerRight, 0.0f, getWidth(), getHeight()),
                paint);

        float frameWidth = 260.0f * scale;
        float frameHeight = 160.0f * scale;
        float frameTop = (getHeight() - frameHeight) * 0.5f;
        if (minimap != null) {
            Path diamond = new Path();
            diamond.moveTo(4.0f * scale, frameTop + 80.0f * scale);
            diamond.lineTo(130.0f * scale, frameTop + 2.0f * scale);
            diamond.lineTo(256.0f * scale, frameTop + 80.0f * scale);
            diamond.lineTo(130.0f * scale, frameTop + 158.0f * scale);
            diamond.close();
            int saved = canvas.save();
            canvas.clipPath(diamond);
            canvas.drawBitmap(
                    minimap,
                    null,
                    new RectF(0.0f, frameTop, frameWidth, frameTop + frameHeight),
                    paint);
            canvas.restoreToCount(saved);
        }
        canvas.drawBitmap(
                minimapFrame,
                null,
                new RectF(0.0f, frameTop, frameWidth, frameTop + frameHeight),
                paint);
    }
}
