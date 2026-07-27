#include "StdAfx.h"
#include "DBMusicSystem.h"
#include "MusicSystem.hpp"
#include "Track.h"
#if defined(BK2_ANDROID)
#include "bk2_android_audio_backend.h"
#include "bk2_android_platform.h"
#include "bk2_android_vorbis_stream.h"
#else
#include "../System/VFSOperations.h"
#include "../vendor/fmod/api/inc/fmod.h"
#endif

namespace NMusicSystem
{
#if !defined(BK2_ANDROID)
signed char F_CALLBACKAPI TrackFinishedCallBack( FSOUND_STREAM *stream, void *buff, int len, void *userdata )
{
	CTrack *pSFX = reinterpret_cast<CTrack*>( userdata );
	pSFX->NotifyTrackFinished();
	return true;
}
#endif
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// CTrack
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CTrack::NotifyTrackFinished()
{
#if defined(BK2_ANDROID)
	if ( eState == ETS_PLAYING && timePlayed < playTime.GetPlayTime() )
		PlayTrack( trackDuration > 0 ? timePlayed % trackDuration : 0 );
#else
	PlayTrack( 0 );
#endif
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CTrack::PlayTrack( int nTrackTime )
{
#if defined(BK2_ANDROID)
	if ( nChannel > 0 )
		bk2::android::AudioBackend().stop( nChannel );
	nChannel = 0;
	delete pStreamingSound;
	pStreamingSound = 0;

	if ( szAndroidTrackPath.empty() )
		return;

	std::string error;
	std::unique_ptr<bk2::android::AndroidVorbisStream> stream =
		bk2::android::AndroidVorbisStream::Open(
			szAndroidTrackPath,
			static_cast<uint64_t>( Max( nTrackTime, 0 ) ),
			2,
			&error );
	if ( !stream )
	{
		bk2::android::PlatformRuntime::instance().log_warn(
			std::string( "Cannot open legacy music track: " ) + error );
		return;
	}
	if ( stream->sample_rate() != bk2::android::AudioBackend().mix_rate() )
	{
		bk2::android::PlatformRuntime::instance().log_warn(
			"Legacy music sample rate does not match the Android mixer." );
		return;
	}

	nChannel = bk2::android::AudioBackend().play_stream( stream->pcm_source() );
	if ( nChannel <= 0 )
		return;

	pStreamingSound = stream.release();
	GetMusicSystem()->SetChannel( nChannel, eType );
#else
	const int nChannel = FSOUND_Stream_PlayEx( FSOUND_FREE, pStreamingSound, 0, true );
	GetMusicSystem()->SetChannel( nChannel, eType );
	FSOUND_Stream_SetEndCallback( pStreamingSound, TrackFinishedCallBack, this );
	FSOUND_Stream_SetTime( pStreamingSound, nTrackTime );
	FSOUND_SetPaused( nChannel, false );
#endif
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CTrack::OpenTrack()
{
#if defined(BK2_ANDROID)
	szAndroidTrackPath.clear();
	trackDuration = 0;
	if ( pTrack == 0 || pTrack->szMusicFileName.empty() )
		return;

	szAndroidTrackPath =
		bk2::android::ResolveAndroidAudioPath(
			std::string( pTrack->szMusicFileName.c_str() ) );
	bk2::android::AndroidAudioMetadata metadata;
	std::string error;
	if ( !bk2::android::ReadAndroidAudioMetadata(
			szAndroidTrackPath, &metadata, &error ) )
	{
		bk2::android::PlatformRuntime::instance().log_warn(
			std::string( "Cannot read legacy music metadata: " ) + error );
		szAndroidTrackPath.clear();
		return;
	}
	trackDuration = static_cast<NTimer::STime>( metadata.duration_ms );
#else
	if ( pTrackStream )
		delete pTrackStream;

	pTrackStream = new CFileStream( NVFS::GetMainVFS(), pTrack->szMusicFileName );
	pStreamingSound = NMusicSystem::OpenTrack( pTrackStream );
#endif
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CTrack::Segment()
{
	switch( eState )
	{
	case ETS_NOT_STARTED:
		OpenTrack();
#if defined(BK2_ANDROID)
		if ( trackDuration > 0 )
			PlayTrack( 0 );
		timeLastCall = GetAbsTime();
		eState = nChannel > 0 ? ETS_PLAYING : ETS_FINISHED;
#else
		if ( pStreamingSound )
			PlayTrack( 0 );
		timeLastCall = GetAbsTime();
		eState = pStreamingSound ? ETS_PLAYING : ETS_FINISHED;
#endif

		break;
	case ETS_PLAYING:
		{
			const NTimer::STime curTime = GetAbsTime();
			timePlayed += curTime - timeLastCall;
			timeLastCall = curTime;
		}
		if ( timePlayed >= playTime.GetPlayTime() )
		{
			eState = ETS_FINISHED;
#if defined(BK2_ANDROID)
			Stop();
#else
			FSOUND_Stream_SetEndCallback( pStreamingSound, 0, this );
			FSOUND_Stream_Stop( pStreamingSound );
			GetMusicSystem()->SetChannel( 0, eType );
#endif
		}
		else
		{
			// init after load
#if defined(BK2_ANDROID)
			if ( nChannel <= 0 ||
				!bk2::android::AudioBackend().is_playing( nChannel ) )
			{
				GetMusicSystem()->SetChannel( 0, eType );
				PlayTrack( trackDuration > 0 ? timePlayed % trackDuration : 0 );
				if ( nChannel <= 0 )
					eState = ETS_FINISHED;
			}
#else
			if ( !GetMusicSystem()->GetChannel( eType ) )
			{
				int nTime = FSOUND_Stream_GetLengthMs( pStreamingSound );
				int nTrackTime = timePlayed % nTime;
				PlayTrack( nTrackTime );
			}
#endif
		}

		break;
	case ETS_FINISHED:
		break;
	}
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CTrack::Play() 
{
	timeLastCall = GetAbsTime();
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CTrack::OnResetTimer() 
{
	timeLastCall = GetAbsTime();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool CTrack::IsFinished() const
{
	return eState == ETS_FINISHED || timePlayed >= playTime.GetPlayTime();
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CTrack::Stop()
{
#if defined(BK2_ANDROID)
	const int stoppedChannel = nChannel;
	if ( stoppedChannel > 0 )
		bk2::android::AudioBackend().stop( stoppedChannel );
	nChannel = 0;
	delete pStreamingSound;
	pStreamingSound = 0;

	CMusicSystem *pMusicSystem = checked_cast<CMusicSystem*>(
		NSingleton::Singleton( IMusicSystem::tidTypeID ) );
	if ( pMusicSystem && pMusicSystem->GetChannel( eType ) == stoppedChannel )
		pMusicSystem->SetChannel( 0, eType );
#else
	FSOUND_Stream_SetEndCallback( pStreamingSound, 0, this );
	FSOUND_Stream_Stop( pStreamingSound );
	GetMusicSystem()->SetChannel( 0, eType );
#endif
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
CTrack::~CTrack()
{
#if defined(BK2_ANDROID)
	Stop();
#else
	if ( pStreamingSound )
	{
		FSOUND_Stream_SetEndCallback( pStreamingSound, 0, this );
		FSOUND_Stream_Stop( pStreamingSound );
	}
	pStreamingSound = 0;
	delete pTrackStream;
#endif
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool CTrack::IsTimeToEndFade( NTimer::STime timeEndFade )
{
	return timePlayed + timeEndFade > playTime.GetPlayTime();
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int CTrack::operator&( IBinSaver &f ) 
{ 
	f.Add(2,&eType); 
	f.Add(3,&timePlayed); 
	f.Add(4,&timeLastCall); 
	f.Add(5,&playTime); 
	f.Add( 6, &eState );
	f.Add( 7, &pTrack );
	if ( f.IsReading() )
		OpenTrack();

	return 0; 
}
}
REGISTER_SAVELOAD_CLASS_NM( 0x111813C3, CTrack, NMusicSystem )
