package com.nival.blitzkrieg2;

import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public final class MissionSelectActivity extends AppCompatActivity {
    private static final String TAG = "Blitzkrieg2";
    private static final String[] EXCLUDED_MISSION_SEGMENTS = {
            "arttests",
            "crap",
            "designertest",
            "distest",
            "editor",
            "m1-test",
            "m1_test",
            "multi",
            "multiplayer",
            "progtests",
            "test",
            "test3",
            "testers",
    };

    private final ArrayList<MissionEntry> missions = new ArrayList<>();
    private ArrayAdapter<String> adapter;
    private TextView status;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(createContentView());
        loadMissions();
    }

    private LinearLayout createContentView() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(24, 18, 24, 18);
        root.setBackgroundColor(0xff101615);

        status = new TextView(this);
        status.setTextColor(0xffe8e1c8);
        status.setTextSize(14.0f);
        status.setText("Scanning single-player missions...");
        root.addView(status, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        Button defaultButton = new Button(this);
        defaultButton.setText("Start first campaign mission");
        defaultButton.setOnClickListener(view -> {
            deleteSelection();
            status.setText("Launching first campaign mission...");
            startActivity(new Intent(this, Blitzkrieg2Activity.class));
        });
        root.addView(defaultButton, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        ListView list = new ListView(this);
        list.setBackgroundColor(0xff101615);
        list.setDividerHeight(1);
        adapter = new ArrayAdapter<String>(
                this,
                android.R.layout.simple_list_item_1,
                new ArrayList<>()) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                View view = super.getView(position, convertView, parent);
                TextView text = view.findViewById(android.R.id.text1);
                if (text != null) {
                    text.setTextColor(0xffe8e1c8);
                    text.setTextSize(18.0f);
                    text.setPadding(28, 18, 28, 18);
                }
                view.setBackgroundColor(0xff101615);
                return view;
            }
        };
        list.setAdapter(adapter);
        list.setClickable(true);
        list.setItemsCanFocus(false);
        list.setOnItemClickListener((parent, view, position, id) -> {
            if (position >= 0 && position < missions.size()) {
                startMission(missions.get(position));
            }
        });
        root.addView(list, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1.0f));

        return root;
    }

    private void loadMissions() {
        new Thread(() -> {
            ArrayList<MissionEntry> found = new ArrayList<>();
            File dataRoot = selectReadableDataRoot();
            Log.i(TAG, "Mission selector scanning: " + dataRoot.getAbsolutePath());
            scanForMapInfos(dataRoot, dataRoot, found);
            Log.i(TAG, "Mission selector found " + found.size() + " maps.");
            Collections.sort(found);
            runOnUiThread(() -> {
                missions.clear();
                missions.addAll(found);
                ArrayList<String> labels = new ArrayList<>(found.size());
                for (MissionEntry mission : found) {
                    labels.add(mission.label);
                }
                adapter.clear();
                adapter.addAll(labels);
                adapter.notifyDataSetChanged();
                status.setText("Single-player missions: " + found.size());
            });
        }, "BK2MissionScan").start();
    }

    private File selectReadableDataRoot() {
        File internalRoot = new File(getFilesDir(), "DataAndroid/Data");
        if (new File(internalRoot, "types.xml").canRead() &&
                new File(internalRoot, "index.bin").canRead()) {
            return internalRoot;
        }
        File external = getExternalFilesDir(null);
        if (external != null) {
            return new File(external, "DataAndroid/Data");
        }
        return internalRoot;
    }

    private void scanForMapInfos(File dataRoot, File file, List<MissionEntry> out) {
        if (file == null || !file.exists()) {
            return;
        }
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children == null) {
                return;
            }
            for (File child : children) {
                scanForMapInfos(dataRoot, child, out);
            }
            return;
        }
        if (!"MapInfo.xdb".equals(file.getName())) {
            return;
        }
        String missionId = relativePath(dataRoot, file);
        if (hasExcludedSegment(missionId)) {
            return;
        }
        out.add(new MissionEntry(missionId, labelForMission(missionId)));
    }

    private String relativePath(File root, File file) {
        String rootPath = root.getAbsolutePath();
        String filePath = file.getAbsolutePath();
        if (filePath.startsWith(rootPath)) {
            filePath = filePath.substring(rootPath.length());
        }
        while (filePath.startsWith(File.separator)) {
            filePath = filePath.substring(1);
        }
        return filePath.replace(File.separatorChar, '/');
    }

    private boolean hasExcludedSegment(String missionId) {
        String[] segments = missionId.replace('\\', '/').split("/");
        for (String segment : segments) {
            String lower = segment.toLowerCase();
            for (String excluded : EXCLUDED_MISSION_SEGMENTS) {
                if (lower.equals(excluded)) {
                    return true;
                }
            }
        }
        return false;
    }

    private String labelForMission(String missionId) {
        String label = missionId;
        if (label.startsWith("Scenario/")) {
            label = label.substring("Scenario/".length());
        }
        if (label.endsWith("/MapInfo.xdb")) {
            label = label.substring(0, label.length() - "/MapInfo.xdb".length());
        }
        return label.replace('/', ' ');
    }

    private void startMission(MissionEntry mission) {
        if (!writeSelection(mission)) {
            status.setText("Cannot write mission selection: " + mission.label);
            return;
        }
        status.setText("Launching: " + mission.label);
        Intent intent = new Intent(this, Blitzkrieg2Activity.class);
        intent.putExtra(Blitzkrieg2Activity.EXTRA_MISSION_ID, mission.missionId);
        intent.putExtra(Blitzkrieg2Activity.EXTRA_DIFFICULTY, 0);
        startActivity(intent);
    }

    private boolean writeSelection(MissionEntry mission) {
        boolean wrote = false;
        File[] targets = selectionTargets();
        for (File target : targets) {
            File parent = target.getParentFile();
            if (parent != null) {
                parent.mkdirs();
            }
            try (FileWriter writer = new FileWriter(target, false)) {
                writer.write("mission_id=" + mission.missionId + "\n");
                writer.write("difficulty=0\n");
                wrote = true;
            } catch (IOException ignored) {
            }
        }
        return wrote;
    }

    private void deleteSelection() {
        for (File target : selectionTargets()) {
            if (target.exists()) {
                target.delete();
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

    private static final class MissionEntry implements Comparable<MissionEntry> {
        final String missionId;
        final String label;

        MissionEntry(String missionId, String label) {
            this.missionId = missionId;
            this.label = label;
        }

        @Override
        public int compareTo(MissionEntry other) {
            return label.compareToIgnoreCase(other.label);
        }
    }
}
