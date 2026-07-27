package com.nival.blitzkrieg2;

import android.content.Intent;
import android.os.Bundle;
import com.google.androidgamesdk.GameActivity;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;

public final class Blitzkrieg2Activity extends GameActivity {
    public static final String EXTRA_MISSION_ID = "com.nival.blitzkrieg2.MISSION_ID";
    public static final String EXTRA_DIFFICULTY = "com.nival.blitzkrieg2.DIFFICULTY";

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
