#include "Sound/stdafx.h"

#include "Sound/Sound2D.h"
#include "Sound/SoundEngine.h"
#include "System/BasicShare.h"

#include "bk2_android_audio_backend.h"

extern CBasicShare<CDBID, CSoundSample> shareSoundSample;

REGISTER_SAVELOAD_CLASS( 0x1107BC01, CSoundEngine );

namespace {

float s_distanceFactor = 10.0f;
float s_rolloffFactor = 0.3f;

}

CSoundEngine::CSoundEngine()
	: timeLastUpdate( 0 ),
	  bInited( false ),
	  bEnableSFX( true ),
	  bEnableStreaming( false ),
	  bSoundCardPresent( false ),
	  bPaused( false ),
	  bStreamingPaused( false ),
	  b3DMode( false ),
	  vFormerListener( VNULL3 )
{
}

CSoundEngine::~CSoundEngine()
{
	ClearChannels();
	channelsMap.clear();
	soundsMap.clear();
	bk2::android::AudioBackend().stop_all();
}

bool CSoundEngine::SearchDevices()
{
	drivers.clear();
	SDriverInfo driver;
	driver.szDriverName = "Android native mixer";
	driver.isHardware3DAccelerated = false;
	driver.supportEAXReverb = false;
	driver.supportReverb = false;
	driver.supportEAX3 = false;
	drivers.push_back( driver );
	return true;
}

bool CSoundEngine::IsInitialized()
{
	return bInited;
}

CObjectBase* CSoundEngine::QI( int nInterfaceTypeID )
{
	return 0;
}

bool CSoundEngine::Init( HWND hWnd, int nDriver, ESFXOutputType output, int nMixRate, int nMaxChannels )
{
	if ( !SearchDevices() )
		return false;

	if ( output == SFX_OUTPUT_NO )
	{
		bSoundCardPresent = false;
		bInited = true;
		return true;
	}

	const bool initialized = bk2::android::AudioBackend().init( nMixRate, nMaxChannels );
	bSoundCardPresent = initialized;
	bInited = initialized;
	if ( initialized )
	{
		SetDistanceFactor( s_distanceFactor );
		SetRolloffFactor( s_rolloffFactor );
	}
	return initialized;
}

void CSoundEngine::SetDistanceFactor( float fFactor )
{
	s_distanceFactor = Max( fFactor, 0.000001f );
	bk2::android::AudioBackend().set_distance_factor( s_distanceFactor );
}

void CSoundEngine::SetRolloffFactor( float fFactor )
{
	NI_VERIFY( fFactor >= 0.0f && fFactor <= 10.0f,
		StrFmt( "Rolloff factor (%g) must be in range [0..10]", fFactor ), return );
	s_rolloffFactor = fFactor;
	bk2::android::AudioBackend().set_rolloff_factor( s_rolloffFactor );
}

void CSoundEngine::Update( const CVec3 &vListener, const CVec3 &vCameraDir, NTimer::STime timeDiff )
{
	bk2::android::AudioListener listener;
	listener.position[0] = vListener.x;
	listener.position[1] = vListener.z;
	listener.position[2] = vListener.y;
	listener.forward[0] = vCameraDir.x;
	listener.forward[1] = vCameraDir.z;
	listener.forward[2] = vCameraDir.y;
	listener.up[0] = 0.0f;
	listener.up[1] = 1.0f;
	listener.up[2] = 0.0f;
	bk2::android::AudioBackend().update_listener( listener );
	vFormerListener = vListener;
	timeLastUpdate = GetTickCount();
	ClearChannels();
}

void CSoundEngine::MapSound( ISound *pSound, int nChannel )
{
	if ( pSound == 0 || nChannel < 0 )
		return;

	CSoundChannelMap::iterator existing = channelsMap.find( pSound );
	if ( existing != channelsMap.end() )
		StopChannel( existing->second );

	channelsMap.insert( pair<ISound*, int>( pSound, nChannel ) );
	soundsMap.insert( pair<int, CPtr<ISound> >( nChannel, pSound ) );
}

bool CSoundEngine::IsPaused()
{
	return bPaused;
}

bool CSoundEngine::Pause( bool bPause )
{
	bPaused = bPause;
	bk2::android::AudioBackend().set_paused( bPause );
	return bPause;
}

