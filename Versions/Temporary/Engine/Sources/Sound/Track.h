#pragma once
#include "PlayElement.h"

#if defined(BK2_ANDROID)
namespace bk2
{
	namespace android
	{
		class AndroidVorbisStream;
	}
}
#else
#include "../vendor/fmod/api/inc/fmod.h"
#endif
#include "PlayTime.h"
#include "MusicSystem.h"

namespace NDb
{
	struct SPlayTime;
	struct SMusicTrack;
}

namespace NMusicSystem
{
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
class CTrack : public IPlayListElement
{
	OBJECT_NOCOPY_METHODS( CTrack )

#if defined(BK2_ANDROID)
	bk2::android::AndroidVorbisStream *pStreamingSound;
	int nChannel;
	NTimer::STime trackDuration;
	std::string szAndroidTrackPath;
#else
	FSOUND_STREAM * pStreamingSound;
#endif
	enum ETrackState
	{
		ETS_NOT_STARTED,
		ETS_PLAYING,
		ETS_FINISHED,
	};

	EStreamType eType;
	NTimer::STime timePlayed;
	NTimer::STime timeLastCall;
	CPlayTime playTime;
	CDBPtr<NDb::SMusicTrack> pTrack;
	ETrackState eState;
#if !defined(BK2_ANDROID)
	CDataStream *pTrackStream;
#endif

	void PlayTrack( int nTrackTime );
	void OpenTrack();
public:
	int operator&( IBinSaver &f );
	
public:
	CTrack()
		: pStreamingSound( 0 )
#if defined(BK2_ANDROID)
		, nChannel( 0 ), trackDuration( 0 )
#else
		, pTrackStream( 0 )
#endif
	{ }
	CTrack( const NDb::SMusicTrack *_pTrack, const NDb::SPlayTime *_pPlayTime, EStreamType _eType )
		: playTime( _pPlayTime, _pTrack ), eType( _eType ), pTrack( _pTrack ), pStreamingSound( 0 ), eState( ETS_NOT_STARTED ),
		timePlayed( 0 ), timeLastCall( 0 )
#if defined(BK2_ANDROID)
		, nChannel( 0 ), trackDuration( 0 )
#else
		, pTrackStream( 0 )
#endif
	{
	}
	~CTrack();

	void NotifyTrackFinished();

	void Segment();
	bool IsFinished() const;
	void Stop();

	const CPlayTime &GetPlayTime() const { return playTime; }
	bool IsTimeToEndFade( NTimer::STime timeEndFade );
	void Play();
	void OnResetTimer();
};
}
