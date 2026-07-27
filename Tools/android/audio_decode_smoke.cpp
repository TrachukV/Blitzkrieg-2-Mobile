#include "../../android/app/src/main/cpp/bk2_android_audio_decode.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

uint64_t PcmChecksum(const std::vector<int16_t>& samples) {
    uint64_t checksum = 1469598103934665603ull;
    for (int16_t sample : samples) {
        const uint16_t value = static_cast<uint16_t>(sample);
        checksum ^= static_cast<uint8_t>(value & 0xff);
        checksum *= 1099511628211ull;
        checksum ^= static_cast<uint8_t>((value >> 8) & 0xff);
        checksum *= 1099511628211ull;
    }
    return checksum;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: audio_decode_smoke <file.wav>\n";
        return 2;
    }

    const std::string path = argv[1];
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "failed to open: " << path << '\n';
        return 2;
    }

    const std::vector<uint8_t> bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
    bk2::android::DecodedPcmClip decoded;
    std::string error;
    if (!bk2::android::DecodeWavToPcm16(
                bytes.data(), bytes.size(), &decoded, &error)) {
        std::cerr << "decode failed: " << error << '\n';
        return 1;
    }

    size_t non_zero_samples = 0;
    for (int16_t sample : decoded.samples) {
        if (sample != 0) {
            ++non_zero_samples;
        }
    }

    std::cout << "audio_decode=ok"
              << "; format=" << decoded.source_format_tag
              << "; channels=" << decoded.channel_count
              << "; sample_rate=" << decoded.sample_rate
              << "; frames=" << decoded.frame_count()
              << "; nonzero_samples=" << non_zero_samples
              << "; checksum=" << PcmChecksum(decoded.samples)
              << '\n';
    return 0;
}
