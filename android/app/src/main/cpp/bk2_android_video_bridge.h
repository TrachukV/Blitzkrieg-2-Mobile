#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace bk2::android {

std::string AndroidVideoRefForLegacyBink(std::string_view source_ref);
std::vector<std::string> AndroidVideoRefsForLegacyMovie(std::string_view source_ref);
std::string AndroidVideoPathForLegacyBink(std::string_view source_ref);
std::vector<std::string> AndroidVideoPathsForLegacyMovie(std::string_view source_ref);
void RequestFullscreenVideo(std::string_view video_path);
void RequestFullscreenVideos(const std::vector<std::string>& video_paths);
void RequestFullscreenVideoForLegacyBink(std::string_view source_ref);
void RequestFullscreenVideoForLegacyMovie(std::string_view source_ref);

}  // namespace bk2::android
