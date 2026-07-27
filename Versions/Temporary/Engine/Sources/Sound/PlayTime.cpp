#include "StdAfx.h"
#include "playtime.h"

#include "DBMusicSystem.h"
#include "../Misc/Win32Random.h"
#if defined(BK2_ANDROID)
#include "bk2_android_vorbis_stream.h"
#else
#include "../vendor/fmod/api/inc/fmod.h"
#include "../System/VFSOperations.h"
#endif
#include "MusicSystem.hpp"

namespace NMusicSystem
{
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//	CPlayTime
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
CPlayTime::CPlayTime( const NDb::SPlayTime *_pPlayTime, const NDb::SMusicTrack *pTrack )
: nPlayTime( 0 )
{
	if ( !pTrack || !_pPlayTime )
		return;
	const float fRandom ( NWin32Random::Random( 0.0f, 1.0f ) );
	if ( _pPlayTime->nNumer != 0 )
	{	
#if defined(BK2_ANDROID)
		bk2::android::AndroidAudioMetadata metadata;
		std::string error;
		if ( bk2::android::ReadAndroidAudioMetadata(
				std::string( pTrack->szMusicFileName.c_str() ),
				&metadata, &error ) )
		{
			nPlayTime = static_cast<int>( metadata.duration_ms ) *
				(_pPlayTime->nNumer +
				 int( fRandom * _pPlayTime->nNumberRandom ));
		}
#else
		CFileStream trackStream( NVFS::GetMainVFS(), pTrack->szMusicFileName );
		FSOUND_STREAM * pStreamingSound = OpenTrack( &trackStream );
//		NI_ASSERT( pStreamingSound != 0, StrFmt( "cannot open stream file %s", pTrack->szMusicFileName ) );
		if ( pStreamingSound )
		{
			nPlayTime = FSOUND_Stream_GetLengthMs( pStreamingSound ) * (_pPlayTime->nNumer + int( fRandom * _pPlayTime->nNumberRandom ) );
			FSOUND_Stream_Close( pStreamingSound );
		}
#endif
	}
	else if ( _pPlayTime->nPlayTime != 0 )
		nPlayTime = _pPlayTime->nPlayTime + int( _pPlayTime->nPlayTimeRandom * fRandom );
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
NTimer::STime CPlayTime::GetPlayTime(  ) const
{
	return nPlayTime;
}
}
