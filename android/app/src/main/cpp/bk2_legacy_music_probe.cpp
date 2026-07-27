#include <chrono>
#include <sstream>
#include <string>
#include <thread>

#include <jni.h>

#include "Sound/stdafx.h"

#include "Sound/DBMusicSystem.h"
#include "Sound/MusicSystem.hpp"
#include "System/System.h"
#include "libdb/Database.h"

#include "bk2_android_audio_backend.h"
#include "bk2_android_platform.h"
#include "bk2_android_vorbis_stream.h"
#include "bk2_legacy_music_probe.h"

namespace bk2::android {
namespace {

void MarkLoaded(NDb::CResource* resource) {
    NDb::CResourceHelper::SetLoaded(resource);
}

}  // namespace

std::string RunLegacyMusicProbe() {
    if (NSingleton::Singleton(IMusicSystem::tidTypeID) != nullptr) {
        return "legacy_music=failed; legacy_music_error=singleton_already_registered";
    }
    if (!AudioBackend().is_initialized()) {
        AudioBackend().init(44100, 96);
    }

    AndroidAudioMetadata metadata;
    std::string metadata_error;
    const bool metadata_ok = ReadAndroidAudioMetadata(
            "/Music/Intro_Main.ogg", &metadata, &metadata_error);

    CPtr<NDb::SMusicTrack> track = new NDb::SMusicTrack();
    track->szMusicFileName = "/Music/Intro_Main.ogg";
    MarkLoaded(track);

    CPtr<NDb::SPlayTime> play_time = new NDb::SPlayTime();
    play_time->nPlayTime = 2000;
    MarkLoaded(play_time);

    CPtr<NDb::SFade> fade_in = new NDb::SFade();
    fade_in->fFinalVolume = 80.0f;
    fade_in->nFadeTime = 40;
    MarkLoaded(fade_in);

    CPtr<NDb::SComposition> composition = new NDb::SComposition();
    composition->pTrack = track;
    composition->pPlayTime = play_time;
    composition->pFadeIn = fade_in;
    MarkLoaded(composition);

    CPtr<NDb::SPlayList> playlist = new NDb::SPlayList();
    playlist->stillOrder.push_back(
            CDBPtr<NDb::SComposition>(composition.GetPtr()));
    MarkLoaded(playlist);

    CPtr<NDb::SMapMusic> map_music = new NDb::SMapMusic();
    map_music->playLists.push_back(
            CDBPtr<NDb::SPlayList>(playlist.GetPtr()));
    MarkLoaded(map_music);

    IMusicSystem* music = CreateMusicSystem();
    NSingleton::RegisterSingleton(music, IMusicSystem::tidTypeID);
    music->Init(map_music, 0);
    music->Update();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    music->Update();

    NMusicSystem::CMusicSystem* concrete = NMusicSystem::GetMusicSystem();
    const int channel = concrete->GetChannel(EST_MUSIC);
    const auto playing_state = AudioBackend().channel_state(channel);
    const uint64_t consumed_before_pause =
            playing_state.has_value() && playing_state->stream != nullptr
            ? playing_state->stream->consumed_frames()
            : 0;

    music->PauseMusic(EMS_MASTER, true);
    music->Update();
    const bool channel_paused = AudioBackend().is_channel_paused(channel);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto paused_state = AudioBackend().channel_state(channel);
    const uint64_t consumed_while_paused =
            paused_state.has_value() && paused_state->stream != nullptr
            ? paused_state->stream->consumed_frames()
            : 0;

    music->PauseMusic(EMS_MASTER, false);
    music->Update();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto resumed_state = AudioBackend().channel_state(channel);
    const uint64_t consumed_after_resume =
            resumed_state.has_value() && resumed_state->stream != nullptr
            ? resumed_state->stream->consumed_frames()
            : 0;
    const bool pause_held =
            consumed_while_paused == consumed_before_pause;
    const bool resume_advanced =
            consumed_after_resume > consumed_while_paused;
    const bool playing = AudioBackend().is_playing(channel);

    music->Clear();
    NSingleton::UnRegisterSingleton(IMusicSystem::tidTypeID);
    const bool stopped = !AudioBackend().is_playing(channel);

    std::ostringstream report;
    report << "legacy_music="
           << (metadata_ok && channel > 0 && playing && channel_paused &&
               pause_held && resume_advanced && stopped
                   ? "probed"
                   : "failed")
           << "; legacy_music_metadata=" << (metadata_ok ? "true" : "false")
           << "; legacy_music_duration_ms=" << metadata.duration_ms
           << "; legacy_music_channel=" << channel
           << "; legacy_music_playing=" << (playing ? "true" : "false")
           << "; legacy_music_channel_paused="
           << (channel_paused ? "true" : "false")
           << "; legacy_music_pause_held=" << (pause_held ? "true" : "false")
           << "; legacy_music_resume_advanced="
           << (resume_advanced ? "true" : "false")
           << "; legacy_music_stopped=" << (stopped ? "true" : "false")
           << "; legacy_music_consumed_before_pause=" << consumed_before_pause
           << "; legacy_music_consumed_while_paused=" << consumed_while_paused
           << "; legacy_music_consumed_after_resume=" << consumed_after_resume;
    if (!metadata_error.empty()) {
        report << "; legacy_music_error=" << metadata_error;
    }
    const std::string text = report.str();
    PlatformRuntime::instance().log_info(text);
    return text;
}

}  // namespace bk2::android

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_runLegacyMusicProbe(JNIEnv* env, jclass) {
    const std::string report = bk2::android::RunLegacyMusicProbe();
    return env->NewStringUTF(report.c_str());
}
