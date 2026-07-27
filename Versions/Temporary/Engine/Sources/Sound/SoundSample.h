#pragma once
#if defined(BK2_ANDROID)
#include "bk2_android_audio_decode.h"
#include <cstddef>
#include <string>
#else
#include "../vendor/fmod/api/inc/fmod.h"
#endif
#include "..\System\GResource.h"

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
class CSoundSample : public CObjectBase
{
	OBJECT_NOCOPY_METHODS( CSoundSample );
	static bool b3DSoundShare;
	//
#if defined(BK2_ANDROID)
	bk2::android::DecodedPcmClip decodedClip;
	bool bLooped;
#else
	FSOUND_SAMPLE *sample;								// FMOD sound sample
#endif
	CDBID dbidSound;
	//
	void Close();
#if !defined(BK2_ANDROID)
	void SetSample( FSOUND_SAMPLE *_sample );
#endif
public:
	static void Set3DMode( bool b3DMode ) { b3DSoundShare = b3DMode; }
	CSoundSample();
	~CSoundSample();
	int operator&( IBinSaver &saver );
#if !defined(_FINALRELEASE)
	string GetName() const { return dbidSound.ToString(); }
#endif
	//
#if defined(BK2_ANDROID)
	bool LoadFromMemory( const void *pData, size_t nSize, std::string *pError = 0 );
	bool IsLoaded() const;
	const bk2::android::DecodedPcmClip& GetDecodedClip() const;
	bool IsLooped() const;
#else
	FSOUND_SAMPLE* GetInternalContainer();
#endif
	void SetLoop( bool bEnable );
	void SetKey( const CDBID &dbid );
};
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