void CSoundEngine::ClearChannels()
{
	if ( bPaused )
		return;

	list<int> channelsToClear;
	for ( CChannelSoundMap::iterator it = soundsMap.begin(); it != soundsMap.end(); ++it )
	{
		const bool valid = IsValid( it->second );
		if ( !valid || !bk2::android::AudioBackend().is_playing( it->first ) )
		{
			if ( !valid )
				bk2::android::AudioBackend().stop( it->first );
			channelsToClear.push_back( it->first );
		}
	}

	for ( list<int>::iterator it = channelsToClear.begin(); it != channelsToClear.end(); ++it )
	{
		CChannelSoundMap::iterator sound = soundsMap.find( *it );
		if ( sound == soundsMap.end() )
			continue;
		ISound *pSound = sound->second;
		soundsMap.erase( sound );
		channelsMap.erase( pSound );
	}
}

int CSoundEngine::PlaySample( ISound *pSound, bool bLooped, unsigned int nStartPos )
{
	if ( pSound == 0 || !bEnableSFX || !bSoundCardPresent )
		return -1;

	CSFXSound *pSFXSound = dynamic_cast<CSFXSound*>( pSound );
	if ( pSFXSound == 0 || pSFXSound->GetSample() == 0 )
		return -1;

	CSoundSample *pSample = pSFXSound->GetSample();
	if ( !pSample->IsLoaded() )
		return -1;

	const unsigned int nLength = pSound->GetLenght();
	if ( nLength == 0 )
		return -1;
	if ( bLooped )
		nStartPos %= nLength;
	else if ( nStartPos >= nLength )
		return -1;

	pSample->SetLoop( bLooped );
	const int nChannel = bk2::android::AudioBackend().play(
		pSample->GetDecodedClip().view(), bLooped, nStartPos );
	if ( nChannel < 0 )
		return -1;

	pSFXSound->SetChannel( nChannel );
	MapSound( pSound, nChannel );
	pSFXSound->Update( this );
	return nChannel;
}

void CSoundEngine::UpdateSample( ISound *pSound )
{
	CSFXSound *pSFXSound = dynamic_cast<CSFXSound*>( pSound );
	if ( pSFXSound )
		pSFXSound->Update( this );
}

void CSoundEngine::StopSample( ISound *pSound )
{
	CSoundChannelMap::iterator it = channelsMap.find( pSound );
	if ( it != channelsMap.end() )
		StopChannel( it->second );
}

bool CSoundEngine::IsPlaying( ISound *pSound )
{
	if ( pSound == 0 )
		return false;
	CSoundChannelMap::iterator it = channelsMap.find( pSound );
	return it != channelsMap.end() && bk2::android::AudioBackend().is_playing( it->second );
}

void CSoundEngine::StopChannel( int nChannel )
{
	if ( nChannel < 0 )
		return;

	bk2::android::AudioBackend().stop( nChannel );
	CChannelSoundMap::iterator it = soundsMap.find( nChannel );
	if ( it != soundsMap.end() )
	{
		ISound *pSound = it->second;
		soundsMap.erase( it );
		channelsMap.erase( pSound );
	}
}

unsigned int CSoundEngine::GetCurrentPosition( ISound *pSound )
{
	CSoundChannelMap::iterator it = channelsMap.find( pSound );
	return it == channelsMap.end()
		? 0
		: bk2::android::AudioBackend().current_position( it->second );
}

void CSoundEngine::SetCurrentPosition( ISound *pSound, unsigned int pos )
{
	CSoundChannelMap::iterator it = channelsMap.find( pSound );
	if ( it != channelsMap.end() )
		bk2::android::AudioBackend().set_current_position( it->second, pos );
}

void CSoundEngine::Set3DMode( bool bEnabled )
{
	b3DMode = bEnabled;
	CSoundSample::Set3DMode( bEnabled );
	if ( bEnabled )
	{
		SetDistanceFactor( s_distanceFactor );
		SetRolloffFactor( s_rolloffFactor );
	}
	ClearChannels();
}

void CSoundEngine::ReEnableSounds()
{
	if ( bEnableSFX )
		return;
	bk2::android::AudioBackend().stop_all();
	soundsMap.clear();
	channelsMap.clear();
}

int CSoundEngine::operator&( IBinSaver &saver )
{
	if ( saver.IsReading() )
	{
		channelsMap.clear();
		soundsMap.clear();
		shareSoundSample.Clear();
	}

	saver.Add( 3, &bSoundCardPresent );
	saver.Add( 4, &timeLastUpdate );
	saver.Add( 12, &bPaused );
	saver.Add( 13, &bStreamingPaused );
	saver.Add( 14, &b3DMode );
	saver.Add( 15, &vFormerListener );

	if ( saver.IsReading() )
		Pause( true );
	return 0;
}

ISFX *CreateSoundEngine()
{
	return new CSoundEngine();
}
