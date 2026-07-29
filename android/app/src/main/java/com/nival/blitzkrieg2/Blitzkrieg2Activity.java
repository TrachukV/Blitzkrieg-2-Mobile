package com.nival.blitzkrieg2;

import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.GridLayout;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.google.androidgamesdk.GameActivity;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

public final class Blitzkrieg2Activity extends GameActivity {
    public static final String EXTRA_MISSION_ID = "com.nival.blitzkrieg2.MISSION_ID";
    public static final String EXTRA_DIFFICULTY = "com.nival.blitzkrieg2.DIFFICULTY";
    private final Handler outcomeHandler = new Handler(Looper.getMainLooper());
    private LinearLayout outcomePanel;
    private LinearLayout missionMenu;
    private TextView outcomeTitle;
    private TextView missionTitle;
    private TextView missionStatus;
    private TextView pauseIndicator;
    private OriginalMissionHudView originalHud;
    private GridLayout commandGrid;
    private ImageButton moveCommandButton;
    private ImageButton attackCommandButton;
    private ImageButton rotateCommandButton;
    private ImageButton spyglassCommandButton;
    private ImageButton clearMinesCommandButton;
    private ImageButton placeMinesCommandButton;
    private ImageButton buildTrenchesCommandButton;
    private File hudDataRoot;
    private String displayedCommandSnapshot = "";
    private boolean outcomePolling;
    private int hudPollCount;
    private final Runnable outcomePoll = new Runnable() {
        @Override
        public void run() {
            if (!outcomePolling) {
                return;
            }
            if (missionTitle != null) {
                String missionId = NativeBridge.getCurrentMissionId();
                if (missionId != null && !missionId.isEmpty()) {
                    missionTitle.setText(missionLabel(missionId));
                }
            }
            if (missionStatus != null) {
                String status = NativeBridge.getMissionHudStatus();
                if (status == null) {
                    status = "";
                }
                missionStatus.setText(status);
            }
            String selectedUnitSnapshot =
                    NativeBridge.getSelectedUnitHudSnapshot();
            if (originalHud != null) {
                originalHud.setSelectedUnitSnapshot(selectedUnitSnapshot);
                if ((hudPollCount++ & 3) == 0) {
                    int[] pixels =
                            NativeBridge.getMissionMinimapArgb(192, 192);
                    if (pixels != null) {
                        originalHud.setMinimapPixels(pixels, 192, 192);
                    }
                }
            }
            updateActionGrid(selectedUnitSnapshot);
            updateCommandButtonState(
                    NativeBridge.getTouchCommandMode());
            syncPauseUi();
            String outcome = NativeBridge.getMissionOutcome();
            if ("won".equals(outcome)) {
                showOutcome("VICTORY", 0xffd9c46b);
            } else if ("lost".equals(outcome)) {
                showOutcome("DEFEAT", 0xffd87862);
            } else if ("progression_error".equals(outcome)) {
                showOutcome("MISSION ENDED", 0xffd87862);
            }
            outcomeHandler.postDelayed(this, 500L);
        }
    };

