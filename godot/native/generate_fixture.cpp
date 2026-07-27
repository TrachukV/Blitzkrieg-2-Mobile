#include "bk2_presentation_internal.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int kGridSize = 25;
constexpr float kSpacing = 8.0f;
constexpr float kPi = 3.14159265358979323846f;

uint32_t ColorForHeight(float height) {
    return height < 0.0f ? 0xff80704au : 0xff6b8f55u;
}

void AppendPyramid(
        std::vector<Bk2PresentationVertex>* vertices,
        std::vector<uint32_t>* indices,
        float x,
        float y,
        float z,
        uint32_t color) {
    const uint32_t base = static_cast<uint32_t>(vertices->size());
    vertices->push_back({x - 2.0f, y - 2.0f, z, 0.0f, 0.0f, color});
    vertices->push_back({x + 2.0f, y - 2.0f, z, 1.0f, 0.0f, color});
    vertices->push_back({x + 2.0f, y + 2.0f, z, 1.0f, 1.0f, color});
    vertices->push_back({x - 2.0f, y + 2.0f, z, 0.0f, 1.0f, color});
    vertices->push_back({x, y, z + 7.0f, 0.5f, 0.5f, color});
    const uint32_t local_indices[] = {
            0, 2, 1, 0, 3, 2,
            0, 1, 4, 1, 2, 4,
            2, 3, 4, 3, 0, 4,
    };
    for (const uint32_t index : local_indices) {
        indices->push_back(base + index);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: bk2_snapshot_fixture OUTPUT_JSON\n";
        return 2;
    }

    std::vector<Bk2PresentationVertex> terrain_vertices;
    std::vector<uint32_t> terrain_indices;
    terrain_vertices.reserve(kGridSize * kGridSize);
    for (int y = 0; y < kGridSize; ++y) {
        for (int x = 0; x < kGridSize; ++x) {
            const float height =
                    std::sin(static_cast<float>(x) * 0.35f) * 2.8f +
                    std::cos(static_cast<float>(y) * 0.27f) * 2.2f;
            terrain_vertices.push_back({
                    x * kSpacing,
                    y * kSpacing,
                    height,
                    static_cast<float>(x) / (kGridSize - 1),
                    1.0f - static_cast<float>(y) / (kGridSize - 1),
                    ColorForHeight(height)});
        }
    }
    for (int y = 0; y < kGridSize - 1; ++y) {
        for (int x = 0; x < kGridSize - 1; ++x) {
            const uint32_t top_left = static_cast<uint32_t>(y * kGridSize + x);
            const uint32_t top_right = top_left + 1;
            const uint32_t bottom_left = top_left + kGridSize;
            const uint32_t bottom_right = bottom_left + 1;
            terrain_indices.insert(
                    terrain_indices.end(),
                    {top_left, bottom_left, top_right,
                     top_right, bottom_left, bottom_right});
        }
    }

    std::vector<Bk2PresentationVertex> world_vertices;
    std::vector<uint32_t> world_indices;
    AppendPyramid(&world_vertices, &world_indices, 44.0f, 72.0f, 2.0f, 0xff57c8ffu);
    AppendPyramid(&world_vertices, &world_indices, 136.0f, 112.0f, 1.0f, 0xff4f5bddu);
    AppendPyramid(&world_vertices, &world_indices, 104.0f, 152.0f, 0.0f, 0xff4aa367u);

    std::vector<Bk2PresentationEntity> entities;
    for (int index = 0; index < 12; ++index) {
        const int column = index % 4;
        const int row = index / 4;
        entities.push_back({
                1000 + index,
                row == 0 ? 1 : 2,
                BK2_PRESENTATION_ENTITY_ALIVE |
                        BK2_PRESENTATION_ENTITY_SELECTABLE |
                        BK2_PRESENTATION_ENTITY_MOVABLE,
                66.0f + column * 12.0f,
                88.0f + row * 14.0f,
                4.0f,
                row == 0 ? 0.0f : kPi,
                100.0f});
    }

    const float world_size = (kGridSize - 1) * kSpacing;
    bk2::presentation::Reset();
    bk2::presentation::PublishMission(
            "/Scenario/Campaigns/USA/Chapter1/US1.0/MapInfo.xdb");
    bk2::presentation::PublishTerrain(
            std::move(terrain_vertices),
            std::move(terrain_indices),
            world_size * 0.5f,
            world_size * 0.5f,
            0.0f,
            world_size);
    bk2::presentation::PublishWorld(
            std::move(world_vertices),
            std::move(world_indices));
    bk2::presentation::PublishEntities(std::move(entities));

    if (!bk2_presentation_write_json(argv[1])) {
        std::cerr << "failed to write " << argv[1] << '\n';
        return 1;
    }
    const Bk2PresentationSnapshotInfo info = bk2_presentation_snapshot_info();
    std::cout << "fixture_written generation=" << info.generation
              << " terrain_vertices=" << info.terrain_vertex_count
              << " entities=" << info.entity_count << '\n';
    return 0;
}
