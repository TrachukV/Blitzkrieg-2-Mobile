#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BK2_PRESENTATION_API_VERSION 3u
#define BK2_PRESENTATION_MISSION_ID_CAPACITY 256u

enum Bk2PresentationEntityFlags {
    BK2_PRESENTATION_ENTITY_ALIVE = 1u << 0,
    BK2_PRESENTATION_ENTITY_SELECTABLE = 1u << 1,
    BK2_PRESENTATION_ENTITY_MOVABLE = 1u << 2,
    BK2_PRESENTATION_ENTITY_SELECTED = 1u << 3,
    BK2_PRESENTATION_ENTITY_MECHANIZED = 1u << 4,
    BK2_PRESENTATION_ENTITY_INFANTRY = 1u << 5,
    BK2_PRESENTATION_ENTITY_FORMATION = 1u << 6,
    BK2_PRESENTATION_ENTITY_TARGETED = 1u << 7,
    BK2_PRESENTATION_ENTITY_MOVING = 1u << 8,
    BK2_PRESENTATION_ENTITY_ATTACKING = 1u << 9,
    BK2_PRESENTATION_ENTITY_DEAD = 1u << 10,
    BK2_PRESENTATION_ENTITY_LYING = 1u << 11,
};

typedef struct Bk2PresentationVertex {
    float x;
    float y;
    float z;
    float u;
    float v;
    uint32_t abgr;
} Bk2PresentationVertex;

typedef struct Bk2PresentationEntity {
    int32_t id;
    int32_t player;
    uint32_t flags;
    float x;
    float y;
    float z;
    float heading_radians;
    float hit_points;
    float max_hit_points;
    uint64_t rpg_stats_path_hash;
    int32_t rpg_stats_record_id;
    int32_t geometry_record_id;
    float visual_scale;
    /* Turret and gun angles the AI aimed this unit's platform to, in radians
       relative to the hull. Zero when the unit has no rotating platform. */
    float turret_yaw_radians;
    float turret_pitch_radians;
} Bk2PresentationEntity;

typedef struct Bk2PresentationSnapshotInfo {
    uint32_t api_version;
    uint64_t generation;
    size_t terrain_vertex_count;
    size_t terrain_triangle_index_count;
    size_t world_vertex_count;
    size_t world_triangle_index_count;
    size_t entity_count;
    float center_x;
    float center_y;
    float center_z;
    float world_size;
    char mission_id[BK2_PRESENTATION_MISSION_ID_CAPACITY];
} Bk2PresentationSnapshotInfo;

uint32_t bk2_presentation_api_version(void);
Bk2PresentationSnapshotInfo bk2_presentation_snapshot_info(void);

size_t bk2_presentation_copy_terrain_vertices(
        Bk2PresentationVertex* output,
        size_t capacity);
size_t bk2_presentation_copy_terrain_triangle_indices(
        uint32_t* output,
        size_t capacity);
size_t bk2_presentation_copy_world_vertices(
        Bk2PresentationVertex* output,
        size_t capacity);
size_t bk2_presentation_copy_world_triangle_indices(
        uint32_t* output,
        size_t capacity);
size_t bk2_presentation_copy_entities(
        Bk2PresentationEntity* output,
        size_t capacity);

int bk2_presentation_write_json(const char* path);

#ifdef __cplusplus
}
#endif
