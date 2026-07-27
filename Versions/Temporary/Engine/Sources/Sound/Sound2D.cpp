#include "StdAfx.h"
#include ".\sound2d.h"
#include "DBSoundDesc.h"
#include "..\System\BasicShare.h"
#include "../System/Commands.h"
#if defined(BK2_ANDROID)
#include "bk2_android_audio_backend.h"
#endif

CBasicShare<CDBID, CSoundSample> shareSoundSample(120, false);

REGISTER_SAVELOAD_CLASS( 0x110B8B00, CSound2D );
REGISTER_SAVELOAD_CLASS( 0x11191C00, CSound3D );


float g_fSFXVolume = 0.5f;
float g_fVoiceVolume = 0.5f;
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//CSFXSound
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int CSFXSound::operator&( IBinSaver &saver )
{
	saver.Add( 1, &pSample );
	saver.Add( 2, &pDesc );
	saver.Add( 3, &nChannel );
	saver.Add( 4, &bLooped );
	saver.Add( 5, &nVolumeType );

	if ( saver.IsReading() )
	{
		nChannel = -1;
		Init();
	}
	return 0;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CSFXSound::Init()
{
	if ( !pSample )
	{
		pSample = shareSoundSample.Get( pDesc->GetDBID() );
		pSample->SetLoop( bLooped );
	}
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CSFXSound::SetSample( CSoundSample *_pSample )
{ 
	pSample = _pSample; 
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int CSFXSound::Play() 
{ 
#if defined(BK2_ANDROID)
	if ( GetSample() == 0 || !GetSample()->IsLoaded() )
		return -1;
	int nChannel = bk2::android::AudioBackend().play(
		GetSample()->GetDecodedClip().view(), GetSample()->IsLooped(), 0 );
#else
	int nChannel = FSOUND_PlaySound( FSOUND_FREE, GetSample()->GetInternalContainer() );
#endif
	SetChannel( nChannel );
	return nChannel;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
CSoundSample* CSFXSound::GetSample() 
{ 
	return pSample; 
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CSFXSound::SetChannel( int _nChannel ) 
{ 
	nChannel = _nChannel; 
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool CSFXSound::IsPlaying() 
{ 
#if defined(BK2_ANDROID)
	return nChannel != -1 && bk2::android::AudioBackend().is_playing( nChannel );
#else
	if ( (nChannel != -1) && FSOUND_IsPlaying(nChannel) )
		return FSOUND_GetCurrentSample( nChannel ) == pSample->GetInternalContainer();
	else
		return false;
#endif
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CSFXSound::SetLooping( bool bEnable, int nStart, int nEnd )
{
#if defined(BK2_ANDROID)
	if ( pSample )
		pSample->SetLoop( bEnable );
#else
	FSOUND_Sample_SetMode( pSample->GetInternalContainer(), bEnable ? FSOUND_LOOP_NORMAL : FSOUND_LOOP_OFF );
	if ( (nStart != -1) && (nEnd != -1) )
		FSOUND_Sample_SetLoopPoints( pSample->GetInternalContainer(), nStart, nEnd );
#endif
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
unsigned int CSFXSound::GetLenght()
{
#if defined(BK2_ANDROID)
	return pSample ? static_cast<unsigned int>( pSample->GetDecodedClip().frame_count() ) : 0;
#else
	return FSOUND_Sample_GetLength( pSample->GetInternalContainer() );
#endif
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
unsigned int CSFXSound::GetSampleRate()
{
#if defined(BK2_ANDROID)
	return pSample ? static_cast<unsigned int>( pSample->GetDecodedClip().sample_rate ) : 0;
#else
	int freq = 44000;
	FSOUND_Sample_GetDefaults( pSample->GetInternalContainer(), &freq, 0, 0, 0 );
	return freq;
#endif
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// CSound2D
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int CSound2D::operator&( IBinSaver &saver )
{
	saver.Add( 2, static_cast<CSFXSound*>( this ) );
	saver.Add( 3, &fVolume );
	saver.Add( 4, &fPan );

	return 0;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
CSound2D::CSound2D( const NDb::SSoundDesc *_pDesc, const bool _bLooped )
: CSFXSound( _pDesc, _bLooped ),
	fVolume( 0.0f ), fPan( 0.0f )
{
	Init();
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int CSound2D::Visit( interface ISFXVisitor *pVisitor ) 
{ 
	return pVisitor->VisitSound2D( this ); 
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CSound2D::Update( ISFX * pSFX )
{
	if ( GetChannel() != -1 )
	{
#if defined(BK2_ANDROID)
		bk2::android::AudioBackend().set_pan( GetChannel(), Clamp( GetPan(), -1.0f, 1.0f ) );
#else
		const int nPan = Clamp( int(128 + (GetPan() * 127)), 0, 255 );
		FSOUND_SetPan( GetChannel(), nPan );
#endif
		
		float fTypedVolume = 0;
		switch( GetVolumeType() )
		{
		case 2:
			fTypedVolume = g_fSFXVolume;
			break;
		case 1:
			fTypedVolume = g_fVoiceVolume;
			break;
		}
#if defined(BK2_ANDROID)
		const float fVolume = Clamp(
			fTypedVolume * ( GetVolume() >= 0.0f ? GetVolume() : 1.0f ), 0.0f, 1.0f );
		bk2::android::AudioBackend().set_volume( GetChannel(), fVolume );
#else
		const int nVolume = Clamp( int( fTypedVolume * ( GetVolume() >= 0.0f ? GetVolume() * 255.0f : 255.0f ) ), 0, 255 );
		FSOUND_SetVolume( GetChannel(), nVolume );
#endif
	}
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CSound2D::Init()
{
	CSoundSample::Set3DMode( false );
	CSFXSound::Init();
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// CSound3D
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
CSound3D::CSound3D( const NDb::SSoundDesc *_pDesc, const bool _bLooped )
: CSFXSound( _pDesc, _bLooped ),
	vPos( VNULL3 ), vSpeed( VNULL3 ), fMax( 1000000000.0f ), fMin( 1.0f ), fVolume( 1.0f )
{
	Init();
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int CSound3D::Visit( interface ISFXVisitor *pVisitor )
{
	return pVisitor->VisitSound3D( this );
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CSound3D::Update( ISFX * pSFX )
{
	if ( GetChannel() != -1 )
	{
		const CVec3 vP( vPos.x, vPos.z, vPos.y );
		const CVec3 vS( vSpeed.x, vSpeed.z, vSpeed.y );
#if defined(BK2_ANDROID)
		const float fChannelVolume = Clamp( GetVolume() >= 0.0f ? GetVolume() : 1.0f, 0.0f, 1.0f );
		const float position[3] = { vP.x, vP.y, vP.z };
		const float velocity[3] = { vS.x, vS.y, vS.z };
		bk2::android::AudioBackend().set_volume( GetChannel(), fChannelVolume );
		bk2::android::AudioBackend().set_spatial_position(
			GetChannel(), position, velocity, fMin, fMax );
#else
		//if ( fMax != 0.0f )
			//FSOUND_3D_SetMinMaxDistance( GetChannel(), fMin, fMax );
		const int nVolume = Clamp( int(GetVolume() >= 0.0f ? GetVolume() * 255.0f : 255.0f), 0, 255 );
		FSOUND_SetVolume( GetChannel(), nVolume );
		FSOUND_3D_SetAttributes( GetChannel(), vP.m, vS.m );
#endif
	}
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CSound3D::Init()
{
	CSoundSample::Set3DMode( true );
	CSFXSound::Init();
}

START_REGISTER(Sound)
REGISTER_VAR_EX( "Sound.SFXVolume", NGlobal::VarFloatHandler, &g_fSFXVolume, 0.5f, STORAGE_USER );
REGISTER_VAR_EX( "Sound.VoiceVolume", NGlobal::VarFloatHandler, &g_fVoiceVolume, 0.5f, STORAGE_USER );
FINISH_REGISTER
