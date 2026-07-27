package com.nival.blitzkrieg2;

import android.content.Intent;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
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
    private TextView outcomeTitle;
    private TextView missionStatus;
    private boolean outcomePolling;
    private final Runnable outcomePoll = new Runnable() {
        @Override
        public void run() {
            if (!outcomePolling) {
                return;
            }
            if (missionStatus != null) {
                missionStatus.setText(NativeBridge.getMissionHudStatus());
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

        LinearLayout info = new LinearLayout(this);
        info.setOrientation(LinearLayout.VERTICAL);
        info.setPadding(dp(14), dp(9), dp(14), dp(9));
        GradientDrawable infoBackground = new GradientDrawable();
        infoBackground.setColor(0xb80b1110);
        infoBackground.setCornerRadius(dp(8));
        info.setBackground(infoBackground);

        TextView mission = new TextView(this);
        mission.setTextColor(0xfff0e7ca);
        mission.setTextSize(16.0f);
        mission.setText(missionLabel());
        info.addView(
                mission,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.WRAP_CONTENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView controls = new TextView(this);
        controls.setTextColor(0xffc7d0c7);
        controls.setTextSize(12.0f);
        controls.setText(
                "Tap unit: select   Tap ground: move   Tap enemy: attack\n"
                        + "Drag: pan   Pinch: zoom   Two-finger twist: rotate");
        info.addView(
                controls,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.WRAP_CONTENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        missionStatus = new TextView(this);
        missionStatus.setTextColor(0xffd9c46b);
        missionStatus.setTextSize(12.0f);
        missionStatus.setText("Objectives: loading");
        info.addView(
                missionStatus,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.WRAP_CONTENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        FrameLayout.LayoutParams infoParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.START);
        infoParams.setMargins(dp(16), dp(14), dp(16), dp(14));
        root.addView(info, infoParams);

        LinearLayout missionActions = new LinearLayout(this);
        missionActions.setOrientation(LinearLayout.HORIZONTAL);

        Button forfeit = new Button(this);
        forfeit.setAllCaps(false);
        forfeit.setText("Surrender");
        forfeit.setTextColor(Color.WHITE);
        forfeit.setOnClickListener(view -> NativeBridge.forfeitMission());
        missionActions.addView(
                forfeit,
                new LinearLayout.LayoutParams(dp(112), dp(48)));

        Button missions = new Button(this);
        missions.setAllCaps(false);
        missions.setText("Missions");
        missions.setTextColor(Color.WHITE);
        missions.setOnClickListener(view -> finish());
        missionActions.addView(
                missions,
                new LinearLayout.LayoutParams(dp(112), dp(48)));

        FrameLayout.LayoutParams actionParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.END);
        actionParams.setMargins(dp(16), dp(12), dp(16), dp(12));
        root.addView(missionActions, actionParams);

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
            return "USA campaign — first mission";
        }
        String label = missionId.replace('\\', '/');
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