    static {
        System.loadLibrary("blitzkrieg2");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        NativeBridge.attachActivity(this);
        persistMissionSelectionFromIntent(getIntent());
        String externalFilesDir = getExternalFilesDir(null) == null
                ? ""
                : getExternalFilesDir(null).getAbsolutePath();
        NativeBridge.configurePaths(
                getFilesDir().getAbsolutePath(),
                getNoBackupFilesDir().getAbsolutePath(),
                externalFilesDir);
        super.onCreate(savedInstanceState);
        addContentView(
                createHud(),
                new ViewGroup.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT));
        outcomePolling = true;
        outcomeHandler.post(outcomePoll);
    }

    @Override
    protected void onDestroy() {
        outcomePolling = false;
        outcomeHandler.removeCallbacks(outcomePoll);
        super.onDestroy();
    }

    private FrameLayout createHud() {
        FrameLayout root = new FrameLayout(this);
        root.setClickable(false);
        root.setFocusable(false);

        File dataRoot = new File(getFilesDir(), "DataAndroid");
        hudDataRoot = dataRoot;
        int hudHeight = dp(112);
        FrameLayout hud = new FrameLayout(this);
        originalHud = new OriginalMissionHudView(this, dataRoot);
        hud.addView(
                originalHud,
                new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT));

        LinearLayout missionInfo = new LinearLayout(this);
        missionInfo.setOrientation(LinearLayout.VERTICAL);
        missionInfo.setGravity(Gravity.CENTER_VERTICAL);

        missionTitle = new TextView(this);
        missionTitle.setTextColor(Color.WHITE);
        missionTitle.setTextSize(14.0f);
        missionTitle.setShadowLayer(dp(2), dp(1), dp(1), Color.BLACK);
        missionTitle.setText(missionLabel());
        missionTitle.setSingleLine(true);
        missionInfo.addView(
                missionTitle,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        missionStatus = new TextView(this);
        missionStatus.setTextColor(0xffeee6c8);
        missionStatus.setTextSize(10.0f);
        missionStatus.setShadowLayer(dp(2), dp(1), dp(1), Color.BLACK);
        missionStatus.setText("Objectives: loading");
        missionStatus.setMaxLines(3);
        missionStatus.setVisibility(View.INVISIBLE);
        missionInfo.addView(
                missionStatus,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        FrameLayout.LayoutParams missionInfoParams =
                new FrameLayout.LayoutParams(
                        dp(440),
                        ViewGroup.LayoutParams.WRAP_CONTENT,
                        Gravity.TOP | Gravity.START);
        missionInfoParams.setMargins(dp(8), dp(4), 0, 0);
        root.addView(missionInfo, missionInfoParams);

        commandGrid = new GridLayout(this);
        commandGrid.setColumnCount(4);
        commandGrid.setRowCount(3);
        rebuildActionGrid(
                new HashSet<>(),
                new HashSet<>(),
                new HashMap<>());

        LinearLayout utilityTouchTargets = new LinearLayout(this);
        utilityTouchTargets.setOrientation(LinearLayout.HORIZONTAL);
        ImageButton objectives = touchOnlyButton(
                "Objectives",
                view -> missionStatus.setVisibility(
                        missionStatus.getVisibility() == View.VISIBLE
                                ? View.INVISIBLE
                                : View.VISIBLE));
        utilityTouchTargets.addView(
                objectives,
                new LinearLayout.LayoutParams(dp(32), dp(38)));

        ImageButton menu = touchOnlyButton(
                "F10 menu",
                view -> setMissionMenuVisible(
                        missionMenu.getVisibility() != View.VISIBLE));
        utilityTouchTargets.addView(
                menu,
                new LinearLayout.LayoutParams(dp(32), dp(38)));

        FrameLayout.LayoutParams commandParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER_VERTICAL | Gravity.END);
        commandParams.setMargins(0, 0, dp(8), 0);
        hud.addView(commandGrid, commandParams);

        FrameLayout.LayoutParams utilityParams =
                new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.WRAP_CONTENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT,
                        Gravity.CENTER_VERTICAL | Gravity.END);
        utilityParams.setMargins(0, 0, dp(156), 0);
        hud.addView(utilityTouchTargets, utilityParams);

        FrameLayout.LayoutParams hudParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                hudHeight,
                Gravity.BOTTOM);
        root.addView(hud, hudParams);

        pauseIndicator = new TextView(this);
        pauseIndicator.setText("PAUSED");
        pauseIndicator.setTextColor(0xffff922f);
        pauseIndicator.setTextSize(38.0f);
        pauseIndicator.setTypeface(
                Typeface.create("sans-serif-condensed", Typeface.BOLD));
        pauseIndicator.setGravity(Gravity.CENTER);
        pauseIndicator.setIncludeFontPadding(false);
        pauseIndicator.setShadowLayer(dp(3), dp(2), dp(2), Color.BLACK);
        pauseIndicator.setVisibility(View.GONE);
        FrameLayout.LayoutParams pauseParams =
                new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        Gravity.TOP);
        pauseParams.bottomMargin = hudHeight;
        root.addView(pauseIndicator, pauseParams);

        missionMenu = new LinearLayout(this);
        missionMenu.setOrientation(LinearLayout.VERTICAL);
        missionMenu.setPadding(dp(14), dp(12), dp(14), dp(12));
        missionMenu.setClickable(true);
        GradientDrawable menuBackground = new GradientDrawable();
        menuBackground.setColor(0xee211f17);
        menuBackground.setStroke(dp(1), 0xff8a8058);
        missionMenu.setBackground(menuBackground);
        missionMenu.setVisibility(View.GONE);

        Button forfeit = new Button(this);
        forfeit.setAllCaps(false);
        forfeit.setText("Surrender");
        forfeit.setTextColor(Color.WHITE);
        forfeit.setOnClickListener(view -> NativeBridge.forfeitMission());
        missionMenu.addView(
                forfeit,
                new LinearLayout.LayoutParams(dp(150), dp(44)));

        Button missions = new Button(this);
        missions.setAllCaps(false);
        missions.setText("Return to missions");
        missions.setTextColor(Color.WHITE);
        missions.setOnClickListener(view -> {
            NativeBridge.setMissionPaused(false);
            finish();
        });
        missionMenu.addView(
                missions,
                new LinearLayout.LayoutParams(dp(150), dp(44)));

        FrameLayout.LayoutParams menuParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER_HORIZONTAL | Gravity.BOTTOM);
        menuParams.bottomMargin = hudHeight + dp(10);
        root.addView(missionMenu, menuParams);

        outcomePanel = new LinearLayout(this);
        outcomePanel.setOrientation(LinearLayout.VERTICAL);
        outcomePanel.setGravity(Gravity.CENTER);
        outcomePanel.setPadding(dp(24), dp(18), dp(24), dp(18));
        outcomePanel.setClickable(true);
        GradientDrawable outcomeBackground = new GradientDrawable();
        outcomeBackground.setColor(0xee0b1110);
        outcomeBackground.setCornerRadius(dp(12));
        outcomePanel.setBackground(outcomeBackground);
        outcomePanel.setVisibility(View.GONE);

        outcomeTitle = new TextView(this);
        outcomeTitle.setTextSize(30.0f);
        outcomeTitle.setGravity(Gravity.CENTER);
        outcomePanel.addView(
                outcomeTitle,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        Button outcomeMissions = new Button(this);
        outcomeMissions.setAllCaps(false);
        outcomeMissions.setText("Return to missions");
        outcomeMissions.setTextColor(Color.WHITE);
        outcomeMissions.setOnClickListener(view -> finish());
        LinearLayout.LayoutParams outcomeButtonParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                dp(48));
        outcomeButtonParams.topMargin = dp(14);
        outcomePanel.addView(outcomeMissions, outcomeButtonParams);

        FrameLayout.LayoutParams outcomeParams = new FrameLayout.LayoutParams(
                dp(300),
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER);
        root.addView(outcomePanel, outcomeParams);
        return root;
    }

    private void updateActionGrid(String snapshot) {
        if (commandGrid == null) {
            return;
        }
        String actionsValue = snapshotField(snapshot, "actions");
        String enabledValue = snapshotField(snapshot, "enabled");
        String tiersValue = snapshotField(snapshot, "tiers");
        String commandSnapshot =
                actionsValue + ";" + enabledValue + ";" + tiersValue;
        if (commandSnapshot.equals(displayedCommandSnapshot)) {
            return;
        }
        displayedCommandSnapshot = commandSnapshot;
        rebuildActionGrid(
                parseActionSet(actionsValue),
                parseActionSet(enabledValue),
                parseActionTiers(tiersValue));
    }

    private void rebuildActionGrid(
            Set<Integer> actions,
            Set<Integer> enabledActions,
            Map<Integer, Integer> abilityTiers) {
        if (commandGrid == null || hudDataRoot == null) {
            return;
        }
        OriginalActionButtonCatalog.Spec[] slots =
                new OriginalActionButtonCatalog.Spec[12];
        List<OriginalActionButtonCatalog.Spec> abilities =
                new ArrayList<>();
        for (int action : actions) {
            OriginalActionButtonCatalog.Spec spec =
                    OriginalActionButtonCatalog.forAction(action);
            if (spec == null) {
                continue;
            }
            if (spec.ability) {
                abilities.add(spec);
            } else if (
                    spec.slot >= 1
                            && spec.slot <= slots.length
                            && slots[spec.slot - 1] == null) {
                slots[spec.slot - 1] = spec;
            }
        }
        abilities.sort((left, right) -> {
            int tierOrder = Integer.compare(
                    abilityTiers.getOrDefault(
                            left.action,
                            Integer.MAX_VALUE),
                    abilityTiers.getOrDefault(
                            right.action,
                            Integer.MAX_VALUE));
            return tierOrder != 0
                    ? tierOrder
                    : Integer.compare(left.action, right.action);
        });
        for (OriginalActionButtonCatalog.Spec ability : abilities) {
            for (int slot = 8; slot < slots.length; ++slot) {
                if (slots[slot] == null) {
                    slots[slot] = ability;
                    break;
                }
            }
        }

        commandGrid.removeAllViews();
        moveCommandButton = null;
        attackCommandButton = null;
        rotateCommandButton = null;
        spyglassCommandButton = null;
        clearMinesCommandButton = null;
        placeMinesCommandButton = null;
        buildTrenchesCommandButton = null;
        final int cellSize = dp(36);
        for (int slot = 0; slot < slots.length; ++slot) {
            FrameLayout cell = new FrameLayout(this);
            GridLayout.LayoutParams cellParams =
                    new GridLayout.LayoutParams(
                            GridLayout.spec(slot / 4),
                            GridLayout.spec(slot % 4));
            cellParams.width = cellSize;
            cellParams.height = cellSize;
            commandGrid.addView(cell, cellParams);

            OriginalActionButtonCatalog.Spec spec = slots[slot];
            if (spec == null) {
                continue;
            }
            boolean enabled = enabledActions.contains(spec.action);
            ImageButton button = originalActionButton(spec, enabled);
            cell.addView(
                    button,
                    new FrameLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT,
                            ViewGroup.LayoutParams.MATCH_PARENT));
            if (spec.action == 1) {
                moveCommandButton = button;
            } else if (spec.action == 2) {
                attackCommandButton = button;
            } else if (spec.action == 6) {
                rotateCommandButton = button;
            } else if (spec.action == 18) {
                placeMinesCommandButton = button;
            } else if (spec.action == 19) {
                clearMinesCommandButton = button;
            } else if (spec.action == 21) {
                buildTrenchesCommandButton = button;
            } else if (spec.action == 40) {
                spyglassCommandButton = button;
            }
        }
        updateCommandButtonState(NativeBridge.getTouchCommandMode());
    }

    private ImageButton originalActionButton(
            OriginalActionButtonCatalog.Spec spec,
            boolean enabled) {
        ImageButton button = new ImageButton(this);
        button.setPadding(0, 0, 0, 0);
        button.setBackgroundColor(Color.TRANSPARENT);
        button.setScaleType(ImageButton.ScaleType.FIT_CENTER);
        String iconPath = enabled
                ? spec.iconPath
                : spec.disabledIconPath();
        android.graphics.Bitmap bitmap =
                TgaDecoder.decode(new File(hudDataRoot, iconPath));
        if (bitmap == null && !enabled) {
            bitmap = TgaDecoder.decode(
                    new File(hudDataRoot, spec.iconPath));
        }
        if (bitmap != null) {
            button.setImageDrawable(
                    new BitmapDrawable(getResources(), bitmap));
        }
        button.setContentDescription(
                enabled
                        ? spec.label
                        : spec.label + " (not yet available)");
        button.setEnabled(enabled);
        button.setFocusable(enabled);
        button.setClickable(enabled);
        if (enabled) {
            button.setOnClickListener(
                    view -> performSelectedAction(spec.action));
        }
        return button;
    }

    private void performSelectedAction(int userAction) {
        if (userAction == 1) {
            toggleTouchCommandMode(1);
            return;
        }
        if (userAction == 2) {
            toggleTouchCommandMode(2);
            return;
        }
        if (userAction == 6) {
            toggleTouchCommandMode(3);
            return;
        }
        if (userAction == 40) {
            toggleTouchCommandMode(4);
            return;
        }
        if (userAction == 19) {
            toggleTouchCommandMode(5);
            return;
        }
        if (userAction == 18) {
            toggleTouchCommandMode(6);
            return;
        }
        if (userAction == 21) {
            toggleTouchCommandMode(7);
            return;
        }
        boolean performed = userAction == 39
                ? NativeBridge.stopSelectedUnit()
                : NativeBridge.performSelectedUnitAction(userAction);
        if (!performed) {
            showCommandHint("Action is not available");
        }
        updateCommandButtonState(0);
    }

    private Set<Integer> parseActionSet(String value) {
        Set<Integer> actions = new HashSet<>();
        if (value == null || value.isEmpty()) {
            return actions;
        }
        for (String item : value.split(",")) {
            try {
                actions.add(Integer.parseInt(item));
            } catch (NumberFormatException ignored) {
            }
        }
        return actions;
    }

    private Map<Integer, Integer> parseActionTiers(String value) {
        Map<Integer, Integer> tiers = new HashMap<>();
        if (value == null || value.isEmpty()) {
            return tiers;
        }
        for (String item : value.split(",")) {
            int separator = item.indexOf(':');
            if (separator <= 0 || separator + 1 >= item.length()) {
                continue;
            }
            try {
                tiers.put(
                        Integer.parseInt(item.substring(0, separator)),
                        Integer.parseInt(item.substring(separator + 1)));
            } catch (NumberFormatException ignored) {
            }
        }
        return tiers;
    }

    private String snapshotField(String snapshot, String fieldName) {
        if (snapshot == null || snapshot.isEmpty()) {
            return "";
        }
        String prefix = fieldName + "=";
        for (String field : snapshot.split(";")) {
            if (field.startsWith(prefix)) {
                return field.substring(prefix.length());
            }
        }
        return "";
    }

    private void toggleTouchCommandMode(int requestedMode) {
        int mode = NativeBridge.getTouchCommandMode() == requestedMode
                ? 0
                : requestedMode;
        if (!NativeBridge.setTouchCommandMode(mode)) {
            showCommandHint("Select a unit first");
            mode = 0;
        }
        updateCommandButtonState(mode);
    }

    private void setMissionMenuVisible(boolean visible) {
        NativeBridge.setMissionPaused(visible);
        missionMenu.setVisibility(visible ? View.VISIBLE : View.GONE);
        syncPauseUi();
        updateCommandButtonState(0);
    }

    private void syncPauseUi() {
        boolean paused = NativeBridge.isMissionPaused();
        if (pauseIndicator != null) {
            pauseIndicator.setVisibility(
                    paused ? View.VISIBLE : View.GONE);
        }
        if (!paused &&
            missionMenu != null &&
            missionMenu.getVisibility() == View.VISIBLE) {
            missionMenu.setVisibility(View.GONE);
        }
    }

    private void updateCommandButtonState(int mode) {
        if (moveCommandButton != null) {
            moveCommandButton.setAlpha(mode == 1 ? 1.0f : 0.72f);
            moveCommandButton.setSelected(mode == 1);
        }
        if (attackCommandButton != null) {
            attackCommandButton.setAlpha(mode == 2 ? 1.0f : 0.72f);
            attackCommandButton.setSelected(mode == 2);
        }
        if (rotateCommandButton != null) {
            rotateCommandButton.setAlpha(mode == 3 ? 1.0f : 0.72f);
            rotateCommandButton.setSelected(mode == 3);
        }
        if (spyglassCommandButton != null) {
            spyglassCommandButton.setAlpha(mode == 4 ? 1.0f : 0.72f);
            spyglassCommandButton.setSelected(mode == 4);
        }
        if (clearMinesCommandButton != null) {
            clearMinesCommandButton.setAlpha(
                    mode == 5 ? 1.0f : 0.72f);
            clearMinesCommandButton.setSelected(mode == 5);
        }
        if (placeMinesCommandButton != null) {
            placeMinesCommandButton.setAlpha(
                    mode == 6 ? 1.0f : 0.72f);
            placeMinesCommandButton.setSelected(mode == 6);
        }
        if (buildTrenchesCommandButton != null) {
            buildTrenchesCommandButton.setAlpha(
                    mode == 7 ? 1.0f : 0.72f);
            buildTrenchesCommandButton.setSelected(mode == 7);
        }
    }

    private void showCommandHint(String hint) {
        if (missionStatus != null) {
            missionStatus.setText(hint);
        }
    }

    private ImageButton originalImageButton(
            File dataRoot,
            String relativePath,
            View.OnClickListener listener) {
        ImageButton button = new ImageButton(this);
        button.setPadding(0, 0, 0, 0);
        button.setBackgroundColor(Color.TRANSPARENT);
        button.setScaleType(ImageButton.ScaleType.FIT_CENTER);
        android.graphics.Bitmap bitmap =
                TgaDecoder.decode(new File(dataRoot, relativePath));
        if (bitmap != null) {
            button.setImageDrawable(
                    new BitmapDrawable(getResources(), bitmap));
        }
        if (listener == null) {
            button.setClickable(false);
            button.setFocusable(false);
        } else {
            button.setOnClickListener(listener);
        }
        return button;
    }

    private ImageButton touchOnlyButton(
            String contentDescription,
            View.OnClickListener listener) {
        ImageButton button = new ImageButton(this);
        button.setPadding(0, 0, 0, 0);
        button.setBackgroundColor(Color.TRANSPARENT);
        button.setImageDrawable(null);
        button.setContentDescription(contentDescription);
        button.setOnClickListener(listener);
        return button;
    }

    private void showOutcome(String title, int color) {
        if (outcomePanel == null || outcomeTitle == null) {
            return;
        }
        outcomeTitle.setText(title);
        outcomeTitle.setTextColor(color);
        outcomePanel.setVisibility(View.VISIBLE);
    }

    private String missionLabel() {
        Intent intent = getIntent();
        String missionId = intent == null
                ? null
                : intent.getStringExtra(EXTRA_MISSION_ID);
        if (missionId == null || missionId.isEmpty()) {
            return "Loading mission...";
        }
        return missionLabel(missionId);
    }

    private String missionLabel(String missionId) {
        String label = missionId.replace('\\', '/');
        String[] segments = label.split("/");
        for (int index = 0; index + 1 < segments.length; ++index) {
            if (!"Campaigns".equalsIgnoreCase(segments[index])) {
                continue;
            }
            String campaign = campaignLabel(segments[index + 1]);
            String mission = segments.length >= 2
                    ? segments[segments.length - 2]
                    : "";
            if (!campaign.isEmpty() && !mission.isEmpty()) {
                return campaign + " campaign — " + mission;
            }
        }
        if (label.startsWith("Scenario/")) {
            label = label.substring("Scenario/".length());
        }
        if (label.endsWith("/MapInfo.xdb")) {
            label = label.substring(
                    0,
                    label.length() - "/MapInfo.xdb".length());
        }
        return label.replace('/', ' ');
    }

    private String campaignLabel(String code) {
        if ("USA".equalsIgnoreCase(code)) {
            return "USA";
        }
        if ("GER".equalsIgnoreCase(code)) {
            return "Germany";
        }
        if ("USSR".equalsIgnoreCase(code)) {
            return "USSR";
        }
        if ("Tutorial".equalsIgnoreCase(code)) {
            return "Tutorial";
        }
        return code;
    }

    private int dp(int value) {
        return Math.round(
                value * getResources().getDisplayMetrics().density);
    }

    private void persistMissionSelectionFromIntent(Intent intent) {
        if (intent == null || !intent.hasExtra(EXTRA_MISSION_ID)) {
            return;
        }
        String missionId = intent.getStringExtra(EXTRA_MISSION_ID);
        if (missionId == null || missionId.isEmpty()) {
            return;
        }
        int difficulty = intent.getIntExtra(EXTRA_DIFFICULTY, 0);
        for (File target : selectionTargets()) {
            File parent = target.getParentFile();
            if (parent != null) {
                parent.mkdirs();
            }
            try (FileWriter writer = new FileWriter(target, false)) {
                writer.write("mission_id=" + missionId + "\n");
                writer.write("difficulty=" + difficulty + "\n");
            } catch (IOException ignored) {
            }
        }
    }

    private File[] selectionTargets() {
        File external = getExternalFilesDir(null);
        if (external == null) {
            return new File[] {new File(getFilesDir(), "selected_mission.txt")};
        }
        return new File[] {
                new File(external, "selected_mission.txt"),
                new File(getFilesDir(), "selected_mission.txt"),
        };
    }
}
