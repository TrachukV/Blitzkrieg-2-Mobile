#include "bk2_android_ai_runtime.h"

#include "../../../../../Versions/Temporary/Engine/Sources/Common_RTS_AI/stdafx.h"
#include "../../../../../Versions/Temporary/Engine/Sources/Common_RTS_AI/AIMap.h"
#include "../../../../../Versions/Temporary/Engine/Sources/Common_RTS_AI/CommonPathFinder.h"
#include "../../../../../Versions/Temporary/Engine/Sources/Common_RTS_AI/StaticMapHeights.h"
#include "../../../../../Versions/Temporary/Engine/Sources/Common_RTS_AI/Terrain.h"
#include "../../../../../Versions/Temporary/Engine/Sources/SceneB2/TerrainInfo.h"
#include "../../../../../Versions/Temporary/Engine/Sources/Stats_B2_M1/DBMapInfo.h"
#include "../../../../../Versions/Temporary/Engine/Sources/Stats_B2_M1/Vis2AI.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

namespace bk2::android {
namespace {

constexpr float kPackedHeightScale = 0.01f;

CObj<CAIMap> g_ai_map;
bool g_ready = false;
int g_terrain_type_count = 0;
int g_terrain_grid_width = 0;
int g_terrain_grid_height = 0;
int g_water_tile_count = 0;
int g_free_track_tile_count = 0;
bool g_route_found = false;
int g_route_length = 0;
SVector g_route_start(-1, -1);
SVector g_route_finish(-1, -1);

int HeightWidth(const STerrainInfo& info)
{
    return info.heights.GetSizeX() > 0
            ? info.heights.GetSizeX()
            : info.optimizedHeights.GetSizeX();
}

int HeightHeight(const STerrainInfo& info)
{
    return info.heights.GetSizeY() > 0
            ? info.heights.GetSizeY()
            : info.optimizedHeights.GetSizeY();
}

float FullVisualHeight(const STerrainInfo& info, int x, int y)
{
    float height = info.heights.GetSizeX() > 0
            ? info.heights[y][x]
            : static_cast<float>(info.optimizedHeights[y][x]) *
                    kPackedHeightScale;
    if (info.addHeights.GetSizeX() == HeightWidth(info) &&
        info.addHeights.GetSizeY() == HeightHeight(info))
    {
        height += info.addHeights[y][x];
    }
    else if (info.optimizedAddHeights.GetSizeX() == HeightWidth(info) &&
             info.optimizedAddHeights.GetSizeY() == HeightHeight(info))
    {
        height += static_cast<float>(info.optimizedAddHeights[y][x]) *
                kPackedHeightScale;
    }
    return height;
}

bool IsSeaTile(const STerrainInfo& info, int x, int y)
{
    if (!info.seaMask.IsEmpty() &&
        x < info.seaMask.GetSizeX() &&
        y < info.seaMask.GetSizeY())
    {
        return info.seaMask[y][x] != 0;
    }
    if (!info.optimizedSeaMask.IsEmpty() &&
        x < info.optimizedSeaMask.GetSizeX() &&
        y < info.optimizedSeaMask.GetSizeY())
    {
        return info.optimizedSeaMask.GetData(x, y) != 0;
    }
    return false;
}

int RequiredTerrainTypeCount(const STerrainInfo& info)
{
    int count = 1;
    for (int y = 0; y < info.tileTerraMap.GetSizeY(); ++y)
    {
        for (int x = 0; x < info.tileTerraMap.GetSizeX(); ++x)
        {
            count = std::max(
                    count,
                    static_cast<int>(info.tileTerraMap[y][x]) + 1);
        }
    }
    return count;
}

void InitializeTerrainProperties(
        const NDb::SMapInfo* map,
        const STerrainInfo& info,
        CTerrain* terrain)
{
    const NDb::STGTerraSet* terrain_set =
            map->pTerraSet.IsEmpty() ? nullptr : map->pTerraSet.GetPtr();
    const int database_type_count = terrain_set == nullptr
            ? 0
            : static_cast<int>(terrain_set->terraTypes.size());
    g_terrain_type_count =
            std::max(database_type_count, RequiredTerrainTypeCount(info));
    terrain->PrepareTerraTypes(g_terrain_type_count);

    for (int index = 0; index < g_terrain_type_count; ++index)
    {
        const NDb::STGTerraType* type = nullptr;
        if (terrain_set != nullptr && index < database_type_count)
        {
            type = terrain_set->terraTypes[index].GetPtr();
        }
        if (type != nullptr)
        {
            const NDb::STerrainAIProperties& properties = type->aIProperty;
            terrain->SetTerraTypes(
                    index,
                    properties.fPassability,
                    properties.nAIClass,
                    static_cast<BYTE>(properties.nSoilType),
                    properties.bCanEntrench ? 0 : 1);
        }
        else
        {
            terrain->SetTerraTypes(index, 1.0f, EAC_TERRAIN, 0, 0);
        }
    }
}

bool InitializeTerrainGrid(
        const STerrainInfo& info,
        CTerrain* terrain,
        std::string* error)
{
    g_terrain_grid_width = info.tileTerraMap.GetSizeX();
    g_terrain_grid_height = info.tileTerraMap.GetSizeY();
    if (g_ai_map)
    {
        g_terrain_grid_width = std::min(
                g_terrain_grid_width,
                g_ai_map->GetSizeX() / 2);
        g_terrain_grid_height = std::min(
                g_terrain_grid_height,
                g_ai_map->GetSizeY() / 2);
    }
    if (g_terrain_grid_width <= 0 || g_terrain_grid_height <= 0)
    {
        *error = "ai_terrain_type_grid_missing";
        return false;
    }

    CArray2D<BYTE> terrain_types(
            g_terrain_grid_width,
            g_terrain_grid_height);
    g_water_tile_count = 0;
    for (int y = 0; y < g_terrain_grid_height; ++y)
    {
        for (int x = 0; x < g_terrain_grid_width; ++x)
        {
            if (IsSeaTile(info, x, y))
            {
                terrain_types[y][x] = 0xff;
                ++g_water_tile_count;
            }
            else
            {
                terrain_types[y][x] = info.tileTerraMap[y][x];
            }
        }
    }
    terrain->UpdateTypes(
            0,
            0,
            g_terrain_grid_width,
            g_terrain_grid_height,
            terrain_types);
    return true;
}

bool InitializeTerrainHeights(
        const STerrainInfo& info,
        CStaticMapHeights* heights,
        std::string* error)
{
    const int width = HeightWidth(info);
    const int height = HeightHeight(info);
    if (width <= 0 || height <= 0)
    {
        *error = "ai_terrain_heights_missing";
        return false;
    }

    CArray2D<float> ai_heights(width, height);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            ai_heights[y][x] = Vis2AI(FullVisualHeight(info, x, y));
        }
    }
    heights->UpdateHeights(0, 0, width, height, ai_heights);
    heights->FinalizeUpdateHeights();
    return true;
}

