#pragma once

#include "bk2_presentation_api.h"

#include <string>
#include <vector>

namespace bk2::presentation {

void Reset();
void PublishMission(std::string mission_id);
void PublishTerrain(
        std::vector<Bk2PresentationVertex> vertices,
        std::vector<uint32_t> triangle_indices,
        float center_x,
        float center_y,
        float center_z,
        float world_size);
void PublishWorld(
        std::vector<Bk2PresentationVertex> vertices,
        std::vector<uint32_t> triangle_indices);
void PublishEntities(std::vector<Bk2PresentationEntity> entities);

}  // namespace bk2::presentation
