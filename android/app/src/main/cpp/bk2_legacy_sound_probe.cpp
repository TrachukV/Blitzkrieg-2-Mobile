#include <array>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "Sound/stdafx.h"

#include "Sound/SFX.h"
#include "Sound/Sound2D.h"
#include "Sound/SoundSample.h"

#include "bk2_android_audio_backend.h"
#include "bk2_legacy_sound_probe.h"

namespace bk2::android {
namespace {

void AppendU16(std::vector<uint8_t>* bytes, uint16_t value) {
	bytes->push_back(static_cast<uint8_t>(value & 0xff));
	bytes->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void AppendU32(std::vector<uint8_t>* bytes, uint32_t value) {
	bytes->push_back(static_cast<uint8_t>(value & 0xff));
	bytes->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
	bytes->push_back(static_cast<uint8_t>((value >> 16) & 0xff));
	bytes->push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

std::vector<uint8_t> MakePcmWav() {
	constexpr uint16_t kChannels = 1;
	constexpr uint32_t kSampleRate = 24000;
	constexpr uint16_t kBitsPerSample = 16;
	constexpr uint32_t kFrameCount = 64;
	constexpr uint32_t kDataSize = kFrameCount * sizeof(int16_t);

	std::vector<uint8_t> bytes;
	bytes.reserve( 44 + kDataSize );
	bytes.insert( bytes.end(), { 'R', 'I', 'F', 'F' } );
	AppendU32( &bytes, 36 + kDataSize );
	bytes.insert( bytes.end(), { 'W', 'A', 'V', 'E' } );
	bytes.insert( bytes.end(), { 'f', 'm', 't', ' ' } );
	AppendU32( &bytes, 16 );
	AppendU16( &bytes, 1 );
	AppendU16( &bytes, kChannels );
	AppendU32( &bytes, kSampleRate );
	AppendU32( &bytes, kSampleRate * kChannels * kBitsPerSample / 8 );
	AppendU16( &bytes, kChannels * kBitsPerSample / 8 );
	AppendU16( &bytes, kBitsPerSample );
	bytes.insert( bytes.end(), { 'd', 'a', 't', 'a' } );
	AppendU32( &bytes, kDataSize );
	for ( uint32_t i = 0; i < kFrameCount; ++i )
		AppendU16( &bytes, static_cast<uint16_t>( i % 8 < 4 ? 12000 : -12000 ) );
	return bytes;
}

int64_t AbsoluteSampleSum(const std::vector<int16_t>& samples) {
	int64_t sum = 0;
	for ( int16_t sample : samples )
		sum += std::abs( static_cast<int>( sample ) );
	return sum;
}

}  // namespace

std::string RunLegacySoundProbe() {
	const std::vector<uint8_t> wav = MakePcmWav();
	CPtr<CSoundSample> sample = new CSoundSample();
	std::string decodeError;
	const bool decoded = sample->LoadFromMemory( wav.data(), wav.size(), &decodeError );

	CPtr<ISFX> engine = CreateSoundEngine();
	const bool initialized = engine->Init( 0, 0, SFX_OUTPUT_ANDROID, 48000, 8 );

	CPtr<CSound2D> sound2D = new CSound2D();
	sound2D->SetSample( sample );
	sound2D->SetVolume( 1.0f );
	sound2D->SetPan( -0.25f );
	const int channel2D = decoded && initialized ? engine->PlaySample( sound2D, false, 3 ) : -1;
	std::vector<int16_t> mixed2D( 12 * 2 );
	AudioBackend().mix_interleaved_stereo( mixed2D.data(), 12 );
	const unsigned int positionAfterMix = engine->GetCurrentPosition( sound2D );
	engine->SetCurrentPosition( sound2D, 5 );
	const unsigned int positionAfterSeek = engine->GetCurrentPosition( sound2D );
	const bool playing2D = engine->IsPlaying( sound2D );
	engine->StopSample( sound2D );
	const bool stopped2D = !engine->IsPlaying( sound2D );

	engine->Set3DMode( true );
	CPtr<CSound3D> sound3D = new CSound3D();
	sound3D->SetSample( sample );
	sound3D->SetVolume( 1.0f );
	sound3D->SetMinMax( 0.0f, 100.0f );
	sound3D->SetPos( CVec3( 1.0f, 0.0f, 0.0f ) );
	const int channel3D = decoded && initialized ? engine->PlaySample( sound3D, true, 0 ) : -1;
	engine->Update( VNULL3, CVec3( 0.0f, 1.0f, 0.0f ), 16 );
	std::vector<int16_t> nearMix( 8 * 2 );
	AudioBackend().mix_interleaved_stereo( nearMix.data(), 8 );
	engine->SetCurrentPosition( sound3D, 0 );
	sound3D->SetPos( CVec3( 100.0f, 0.0f, 0.0f ) );
	engine->UpdateSample( sound3D );
	std::vector<int16_t> farMix( 8 * 2 );
	AudioBackend().mix_interleaved_stereo( farMix.data(), 8 );
	const int64_t nearEnergy = AbsoluteSampleSum( nearMix );
	const int64_t farEnergy = AbsoluteSampleSum( farMix );
	const bool attenuationWorks = nearEnergy > 0 && farEnergy < nearEnergy;
	engine->StopSample( sound3D );

	std::ostringstream report;
	report << "legacy_isfx=" << ( decoded && initialized ? "probed" : "failed" )
		   << "; legacy_decode=" << ( decoded ? "true" : "false" )
		   << "; legacy_2d_channel=" << channel2D
		   << "; legacy_2d_nonzero=" << ( AbsoluteSampleSum( mixed2D ) > 0 ? "true" : "false" )
		   << "; legacy_position_after_mix=" << positionAfterMix
		   << "; legacy_seek_position=" << positionAfterSeek
		   << "; legacy_playing=" << ( playing2D ? "true" : "false" )
		   << "; legacy_stopped=" << ( stopped2D ? "true" : "false" )
		   << "; legacy_3d_channel=" << channel3D
		   << "; legacy_3d_near_energy=" << nearEnergy
		   << "; legacy_3d_far_energy=" << farEnergy
		   << "; legacy_3d_attenuation=" << ( attenuationWorks ? "true" : "false" );
	if ( !decodeError.empty() )
		report << "; legacy_decode_error=" << decodeError;
	return report.str();
}

}  // namespace bk2::android
