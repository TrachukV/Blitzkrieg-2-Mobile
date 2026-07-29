package com.nival.blitzkrieg2;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

final class OriginalMissionHudView extends View {
    private static final class SelectedMember {
        final int id;
        final String kind;
        final int hitPoints;
        final int maxHitPoints;

        SelectedMember(
                int id,
                String kind,
                int hitPoints,
                int maxHitPoints) {
            this.id = id;
            this.kind = kind;
            this.hitPoints = hitPoints;
            this.maxHitPoints = maxHitPoints;
        }
    }

    private static final class MemberCardTarget {
        final RectF bounds;
        final int unitId;

        MemberCardTarget(RectF bounds, int unitId) {
            this.bounds = bounds;
            this.unitId = unitId;
        }
    }

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
    private String selectedMembersValue = "";
    private final List<SelectedMember> selectedMembers =
            new ArrayList<>();
    private final List<MemberCardTarget> memberCardTargets =
            new ArrayList<>();
    private int pressedMemberId = -1;

    OriginalMissionHudView(Context context, File dataRoot) {
        super(context);
        setClickable(true);
        panel = loadOriginalBitmap(
                context,
                dataRoot,
                "Complete/UI/Panels/MissionMain.tga");
        minimapFrame = loadOriginalBitmap(
                context,
                dataRoot,
                "Complete/UI/MiniMap/foreground.tga");
        unitPreviewFrame = loadOriginalBitmap(
                context,
                dataRoot,
                "Complete/UI/New_mission/Middle_Panels_screen001.tga");
        unitIconBackground = loadOriginalBitmap(
                context,
                dataRoot,
                "Complete/UI/Buttons/Icons/UnitIconBackground.tga");
        soldierIcon = loadOriginalBitmap(
                context,
                dataRoot,
                "Complete/UI/Buttons/Icons/Allies/Soldier.tga");
        tankIcon = loadOriginalBitmap(
                context,
                dataRoot,
                "Complete/UI/Buttons/Icons/Tank.tga");
        greenHitBar = loadOriginalBitmap(
                context,
                dataRoot,
                "Complete/UI/Buttons/HitBars/GreenUnitBar.tga");
        yellowHitBar = loadOriginalBitmap(
                context,
                dataRoot,
                "Complete/UI/Buttons/HitBars/YellowUnitBar.tga");
        redHitBar = loadOriginalBitmap(
                context,
                dataRoot,
                "Complete/UI/Buttons/HitBars/RedUnitBar.tga");
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
        String membersValue = "";
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
                } else if ("members".equals(key)) {
                    membersValue = value;
                }
            }
        }
        if (!kind.equals(selectedKind)
                || hitPoints != selectedHitPoints
                || maxHitPoints != selectedMaxHitPoints
                || !membersValue.equals(selectedMembersValue)) {
            selectedKind = kind;
            selectedHitPoints = hitPoints;
            selectedMaxHitPoints = maxHitPoints;
            selectedMembersValue = membersValue;
            selectedMembers.clear();
            for (String memberValue : membersValue.split(",")) {
                String[] parts = memberValue.split(":");
                if (parts.length != 4) {
                    continue;
                }
                selectedMembers.add(
                        new SelectedMember(
                                parseInt(parts[0]),
                                parts[1],
                                parseInt(parts[2]),
                                parseInt(parts[3])));
            }
            if (selectedMembers.isEmpty() && !kind.isEmpty()) {
                selectedMembers.add(
                        new SelectedMember(
                                -1,
                                kind,
                                hitPoints,
                                maxHitPoints));
            }
            memberCardTargets.clear();
            pressedMemberId = -1;
            invalidate();
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
            pressedMemberId = findMemberCard(
                    event.getX(),
                    event.getY());
            return pressedMemberId >= 0;
        }
        if (event.getActionMasked() == MotionEvent.ACTION_UP) {
            int releasedMemberId = findMemberCard(
                    event.getX(),
                    event.getY());
            boolean handled = pressedMemberId >= 0;
            boolean activate =
                    handled &&
                    releasedMemberId == pressedMemberId;
            int unitId = pressedMemberId;
            pressedMemberId = -1;
            if (activate) {
                performClick();
                NativeBridge.setActiveSelectedUnit(unitId);
            }
            return handled;
        }
        if (event.getActionMasked() == MotionEvent.ACTION_CANCEL) {
            boolean handled = pressedMemberId >= 0;
            pressedMemberId = -1;
            return handled;
        }
        return pressedMemberId >= 0;
    }

    @Override
    public boolean performClick() {
        super.performClick();
        return true;
    }

    private int findMemberCard(float x, float y) {
        for (MemberCardTarget target : memberCardTargets) {
            if (target.bounds.contains(x, y)) {
                return target.unitId;
            }
        }
        return -1;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        memberCardTargets.clear();
        canvas.drawColor(0xff000000);
        if (panel == null || minimapFrame == null) {
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

        float cardSize = 48.0f * scale;
        float cardLeft = previewLeft + previewWidth + 18.0f * scale;
        float cardTop = 20.0f * scale;
        if (cardLeft + cardSize >= centerRight) {
            return;
        }
        float cardGap = 6.0f * scale;
        int columns = Math.max(
                1,
                (int) ((centerRight - cardLeft + cardGap)
                        / (cardSize + cardGap)));
        for (int index = 0;
             index < selectedMembers.size() && index < 12;
             ++index) {
            SelectedMember member = selectedMembers.get(index);
            int row = index / columns;
            int column = index % columns;
            float left =
                    cardLeft + column * (cardSize + cardGap);
            float top =
                    cardTop + row * (cardSize + 10.0f * scale);
            if (top + cardSize + 8.0f * scale > getHeight()) {
                break;
            }
            drawSelectedMemberCard(
                    canvas,
                    scale,
                    left,
                    top,
                    cardSize,
                    member,
                    index == 0);
            if (member.id >= 0) {
                memberCardTargets.add(
                        new MemberCardTarget(
                                new RectF(
                                        left,
                                        top,
                                        left + cardSize,
                                        top + cardSize + 8.0f * scale),
                                member.id));
            }
        }
    }

    private void drawSelectedMemberCard(
            Canvas canvas,
            float scale,
            float cardLeft,
            float cardTop,
            float cardSize,
            SelectedMember member,
            boolean active) {
        Bitmap icon = "tank".equals(member.kind)
                ? tankIcon
                : soldierIcon;
        if (icon == null ||
            unitIconBackground == null ||
            member.maxHitPoints <= 0) {
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
        if (active) {
            Paint.Style previousStyle = paint.getStyle();
            float previousStrokeWidth = paint.getStrokeWidth();
            int previousColor = paint.getColor();
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(Math.max(2.0f, 2.0f * scale));
            paint.setColor(0xffe5d94c);
            canvas.drawRect(card, paint);
            paint.setColor(previousColor);
            paint.setStrokeWidth(previousStrokeWidth);
            paint.setStyle(previousStyle);
        }

        float healthRatio = Math.max(
                0.0f,
                Math.min(
                        1.0f,
                        (float) member.hitPoints
                                / (float) member.maxHitPoints));
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

    private static Bitmap loadOriginalBitmap(
            Context context,
            File dataRoot,
            String relativePath) {
        return TgaDecoder.decode(
                context,
                new File(dataRoot, relativePath),
                relativePath);
    }
}