SVector FindNearestFreeTile(
        const SVector& requested,
        const CTerrain* terrain,
        const CAIMap* map)
{
    SVector best(-1, -1);
    int best_distance = 0x7fffffff;
    for (int y = 8; y < map->GetSizeY() - 8; ++y)
    {
        for (int x = 8; x < map->GetSizeX() - 8; ++x)
        {
            const SVector tile(x, y);
            if (terrain->CanUnitGo(0, tile, EAC_TRACK) == FREE_NONE)
            {
                continue;
            }
            const int distance =
                    std::abs(x - requested.x) + std::abs(y - requested.y);
            if (distance < best_distance)
            {
                best = tile;
                best_distance = distance;
            }
        }
    }
    return best;
}

void ValidatePathFinder(const NDb::SMapInfo* map)
{
    CTerrain* terrain = g_ai_map->GetTerrain();
    g_free_track_tile_count = 0;
    for (int y = 0; y < g_ai_map->GetSizeY(); ++y)
    {
        for (int x = 0; x < g_ai_map->GetSizeX(); ++x)
        {
            if (terrain->CanUnitGo(0, SVector(x, y), EAC_TRACK) != FREE_NONE)
            {
                ++g_free_track_tile_count;
            }
        }
    }

    std::vector<SVector> requested_tiles;
    for (size_t index = 0;
         index < map->objects.size() && requested_tiles.size() < 8;
         ++index)
    {
        const NDb::SMapObjectInfo& object = map->objects[index];
        if (object.pObject.GetPtrNoLoad() == nullptr)
        {
            continue;
        }
        const int type_id = object.pObject.GetPtrNoLoad()->GetTypeID();
        if (type_id == NDb::SMechUnitRPGStats::typeID ||
            type_id == NDb::SSquadRPGStats::typeID)
        {
            requested_tiles.push_back(g_ai_map->GetTile(object.vPos.x, object.vPos.y));
        }
    }
    if (requested_tiles.empty())
    {
        requested_tiles.push_back(
                SVector(g_ai_map->GetSizeX() / 3, g_ai_map->GetSizeY() / 2));
    }

    g_route_start = FindNearestFreeTile(
            requested_tiles.front(),
            terrain,
            g_ai_map);
    if (g_route_start.x < 0)
    {
        return;
    }

    CObj<CCommonPathFinder> path_finder = new CCommonPathFinder();
    const SVector offsets[] = {
            SVector(48, 0),
            SVector(0, 48),
            SVector(32, 32),
            SVector(-48, 0),
            SVector(0, -48),
    };
    for (const SVector& offset : offsets)
    {
        const SVector requested_finish(
                Clamp(g_route_start.x + offset.x, 8, g_ai_map->GetSizeX() - 9),
                Clamp(g_route_start.y + offset.y, 8, g_ai_map->GetSizeY() - 9));
        const SVector finish =
                FindNearestFreeTile(requested_finish, terrain, g_ai_map);
        if (finish.x < 0 || finish == g_route_start)
        {
            continue;
        }
        path_finder->SetPathParameters(
                0,
                EAC_TRACK,
                g_ai_map->GetPointByTile(g_route_start),
                g_ai_map->GetPointByTile(finish),
                g_route_start,
                g_ai_map);
        if (path_finder->DoesPathExist())
        {
            g_route_found = true;
            g_route_finish = finish;
            g_route_length = path_finder->GetPathLength();
            return;
        }
    }
}

}

