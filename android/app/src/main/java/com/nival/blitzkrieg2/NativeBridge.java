package com.nival.blitzkrieg2;

import android.content.Intent;

import java.lang.ref.WeakReference;

final class NativeBridge {
    private static WeakReference<Blitzkrieg2Activity> activityRef = new WeakReference<>(null);

    private NativeBridge() {
    }

    static native void configurePaths(String filesDir, String noBackupDir, String externalFilesDir);
    static native String runStartupProbe();
    static native String runAudioBackendProbe();
    static native String runAudioDeviceProbe();
    static native String runMusicStreamingProbe();
    static native String runLegacyMusicProbe();
    static native String runSinglePlayerCatalogProbe();
    static native String runOriginalMenuProbe();
    static native String startFirstCampaignMissionProbe();
    static native String runMissionProgressionProbe();
    static native String runMissionCheckpointProbe();
    static native String getMissionOutcome();
    static native String getCurrentMissionId();
    static native String getMissionHudHeadline();
    static native String getMissionHudStatus();
    static native String getSelectedUnitHudStatus();
    static native String getSelectedUnitHudSnapshot();
    static native boolean setTouchCommandMode(int mode);
    static native int getTouchCommandMode();
    static native boolean setActiveSelectedUnit(int unitId);
    static native boolean stopSelectedUnit();
    static native boolean performSelectedUnitAction(int userAction);
    static native void setMissionHudHeightPixels(int height);
    static native void setMissionPaused(boolean paused);
    static native boolean isMissionPaused();
    static native int[] getMissionMinimapArgb(int width, int height);
    static native boolean centerMissionCameraFromMinimap(
            float normalizedX,
            float normalizedY);
    static native void forfeitMission();

    static void attachActivity(Blitzkrieg2Activity activity) {
        activityRef = new WeakReference<>(activity);
    }

    static void playFullscreenVideo(String videoPath) {
        playFullscreenVideos(videoPath == null ? null : new String[] {videoPath});
    }

    static void playFullscreenVideos(String[] videoPaths) {
        Blitzkrieg2Activity activity = activityRef.get();
        if (activity == null || videoPaths == null || videoPaths.length == 0) {
            return;
        }

        activity.runOnUiThread(() -> {
            Intent intent = new Intent(activity, VideoPlayerActivity.class);
            intent.putExtra(VideoPlayerActivity.EXTRA_VIDEO_PATHS, videoPaths);
            activity.startActivity(intent);
        });
    }
}
