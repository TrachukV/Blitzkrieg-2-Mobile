#include "StdAfx.h"

#include ".\soundsample.h"
#include "DBSoundDesc.h"
#include "../System/VFSOperations.h"

bool CSoundSample::b3DSoundShare = false;
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// ************************************************************************************************************************ //
// **
// ** base shared sound sample resource
// **
// **
// **
// ************************************************************************************************************************ //
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
CSoundSample::CSoundSample() 
#if defined(BK2_ANDROID)
: bLooped( false )
#else
: sample( 0 )
#endif
{
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
CSoundSample::~CSoundSample() 
{ 
	Close(); 
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CSoundSample::Close() 
{ 
#if defined(BK2_ANDROID)
	decodedClip = bk2::android::DecodedPcmClip();
	bLooped = false;
#else
	if ( sample ) 
		FSOUND_Sample_Free( sample ); 
	sample = 0; 
#endif
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#if !defined(BK2_ANDROID)
void CSoundSample::SetSample( FSOUND_SAMPLE *_sample ) 
{ 
	Close(); 
	sample = _sample; 
	if ( sample )
		FSOUND_Sample_SetMinMaxDistance( sample, 45, 1000000000.0f );
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FSOUND_SAMPLE* CSoundSample::GetInternalContainer() 
{ 
	return sample; 
}
#else
bool CSoundSample::LoadFromMemory( const void *pData, size_t nSize, std::string *pError )
{
	Close();
	if ( pData == 0 || nSize == 0 )
	{
		if ( pError )
			*pError = "sound sample is empty";
		return false;
	}
	return bk2::android::DecodeWavToPcm16(
		static_cast<const uint8_t*>( pData ), nSize, &decodedClip, pError );
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool CSoundSample::IsLoaded() const
{
	return decodedClip.frame_count() != 0;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
const bk2::android::DecodedPcmClip& CSoundSample::GetDecodedClip() const
{
	return decodedClip;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool CSoundSample::IsLooped() const
{
	return bLooped;
}
#endif
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void CSoundSample::SetLoop( bool bEnable ) 
{ 
#if defined(BK2_ANDROID)
	bLooped = bEnable;
#else
	if ( sample )
		FSOUND_Sample_SetMode( sample, bEnable ? FSOUND_LOOP_NORMAL : FSOUND_LOOP_OFF ); 
#endif
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void CSoundSample::SetKey( const CDBID &dbid )
{
	dbidSound = dbid;
	// load info
	const NDb::SSoundDesc *pDesc = NDb::Get<NDb::SSoundDesc>( dbidSound );
	NI_ASSERT( pDesc != 0, StrFmt( "wrong sound Desc DBID \"%s\"", dbidSound.ToString().c_str() ) );
	if ( pDesc )
	{
		CFileStream stream( NVFS::GetMainVFS(), pDesc->szSoundPath );
		if ( stream.IsOk() )
		{
			const int nSize = stream.GetSize();
			vector<char> data(nSize);
			stream.Read( &data[0], nSize );
#if defined(BK2_ANDROID)
			std::string error;
			if ( !LoadFromMemory( &data[0], nSize, &error ) )
				DebugTrace( "Cannot decode sound sample %s: %s", pDesc->szSoundPath.c_str(), error.c_str() );
#else
			FSOUND_SAMPLE *pSample = FSOUND_Sample_Load( FSOUND_UNMANAGED, &data[0], /*( b3DSoundShare ? FSOUND_HW3D : FSOUND_2D ) |*/ FSOUND_LOADMEMORY, 0, nSize );
			SetSample( pSample );
#endif
		}
	}
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int CSoundSample::operator&( IBinSaver &saver )
{
	saver.Add( 5, &dbidSound );
	if ( saver.IsReading() )
	{
#if defined(BK2_ANDROID)
		Close();
#else
		sample = 0;
#endif
		SetKey( dbidSound );
	}
	return 0;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
REGISTER_SAVELOAD_CLASS( 0x110B2C00, CSoundSample );