bool InitializeAIRuntime(
        const NDb::SMapInfo* map,
        const STerrainInfo& terrain_info,
        std::string* error)
{
    ShutdownAIRuntime();
    if (map == nullptr)
    {
        *error = "ai_map_resource_missing";
        return false;
    }

    const int map_width = map->nNumPatchesX * AI_TILES_IN_PATCH;
    const int map_height = map->nNumPatchesY * AI_TILES_IN_PATCH;
    if (map_width <= 0 || map_height <= 0)
    {
        *error = "ai_map_dimensions_invalid";
        return false;
    }

    g_ai_map = new CAIMap(
            map_width,
            map_height,
            AI_TILE_SIZE,
            MAXIMUM_UNIT_TILE_RADIUS,
            MAXIMUM_MAP_SIZE);
    CTerrain* terrain = g_ai_map->GetTerrain();
    InitializeTerrainProperties(map, terrain_info, terrain);
    if (!InitializeTerrainGrid(terrain_info, terrain, error) ||
        !InitializeTerrainHeights(
                terrain_info,
                g_ai_map->GetHeights(),
                error))
    {
        ShutdownAIRuntime();
        return false;
    }

    terrain->FinishInitMode();
    ValidatePathFinder(map);
    g_ready = true;
    return true;
}

void ShutdownAIRuntime()
{
    g_ai_map = 0;
    g_ready = false;
    g_terrain_type_count = 0;
    g_terrain_grid_width = 0;
    g_terrain_grid_height = 0;
    g_water_tile_count = 0;
    g_free_track_tile_count = 0;
    g_route_found = false;
    g_route_length = 0;
    g_route_start = SVector(-1, -1);
    g_route_finish = SVector(-1, -1);
}

std::string AIRuntimeReport()
{
    std::ostringstream report;
    report << "ai_runtime=" << (g_ready ? "ready" : "not_ready");
    if (g_ai_map)
    {
        report << "; ai_map=" << g_ai_map->GetSizeX()
               << "x" << g_ai_map->GetSizeY();
    }
    report << "; terrain_types=" << g_terrain_type_count
           << "; terrain_grid=" << g_terrain_grid_width
           << "x" << g_terrain_grid_height
           << "; water_tiles=" << g_water_tile_count
           << "; free_track_tiles=" << g_free_track_tile_count
           << "; pathfinder_route=" << (g_route_found ? "found" : "not_found")
           << "; route_length=" << g_route_length;
    if (g_route_start.x >= 0)
    {
        report << "; route=" << g_route_start.x << "," << g_route_start.y
               << "->" << g_route_finish.x << "," << g_route_finish.y;
    }
    return report.str();
}

}
