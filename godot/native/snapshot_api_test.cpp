#include "bk2_presentation_internal.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool Near(float left, float right) {
    return std::fabs(left - right) < 0.0001f;
}

int Fail(const char* reason) {
    std::cerr << "snapshot_api_test_failed: " << reason << '\n';
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return Fail("missing_output_path");
    }
    if (bk2_presentation_api_version() != BK2_PRESENTATION_API_VERSION) {
        return Fail("api_version");
    }

    bk2::presentation::Reset();
    bk2::presentation::PublishMission("test/mission");
    bk2::presentation::PublishTerrain(
            {{1.0f, 2.0f, 3.0f, 0.25f, 0.75f, 0xff123456u}},
            {0, 0, 0},
            4.0f,
            5.0f,
            6.0f,
            7.0f);
    bk2::presentation::PublishWorld({}, {});
    bk2::presentation::PublishEntities(
            {{42, 2, BK2_PRESENTATION_ENTITY_ALIVE,
              8.0f, 9.0f, 10.0f, 1.25f, 75.0f}});

    const Bk2PresentationSnapshotInfo info =
            bk2_presentation_snapshot_info();
    if (info.api_version != BK2_PRESENTATION_API_VERSION ||
        info.generation != 4 ||
        info.terrain_vertex_count != 1 ||
        info.terrain_triangle_index_count != 3 ||
        info.entity_count != 1 ||
        !Near(info.center_x, 4.0f) ||
        !Near(info.world_size, 7.0f)) {
        return Fail("snapshot_info");
    }

    Bk2PresentationVertex vertex = {};
    if (bk2_presentation_copy_terrain_vertices(&vertex, 1) != 1 ||
        !Near(vertex.x, 1.0f) ||
        vertex.abgr != 0xff123456u) {
        return Fail("terrain_copy");
    }

    Bk2PresentationEntity entity = {};
    if (bk2_presentation_copy_entities(&entity, 1) != 1 ||
        entity.id != 42 ||
        entity.player != 2 ||
        !Near(entity.hit_points, 75.0f)) {
        return Fail("entity_copy");
    }

    if (!bk2_presentation_write_json(argv[1])) {
        return Fail("json_write");
    }
    std::cout << "snapshot_api_test_ok generation=" << info.generation << '\n';
    return 0;
}
