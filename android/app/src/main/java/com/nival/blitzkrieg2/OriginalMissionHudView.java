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
    private final Bitmap unitPreviewFrame;
    private final Bitmap unitIconBackground;
    private final Bitmap soldierIcon;
    private final Bitmap tankIcon;
    private final Bitmap greenHitBar;
    private final Bitmap yellowHitBar;
    private final Bitmap redHitBar;
    private Bitmap minimap;
    private String selectedKind = "";
    private int selectedHitPoints;
    private int selectedMaxHitPoints;

    OriginalMissionHudView(Context context, File dataRoot) {
        super(context);
        setClickable(false);
        panel = TgaDecoder.decode(
                new File(dataRoot, "Complete/UI/Panels/MissionMain.tga"));
        minimapFrame = TgaDecoder.decode(
                new File(dataRoot, "Complete/UI/MiniMap/foreground.tga"));
        unitPreviewFrame = TgaDecoder.decode(
                new File(
                        dataRoot,
                        "Complete/UI/New_mission/Middle_Panels_screen001.tga"));
        unitIconBackground = TgaDecoder.decode(
                new File(
                        dataRoot,
                        "Complete/UI/Buttons/Icons/UnitIconBackground.tga"));
        soldierIcon = TgaDecoder.decode(
                new File(
                        dataRoot,
                        "Complete/UI/Buttons/Icons/Allies/Soldier.tga"));
        tankIcon = TgaDecoder.decode(
                new File(
                        dataRoot,
                        "Complete/UI/Buttons/Icons/Tank.tga"));
        greenHitBar = TgaDecoder.decode(
                new File(
                        dataRoot,
                        "Complete/UI/Buttons/HitBars/GreenUnitBar.tga"));
        yellowHitBar = TgaDecoder.decode(
                new File(
                        dataRoot,
                        "Complete/UI/Buttons/HitBars/YellowUnitBar.tga"));
        redHitBar = TgaDecoder.decode(
                new File(
                        dataRoot,
                        "Complete/UI/Buttons/HitBars/RedUnitBar.tga"));
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

    void setSelectedUnitSnapshot(String snapshot) {
        String kind = "";
        int hitPoints = 0;
        int maxHitPoints = 0;
        if (snapshot != null && !snapshot.isEmpty()) {
            String[] fields = snapshot.split(";");
            for (String field : fields) {
                int separator = field.indexOf('=');
                if (separator <= 0 || separator + 1 >= field.length()) {
                    continue;
                }
                String key = field.substring(0, separator);
                String value = field.substring(separator + 1);
                if ("kind".equals(key)) {
                    kind = value;
                } else if ("hp".equals(key)) {
                    hitPoints = parseInt(value);
                } else if ("max_hp".equals(key)) {
                    maxHitPoints = parseInt(value);
                }
            }
        }
        if (!kind.equals(selectedKind)
                || hitPoints != selectedHitPoints
                || maxHitPoints != selectedMaxHitPoints) {
            selectedKind = kind;
            selectedHitPoints = hitPoints;
            selectedMaxHitPoints = maxHitPoints;
            invalidate();
        }
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
        drawSelectedUnit(canvas, scale, leftWidth, centerRight);
    }

    private void drawSelectedUnit(
            Canvas canvas,
            float scale,
            float leftWidth,
            float centerRight) {
        if (selectedKind.isEmpty()) {
            return;
        }
        Bitmap icon = "tank".equals(selectedKind) ? tankIcon : soldierIcon;
        if (icon == null || unitIconBackground == null) {
            return;
        }

        float previewLeft = leftWidth + 14.0f * scale;
        float previewWidth = 176.0f * scale;
        float previewHeight = 148.0f * scale;
        float previewTop = (getHeight() - previewHeight) * 0.5f;
        if (unitPreviewFrame != null
                && previewLeft + previewWidth < centerRight) {
            canvas.drawBitmap(
                    unitPreviewFrame,
                    null,
                    new RectF(
                            previewLeft,
                            previewTop,
                            previewLeft + previewWidth,
                            previewTop + previewHeight),
                    paint);
        }

        float previewIconSize = 76.0f * scale;
        float previewIconLeft =
                previewLeft + (previewWidth - previewIconSize) * 0.5f;
        float previewIconTop =
                previewTop + (previewHeight - previewIconSize) * 0.44f;
        canvas.drawBitmap(
                icon,
                null,
                new RectF(
                        previewIconLeft,
                        previewIconTop,
                        previewIconLeft + previewIconSize,
                        previewIconTop + previewIconSize),
                paint);

        float cardSize = 58.0f * scale;
        float cardLeft = previewLeft + previewWidth + 18.0f * scale;
        float cardTop = 30.0f * scale;
        if (cardLeft + cardSize >= centerRight) {
            return;
        }
        RectF card = new RectF(
                cardLeft,
                cardTop,
                cardLeft + cardSize,
                cardTop + cardSize);
        canvas.drawBitmap(unitIconBackground, null, card, paint);
        float inset = 4.0f * scale;
        canvas.drawBitmap(
                icon,
                null,
                new RectF(
                        card.left + inset,
                        card.top + inset,
                        card.right - inset,
                        card.bottom - inset),
                paint);

        float healthRatio = selectedMaxHitPoints <= 0
                ? 0.0f
                : Math.max(
                        0.0f,
                        Math.min(
                                1.0f,
                                (float) selectedHitPoints
                                        / (float) selectedMaxHitPoints));
        Bitmap hitBar = healthRatio > 0.6f
                ? greenHitBar
                : (healthRatio > 0.3f ? yellowHitBar : redHitBar);
        if (hitBar == null) {
            return;
        }
        float barTop = card.bottom + 3.0f * scale;
        float barHeight = 5.0f * scale;
        float barWidth = cardSize * healthRatio;
        int sourceWidth = Math.max(
                1,
                Math.round(hitBar.getWidth() * healthRatio));
        canvas.drawBitmap(
                hitBar,
                new Rect(0, 0, sourceWidth, hitBar.getHeight()),
                new RectF(
                        card.left,
                        barTop,
                        card.left + barWidth,
                        barTop + barHeight),
                paint);
    }

    private static int parseInt(String value) {
        try {
            return Integer.parseInt(value);
        } catch (NumberFormatException ignored) {
            return 0;
        }
    }
}
