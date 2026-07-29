package com.nival.blitzkrieg2;

import android.content.Intent;
import android.graphics.Color;
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
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.google.androidgamesdk.GameActivity;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;

public final class Blitzkrieg2Activity extends GameActivity {
    public static final String EXTRA_MISSION_ID = "com.nival.blitzkrieg2.MISSION_ID";
    public static final String EXTRA_DIFFICULTY = "com.nival.blitzkrieg2.DIFFICULTY";
    private final Handler outcomeHandler = new Handler(Looper.getMainLooper());
    private LinearLayout outcomePanel;
    private LinearLayout missionMenu;
    private TextView outcomeTitle;
    private TextView missionTitle;
    private TextView missionStatus;
    private OriginalMissionHudView originalHud;
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
                missionStatus.setText(NativeBridge.getMissionHudStatus());
            }
            if (originalHud != null && (hudPollCount++ & 3) == 0) {
                int[] pixels = NativeBridge.getMissionMinimapArgb(192, 192);
                if (pixels != null) {
                    originalHud.setMinimapPixels(pixels, 192, 192);
                }
            }
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
        missionTitle.setTextColor(0xffe5d9ad);
        missionTitle.setTextSize(12.0f);
        missionTitle.setText(missionLabel());
        missionTitle.setSingleLine(true);
        missionInfo.addView(
                missionTitle,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        missionStatus = new TextView(this);
        missionStatus.setTextColor(0xffd4c580);
        missionStatus.setTextSize(10.0f);
        missionStatus.setText("Objectives: loading");
        missionStatus.setMaxLines(3);
        missionInfo.addView(
                missionStatus,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        FrameLayout.LayoutParams missionInfoParams =
                new FrameLayout.LayoutParams(
                        dp(360),
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        Gravity.CENTER);
        missionInfoParams.setMargins(0, dp(10), 0, dp(10));
        hud.addView(missionInfo, missionInfoParams);

        LinearLayout commandButtons = new LinearLayout(this);
        commandButtons.setOrientation(LinearLayout.HORIZONTAL);
        commandButtons.setGravity(Gravity.CENTER);
        commandButtons.addView(
                originalImageButton(
                        dataRoot,
                        "Complete/UI/Buttons/Move/MoveNormal.tga",
                        null),
                new LinearLayout.LayoutParams(dp(38), dp(38)));
        commandButtons.addView(
                originalImageButton(
                        dataRoot,
                        "Complete/UI/Buttons/Attack/AttackNormal.tga",
                        null),
                new LinearLayout.LayoutParams(dp(38), dp(38)));
        commandButtons.addView(
                originalImageButton(
                        dataRoot,
                        "Complete/UI/Buttons/Stop/StopNormal.tga",
                        null),
                new LinearLayout.LayoutParams(dp(38), dp(38)));

        ImageButton objectives = originalImageButton(
                dataRoot,
                "Complete/UI/Buttons/Objectives/ObjectivesNormal.tga",
                view -> missionStatus.setVisibility(
                        missionStatus.getVisibility() == View.VISIBLE
                                ? View.INVISIBLE
                                : View.VISIBLE));
        commandButtons.addView(
                objectives,
                new LinearLayout.LayoutParams(dp(82), dp(34)));

        ImageButton menu = originalImageButton(
                dataRoot,
                "Complete/UI/Buttons/F10Menu/fake_F10MenuNormal.tga",
                view -> missionMenu.setVisibility(
                        missionMenu.getVisibility() == View.VISIBLE
                                ? View.GONE
                                : View.VISIBLE));
        commandButtons.addView(
                menu,
                new LinearLayout.LayoutParams(dp(82), dp(34)));

        FrameLayout.LayoutParams commandParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER_VERTICAL | Gravity.END);
        commandParams.setMargins(0, 0, dp(8), 0);
        hud.addView(commandButtons, commandParams);

        FrameLayout.LayoutParams hudParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                hudHeight,
                Gravity.BOTTOM);
        root.addView(hud, hudParams);

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
        missions.setOnClickListener(view -> finish());
        missionMenu.addView(
                missions,
                new LinearLayout.LayoutParams(dp(150), dp(44)));

        FrameLayout.LayoutParams menuParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER);
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
