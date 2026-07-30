package com.nival.blitzkrieg2;

import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.util.Xml;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.Spinner;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

import org.xmlpull.v1.XmlPullParser;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
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
    private LinearLayout campaignButtons;
    private Button continueButton;
    private ListView missionList;
    private Spinner difficultySpinner;
    private TextView status;

    /** Opens the debug mission browser instead of the original menu. */
    public static final String EXTRA_DEBUG_BROWSER =
            "com.nival.blitzkrieg2.DEBUG_BROWSER";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (!getIntent().getBooleanExtra(EXTRA_DEBUG_BROWSER, false)) {
            // The game boots into its own interface; this Android layout is
            // only the developer mission browser now.
            Intent menu = new Intent(this, Blitzkrieg2Activity.class);
            menu.putExtra(Blitzkrieg2Activity.EXTRA_SHOW_MENU, true);
            startActivity(menu);
            finish();
            return;
        }
        setContentView(createContentView());
        loadMissions();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshContinueButton();
    }

    private LinearLayout createContentView() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(24, 18, 24, 18);
        root.setBackgroundColor(0xff101615);

        TextView title = new TextView(this);
        title.setTextColor(0xffe8e1c8);
        title.setTextSize(22.0f);
        title.setText("Blitzkrieg 2 — Mobile Port");
        root.addView(title, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout difficultyRow = new LinearLayout(this);
        difficultyRow.setOrientation(LinearLayout.HORIZONTAL);
        TextView difficultyLabel = new TextView(this);
        difficultyLabel.setTextColor(0xffd4c580);
        difficultyLabel.setTextSize(15.0f);
        difficultyLabel.setText("Difficulty");
        difficultyRow.addView(
                difficultyLabel,
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));
        difficultySpinner = new Spinner(this);
        ArrayAdapter<String> difficultyAdapter = new ArrayAdapter<>(
                this,
                android.R.layout.simple_spinner_item,
                new String[] {"Easy", "Normal", "Hard", "Very hard"}) {
            @Override
            public View getView(
                    int position,
                    View convertView,
                    ViewGroup parent) {
                return styleDifficultyView(
                        super.getView(position, convertView, parent));
            }

            @Override
            public View getDropDownView(
                    int position,
                    View convertView,
                    ViewGroup parent) {
                return styleDifficultyView(
                        super.getDropDownView(position, convertView, parent));
            }
        };
        difficultyAdapter.setDropDownViewResource(
                android.R.layout.simple_spinner_dropdown_item);
        difficultySpinner.setAdapter(difficultyAdapter);
        difficultyRow.addView(
                difficultySpinner,
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));
        root.addView(
                difficultyRow,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        continueButton = new Button(this);
        continueButton.setText("Continue campaign");
        continueButton.setVisibility(View.GONE);
        continueButton.setOnClickListener(view -> {
            if (!writeContinueSelection()) {
                status.setText("Cannot prepare campaign autosave.");
                return;
            }
            status.setText("Loading campaign autosave...");
            startActivity(new Intent(this, Blitzkrieg2Activity.class));
        });
        root.addView(continueButton, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView campaignTitle = new TextView(this);
        campaignTitle.setTextColor(0xffd4c580);
        campaignTitle.setTextSize(16.0f);
        campaignTitle.setText("Start a new campaign");
        root.addView(campaignTitle, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        campaignButtons = new LinearLayout(this);
        campaignButtons.setOrientation(LinearLayout.HORIZONTAL);
        root.addView(
                campaignButtons,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        Button browseButton = new Button(this);
        browseButton.setAllCaps(false);
        browseButton.setText("Individual mission browser (debug)");
        browseButton.setOnClickListener(view -> {
            boolean show = missionList.getVisibility() != View.VISIBLE;
            missionList.setVisibility(show ? View.VISIBLE : View.GONE);
            browseButton.setText(
                    show
                            ? "Hide individual mission browser"
                            : "Individual mission browser (debug)");
        });
        root.addView(browseButton, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        status = new TextView(this);
        status.setTextColor(0xffe8e1c8);
        status.setTextSize(14.0f);
        status.setText("Loading original campaign catalog...");
        root.addView(status, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        missionList = new ListView(this);
        missionList.setBackgroundColor(0xff101615);
        missionList.setDividerHeight(1);
        missionList.setVisibility(View.GONE);
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
        missionList.setAdapter(adapter);
        missionList.setClickable(true);
        missionList.setItemsCanFocus(false);
        missionList.setOnItemClickListener((parent, view, position, id) -> {
            if (position >= 0 && position < missions.size()) {
                startMission(missions.get(position));
            }
        });
        root.addView(missionList, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1.0f));

        return root;
    }

    private View styleDifficultyView(View view) {
        view.setBackgroundColor(0xff101615);
        TextView text = view.findViewById(android.R.id.text1);
        if (text != null) {
            text.setTextColor(0xffe8e1c8);
            text.setTextSize(16.0f);
        }
        return view;
    }

    private void refreshContinueButton() {
        if (continueButton != null) {
            continueButton.setVisibility(
                    campaignAutosave().canRead() ? View.VISIBLE : View.GONE);
        }
    }

    private File campaignAutosave() {
        return new File(
                getFilesDir(),
                "Profiles/default/Saves/android_autosave.bk2checkpoint");
    }

    private boolean writeContinueSelection() {
        boolean wrote = false;
        for (File target : selectionTargets()) {
            File parent = target.getParentFile();
            if (parent != null) {
                parent.mkdirs();
            }
            try (FileWriter writer = new FileWriter(target, false)) {
                writer.write("continue=1\n");
                writer.write("difficulty=" + selectedDifficulty() + "\n");
                wrote = true;
            } catch (IOException ignored) {
            }
        }
        return wrote;
    }

    private void loadMissions() {
        new Thread(() -> {
            ArrayList<MissionEntry> found = new ArrayList<>();
            ArrayList<CampaignEntry> foundCampaigns = new ArrayList<>();
            int[] missingMapData = {0};
            File dataRoot = selectReadableDataRoot();
            Log.i(TAG, "Mission selector scanning: " + dataRoot.getAbsolutePath());
            loadCampaignCatalog(dataRoot, foundCampaigns);
            scanForMapInfos(dataRoot, dataRoot, found, missingMapData);
            Log.i(
                    TAG,
                    "Mission selector found " + foundCampaigns.size()
                            + " campaigns and " + found.size()
                            + " maps with local binary data; skipped "
                            + missingMapData[0]
                            + " descriptors without map data.");
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
                populateCampaignButtons(foundCampaigns);
                if (foundCampaigns.isEmpty()) {
                    status.setText(
                            "Campaign catalog is unavailable. "
                                    + "The debug mission browser found "
                                    + found.size()
                                    + " maps.");
                } else {
                    status.setText(
                            "Choose a campaign. "
                                    + foundCampaigns.size()
                                    + " original campaigns available; "
                                    + found.size()
                                    + " individual maps in the debug browser.");
                }
            });
        }, "BK2MissionScan").start();
    }

    private int selectedDifficulty() {
        return difficultySpinner == null
                ? 0
                : difficultySpinner.getSelectedItemPosition();
    }

    private void populateCampaignButtons(List<CampaignEntry> campaigns) {
        campaignButtons.removeAllViews();
        for (CampaignEntry campaign : campaigns) {
            Button button = new Button(this);
            button.setAllCaps(false);
            button.setText(campaign.label);
            button.setOnClickListener(view -> startCampaign(campaign));
            campaignButtons.addView(
                    button,
                    new LinearLayout.LayoutParams(
                            0,
                            ViewGroup.LayoutParams.WRAP_CONTENT,
                            1.0f));
        }
    }

    private void startCampaign(CampaignEntry campaign) {
        if (!writeCampaignSelection(campaign.index)) {
            status.setText("Cannot write campaign selection: " + campaign.label);
            return;
        }
        status.setText("Launching " + campaign.label + "...");
        startActivity(new Intent(this, Blitzkrieg2Activity.class));
    }

    private boolean writeCampaignSelection(int campaignIndex) {
        boolean wrote = false;
        for (File target : selectionTargets()) {
            File parent = target.getParentFile();
            if (parent != null) {
                parent.mkdirs();
            }
            try (FileWriter writer = new FileWriter(target, false)) {
                writer.write("campaign=" + campaignIndex + "\n");
                writer.write("difficulty=" + selectedDifficulty() + "\n");
                wrote = true;
            } catch (IOException ignored) {
            }
        }
        return wrote;
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

    private void loadCampaignCatalog(
            File dataRoot,
            List<CampaignEntry> out) {
        File gameRoot = new File(dataRoot, "GameRoot.xdb");
        if (!gameRoot.canRead()) {
            Log.e(TAG, "Campaign catalog missing: " + gameRoot.getAbsolutePath());
            return;
        }
        try (FileInputStream input = new FileInputStream(gameRoot)) {
            XmlPullParser parser = Xml.newPullParser();
            parser.setInput(input, null);
            boolean inCampaigns = false;
            int campaignIndex = 0;
            int event = parser.getEventType();
            while (event != XmlPullParser.END_DOCUMENT) {
                if (event == XmlPullParser.START_TAG) {
                    String name = parser.getName();
                    if ("Campaigns".equals(name)) {
                        inCampaigns = true;
                    } else if (inCampaigns && "Item".equals(name)) {
                        String href = parser.getAttributeValue(null, "href");
                        CampaignEntry campaign =
                                campaignEntryFromHref(
                                        dataRoot,
                                        campaignIndex++,
                                        href);
                        if (campaign != null) {
                            out.add(campaign);
                        }
                    }
                } else if (event == XmlPullParser.END_TAG &&
                        "Campaigns".equals(parser.getName())) {
                    inCampaigns = false;
                }
                event = parser.next();
            }
        } catch (Exception error) {
            Log.e(TAG, "Cannot parse GameRoot campaign catalog", error);
        }
    }

    private CampaignEntry campaignEntryFromHref(
            File dataRoot,
            int index,
            String href) {
        String relativePath = referencePath(href);
        if (relativePath.isEmpty()) {
            return null;
        }
        File campaignFile = new File(dataRoot, relativePath);
        if (!campaignFile.canRead()) {
            Log.w(TAG, "Campaign descriptor missing: " + campaignFile);
            return null;
        }
        String code = campaignFile.getParentFile() == null
                ? "Campaign " + (index + 1)
                : campaignFile.getParentFile().getName();
        String label = readLocalizedCampaignName(dataRoot, campaignFile);
        if (label.isEmpty()) {
            label = fallbackCampaignLabel(code);
        }
        return new CampaignEntry(index, label);
    }

    private String readLocalizedCampaignName(
            File dataRoot,
            File campaignFile) {
        try (FileInputStream input = new FileInputStream(campaignFile)) {
            XmlPullParser parser = Xml.newPullParser();
            parser.setInput(input, null);
            int event = parser.getEventType();
            while (event != XmlPullParser.END_DOCUMENT) {
                if (event == XmlPullParser.START_TAG &&
                        "LocalizedNameFileRef".equals(parser.getName())) {
                    String rawHref =
                            parser.getAttributeValue(null, "href");
                    boolean absolute = rawHref != null &&
                            (rawHref.startsWith("/") ||
                                    rawHref.startsWith("\\"));
                    String href = referencePath(rawHref);
                    if (href.isEmpty()) {
                        return "";
                    }
                    File textFile = absolute
                            ? new File(dataRoot, href)
                            : new File(campaignFile.getParentFile(), href);
                    return readLegacyText(textFile);
                }
                event = parser.next();
            }
        } catch (Exception error) {
            Log.w(TAG, "Cannot read campaign name from " + campaignFile, error);
        }
        return "";
    }

    private String referencePath(String href) {
        if (href == null) {
            return "";
        }
        int xpointer = href.indexOf('#');
        String path = xpointer < 0 ? href : href.substring(0, xpointer);
        path = path.replace('\\', '/');
        while (path.startsWith("/")) {
            path = path.substring(1);
        }
        return path;
    }

    private String readLegacyText(File file) throws IOException {
        if (!file.canRead()) {
            return "";
        }
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[4096];
            int count;
            while ((count = input.read(buffer)) >= 0) {
                bytes.write(buffer, 0, count);
            }
        }
        byte[] data = bytes.toByteArray();
        String value;
        if (data.length >= 2 &&
                (data[0] & 0xff) == 0xff &&
                (data[1] & 0xff) == 0xfe) {
            value = new String(
                    data,
                    2,
                    data.length - 2,
                    StandardCharsets.UTF_16LE);
        } else if (data.length >= 2 &&
                (data[0] & 0xff) == 0xfe &&
                (data[1] & 0xff) == 0xff) {
            value = new String(
                    data,
                    2,
                    data.length - 2,
                    StandardCharsets.UTF_16BE);
        } else {
            int offset = data.length >= 3 &&
                    (data[0] & 0xff) == 0xef &&
                    (data[1] & 0xff) == 0xbb &&
                    (data[2] & 0xff) == 0xbf
                    ? 3
                    : 0;
            value = new String(
                    data,
                    offset,
                    data.length - offset,
                    StandardCharsets.UTF_8);
        }
        return value.replace("\u0000", "").trim();
    }

    private String fallbackCampaignLabel(String code) {
        if ("USA".equalsIgnoreCase(code)) {
            return "U.S.A.";
        }
        if ("GER".equalsIgnoreCase(code)) {
            return "Germany";
        }
        if ("USSR".equalsIgnoreCase(code)) {
            return "U.S.S.R.";
        }
        return code;
    }

    private void scanForMapInfos(
            File dataRoot,
            File file,
            List<MissionEntry> out,
            int[] missingMapData) {
        if (file == null || !file.exists()) {
            return;
        }
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children == null) {
                return;
            }
            for (File child : children) {
                scanForMapInfos(dataRoot, child, out, missingMapData);
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
        File mapBinary = new File(file.getParentFile(), "map.b2m");
        if (!mapBinary.canRead()) {
            ++missingMapData[0];
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
        intent.putExtra(
                Blitzkrieg2Activity.EXTRA_DIFFICULTY,
                selectedDifficulty());
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
                writer.write("difficulty=" + selectedDifficulty() + "\n");
                wrote = true;
            } catch (IOException ignored) {
            }
        }
        return wrote;
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

    private static final class CampaignEntry {
        final int index;
        final String label;

        CampaignEntry(int index, String label) {
            this.index = index;
            this.label = label;
        }
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
