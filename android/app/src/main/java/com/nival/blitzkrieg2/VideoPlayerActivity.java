package com.nival.blitzkrieg2;

import android.app.Activity;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.VideoView;

import java.io.File;

public final class VideoPlayerActivity extends Activity {
    static final String EXTRA_VIDEO_PATH = "com.nival.blitzkrieg2.VIDEO_PATH";
    static final String EXTRA_VIDEO_PATHS = "com.nival.blitzkrieg2.VIDEO_PATHS";

    private VideoView videoView;
    private String[] videoPaths;
    private int videoIndex;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        videoView = new VideoView(this);
        videoView.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        setContentView(videoView);

        videoPaths = getIntent().getStringArrayExtra(EXTRA_VIDEO_PATHS);
        if (videoPaths == null || videoPaths.length == 0) {
            String videoPath = getIntent().getStringExtra(EXTRA_VIDEO_PATH);
            videoPaths = videoPath == null ? new String[0] : new String[] {videoPath};
        }
        if (!playNextVideo()) {
            finish();
        }
    }

    private boolean playNextVideo() {
        while (videoIndex < videoPaths.length) {
            String videoPath = videoPaths[videoIndex++];
            if (videoPath == null || videoPath.isEmpty()) {
                continue;
            }
            playVideo(videoPath);
            return true;
        }
        return false;
    }

    private void playVideo(String videoPath) {
        videoView.setVideoURI(Uri.fromFile(new File(videoPath)));
        videoView.setOnCompletionListener(player -> {
            if (!playNextVideo()) {
                finish();
            }
        });
        videoView.setOnErrorListener((player, what, extra) -> {
            if (!playNextVideo()) {
                finish();
            }
            return true;
        });
        videoView.start();
    }
}
