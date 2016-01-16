#include "MeshBuilder.hpp"
#include "Common.hpp"

#include "parser/Include/parser.hpp"
#include "utility/Include/MathHelper.hpp"

#include "Recast.h"
#include "DetourNavMeshBuilder.h"

#include <cassert>
#include <string>
#include <fstream>
#include <sstream>
#include <list>
#include <mutex>
#include <iomanip>

#define ZERO(x) memset(&x, 0, sizeof(x))

namespace pathfind
{
namespace build
{
namespace
{
bool Rasterize(rcContext &ctx, rcHeightfield &heightField, bool filterWalkable, float slope,
               const std::vector<utility::Vertex> &vertices, const std::vector<int> &indices, unsigned char areaFlags)
{
    if (!vertices.size() || !indices.size())
        return true;

    std::vector<float> rastVert;
    utility::Convert::VerticesToRecast(vertices, rastVert);

    std::vector<unsigned short> rastIndices;
    utility::Convert::ToShort(indices, rastIndices);

    std::vector<unsigned char> areas(indices.size() / 3, areaFlags);

    // XXX FIXME - why on earth does recast take indices as ints here, but unsigned short elsewhere? o.O
    if (filterWalkable)
        rcClearUnwalkableTriangles(&ctx, slope, &rastVert[0], vertices.size(), &indices[0], indices.size() / 3, &areas[0]);

    return rcRasterizeTriangles(&ctx, &rastVert[0], vertices.size(), &rastIndices[0], &areas[0], rastIndices.size() / 3, heightField);
}

void FilterGroundBeneathLiquid(rcHeightfield &solid)
{
    for (int i = 0; i < solid.width*solid.height; ++i)
    {
        std::list<rcSpan *> spans;

        for (rcSpan *s = solid.spans[i]; s; s = s->next)
        {
            // if we found a non-wmo liquid span, remove everything beneath it
            if (!!(s->area & AreaFlags::Liquid) && !(s->area & AreaFlags::WMO))
            {
                for (auto ns : spans)
                    ns->area = RC_NULL_AREA;

                spans.clear();
            }
            // if we found a wmo liquid span, remove every wmo span beneath it
            else if (!!(s->area & (AreaFlags::Liquid | AreaFlags::WMO)))
            {
                for (auto ns : spans)
                    if (!!(ns->area & AreaFlags::WMO))
                        ns->area = RC_NULL_AREA;

                spans.clear();
            }
            else
                spans.push_back(s);
        }
    }
}

void RestoreAdtSpans(const std::vector<rcSpan *> &spans)
{
    for (auto s : spans)
        s->area |= AreaFlags::ADT;
}

// NOTE: this does not set bmin/bmax
void InitializeRecastConfig(rcConfig &config)
{
    ZERO(config);

    config.cs = RecastSettings::TileSize / static_cast<float>(RecastSettings::TileVoxelSize);
    config.ch = RecastSettings::CellHeight;
    config.walkableSlopeAngle = RecastSettings::WalkableSlope;
    config.walkableClimb = static_cast<int>(std::round(RecastSettings::WalkableClimb / RecastSettings::CellHeight));
    config.walkableHeight = static_cast<int>(std::round(RecastSettings::WalkableHeight / RecastSettings::CellHeight));
    config.walkableRadius = static_cast<int>(std::round(RecastSettings::WalkableRadius / config.cs));
    config.maxEdgeLen = config.walkableRadius * 8;
    config.maxSimplificationError = RecastSettings::MaxSimplificationError;
    config.minRegionArea = RecastSettings::MinRegionSize;
    config.mergeRegionArea = RecastSettings::MergeRegionSize;
    config.maxVertsPerPoly = 6;
    config.tileSize = RecastSettings::TileVoxelSize;
    config.borderSize = config.walkableRadius + 3;
    config.width = config.tileSize + config.borderSize * 2;
    config.height = config.tileSize + config.borderSize * 2;
    config.detailSampleDist = 3.f;
    config.detailSampleMaxError = 1.25f;
}

bool FinishMesh(rcContext &ctx, const rcConfig &config, int tileX, int tileY, const std::string &outputFile, rcHeightfield &solid)
{
    // initialize compact height field
    std::unique_ptr<rcCompactHeightfield, decltype(&rcFreeCompactHeightfield)> chf(rcAllocCompactHeightfield(), rcFreeCompactHeightfield);

    if (!rcBuildCompactHeightfield(&ctx, config.walkableHeight, config.walkableClimb, solid, *chf))
        return false;

    //if (!rcErodeWalkableArea(&ctx, config.walkableRadius, *chf))
    //    return false;

    // we use watershed partitioning only for now.  we also have the option of monotone and partition layers.  see Sample_TileMesh.cpp for more information.

    if (!rcBuildDistanceField(&ctx, *chf))
        return false;

    if (!rcBuildRegions(&ctx, *chf, config.borderSize, config.minRegionArea, config.mergeRegionArea))
        return false;

    std::unique_ptr<rcContourSet, decltype(&rcFreeContourSet)> cset(rcAllocContourSet(), rcFreeContourSet);

    if (!rcBuildContours(&ctx, *chf, config.maxSimplificationError, config.maxEdgeLen, *cset))
        return false;

    assert(!!cset->nconts);

    std::unique_ptr<rcPolyMesh, decltype(&rcFreePolyMesh)> polyMesh(rcAllocPolyMesh(), rcFreePolyMesh);

    if (!rcBuildPolyMesh(&ctx, *cset, config.maxVertsPerPoly, *polyMesh))
        return false;

    std::unique_ptr<rcPolyMeshDetail, decltype(&rcFreePolyMeshDetail)> polyMeshDetail(rcAllocPolyMeshDetail(), rcFreePolyMeshDetail);

    if (!rcBuildPolyMeshDetail(&ctx, *polyMesh, *chf, config.detailSampleDist, config.detailSampleMaxError, *polyMeshDetail))
        return false;

    chf.reset(nullptr);
    cset.reset(nullptr);

    dtNavMeshCreateParams params;
    ZERO(params);

    params.verts = polyMesh->verts;
    params.vertCount = polyMesh->nverts;
    params.polys = polyMesh->polys;
    params.polyAreas = polyMesh->areas;
    params.polyFlags = polyMesh->flags;
    params.polyCount = polyMesh->npolys;
    params.nvp = polyMesh->nvp;
    params.detailMeshes = polyMeshDetail->meshes;
    params.detailVerts = polyMeshDetail->verts;
    params.detailVertsCount = polyMeshDetail->nverts;
    params.detailTris = polyMeshDetail->tris;
    params.detailTriCount = polyMeshDetail->ntris;
    params.walkableHeight = RecastSettings::WalkableHeight;
    params.walkableRadius = RecastSettings::WalkableRadius;
    params.walkableClimb = 1.f;
    params.tileX = tileX;
    params.tileY = tileY;
    params.tileLayer = 0;
    memcpy(params.bmin, polyMesh->bmin, sizeof(polyMesh->bmin));
    memcpy(params.bmax, polyMesh->bmax, sizeof(polyMesh->bmax));
    params.cs = config.cs;
    params.ch = config.ch;
    params.buildBvTree = true;

    unsigned char *outData;
    int outDataSize;
    if (!dtCreateNavMeshData(&params, &outData, &outDataSize))
        return false;

    std::ofstream out(outputFile, std::ofstream::binary | std::ofstream::trunc);
    out.write(reinterpret_cast<const char *>(outData), outDataSize);
    out.close();

    dtFree(outData);

    return true;
}
}

MeshBuilder::MeshBuilder(const std::string &dataPath, const std::string &outputPath, std::string &continentName) : m_outputPath(outputPath)
{
    memset(m_adtReferences, 0, sizeof(m_adtReferences));

    parser::Parser::Initialize(dataPath.c_str());

    // this must follow the parser initialization
    m_continent.reset(new parser::Continent(continentName));
}

void MeshBuilder::BuildWorkList(std::vector<std::pair<int, int>> &tiles) const
{
    tiles.reserve(64 * 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            if (m_continent->HasAdt(x, y))
                tiles.push_back(std::pair<int, int>(x, y));
}

bool MeshBuilder::IsGlobalWMO() const
{
    return !!m_continent->GetWmo();
}

void MeshBuilder::AddReference(int adtX, int adtY)
{
    if (!m_continent->HasAdt(adtX, adtY))
        return;

    std::lock_guard<std::mutex> guard(m_mutex);
    ++m_adtReferences[adtY][adtX];
}

void MeshBuilder::RemoveReference(int adtX, int adtY)
{
    if (!m_continent->HasAdt(adtX, adtY))
        return;

    std::lock_guard<std::mutex> guard(m_mutex);
    --m_adtReferences[adtY][adtX];

    if (m_adtReferences[adtY][adtX] <= 0)
    {
#ifdef _DEBUG
        std::stringstream str;
        str << "No threads need ADT (" << std::setfill(' ') << std::setw(2) << adtX << ", "
            << std::setfill(' ') << std::setw(2) << adtY << ").  Unloading.\n";
        std::cout << str.str();
#endif
        m_continent->UnloadAdt(adtX, adtY);
    }
}

bool MeshBuilder::GenerateAndSaveGlobalWMO()
{
    auto const wmo = m_continent->GetWmo();

    assert(!!wmo);

//#ifdef _DEBUG
//    wmo->WriteGlobalObjFile(m_continent->Name);
//#endif

    rcConfig config;
    InitializeRecastConfig(config);

    config.bmin[0] = -wmo->Bounds.MaxCorner.Y;
    config.bmin[1] =  wmo->Bounds.MinCorner.Z;
    config.bmin[2] = -wmo->Bounds.MaxCorner.X;

    config.bmax[0] = -wmo->Bounds.MinCorner.Y;
    config.bmax[1] =  wmo->Bounds.MaxCorner.Z;
    config.bmax[2] = -wmo->Bounds.MinCorner.X;

    rcContext ctx;

    std::unique_ptr<rcHeightfield, decltype(&rcFreeHeightField)> solid(rcAllocHeightfield(), rcFreeHeightField);

    if (!rcCreateHeightfield(&ctx, *solid, config.width, config.height, config.bmin, config.bmax, config.cs, config.ch))
        return false;

    // wmo terrain
    if (!Rasterize(ctx, *solid, true, config.walkableSlopeAngle, wmo->Vertices, wmo->Indices, AreaFlags::WMO))
        return false;

    // wmo liquid
    if (!Rasterize(ctx, *solid, false, config.walkableSlopeAngle, wmo->LiquidVertices, wmo->LiquidIndices, AreaFlags::WMO | AreaFlags::Liquid))
        return false;

    // wmo doodads
    if (!Rasterize(ctx, *solid, true, config.walkableSlopeAngle, wmo->DoodadVertices, wmo->DoodadIndices, AreaFlags::WMO | AreaFlags::Doodad))
        return false;

    FilterGroundBeneathLiquid(*solid);

    // note that no area id preservation is necessary here because we have no ADT terrain
    rcFilterLowHangingWalkableObstacles(&ctx, config.walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, config.walkableHeight, config.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, config.walkableHeight, *solid);

    std::stringstream str;

    str << m_outputPath << "\\" << m_continent->Name <<  ".map";

    return FinishMesh(ctx, config, 0, 0, str.str(), *solid);
}

bool MeshBuilder::GenerateAndSaveTile(int adtX, int adtY)
{
    const parser::Adt *adts[9] = {
        m_continent->LoadAdt(adtX - 1, adtY - 1), m_continent->LoadAdt(adtX - 0, adtY - 1), m_continent->LoadAdt(adtX + 1, adtY - 1),
        m_continent->LoadAdt(adtX - 1, adtY - 0), m_continent->LoadAdt(adtX - 0, adtY - 0), m_continent->LoadAdt(adtX + 1, adtY - 0),
        m_continent->LoadAdt(adtX - 1, adtY + 1), m_continent->LoadAdt(adtX - 0, adtY + 1), m_continent->LoadAdt(adtX + 1, adtY + 1),
    };

    const parser::Adt *thisTile = adts[4];

    assert(!!thisTile);

#ifdef _DEBUG
    thisTile->WriteObjFile();
#endif

    rcConfig config;
    InitializeRecastConfig(config);

    config.bmin[0] = -thisTile->Bounds.MaxCorner.Y;
    config.bmin[1] =  thisTile->Bounds.MinCorner.Z;
    config.bmin[2] = -thisTile->Bounds.MaxCorner.X;

    config.bmax[0] = -thisTile->Bounds.MinCorner.Y;
    config.bmax[1] =  thisTile->Bounds.MaxCorner.Z;
    config.bmax[2] = -thisTile->Bounds.MinCorner.X;

    rcContext ctx;

    std::unique_ptr<rcHeightfield, decltype(&rcFreeHeightField)> solid(rcAllocHeightfield(), rcFreeHeightField);

    if (!rcCreateHeightfield(&ctx, *solid, config.width, config.height, config.bmin, config.bmax, config.cs, config.ch))
        return false;

    std::set<int> rasterizedWmos;
    std::set<int> rasterizedDoodads;

    for (int i = 0; i < sizeof(adts) / sizeof(adts[0]); ++i)
    {
        if (!adts[i])
            continue;

        // the mesh geometry can be rasterized into the height field stages, which is good for us
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
            {
                auto const chunk = adts[i]->GetChunk(x, y);

                // adt terrain
                if (!Rasterize(ctx, *solid, false, config.walkableSlopeAngle, chunk->m_terrainVertices, chunk->m_terrainIndices, AreaFlags::ADT))
                    return false;

                // liquid
                if (!Rasterize(ctx, *solid, false, config.walkableSlopeAngle, chunk->m_liquidVertices, chunk->m_liquidIndices, AreaFlags::Liquid))
                    return false;

                // wmos (and included doodads and liquid)
                for (auto const &wmoId : chunk->m_wmos)
                {
                    if (rasterizedWmos.find(wmoId) != rasterizedWmos.end())
                        continue;

                    auto const wmo = m_continent->GetWmo(wmoId);

                    assert(wmo);

                    if (!Rasterize(ctx, *solid, true, config.walkableSlopeAngle, wmo->Vertices, wmo->Indices, AreaFlags::WMO))
                        return false;

                    if (!Rasterize(ctx, *solid, false, config.walkableSlopeAngle, wmo->LiquidVertices, wmo->LiquidIndices, AreaFlags::WMO | AreaFlags::Liquid))
                        return false;

                    if (!Rasterize(ctx, *solid, true, config.walkableSlopeAngle, wmo->DoodadVertices, wmo->DoodadIndices, AreaFlags::WMO | AreaFlags::Doodad))
                        return false;
                }

                // doodads
                for (auto const &doodadId : chunk->m_doodads)
                {
                    if (rasterizedDoodads.find(doodadId) != rasterizedDoodads.end())
                        continue;

                    auto const doodad = m_continent->GetDoodad(doodadId);

                    assert(doodad);

                    if (!Rasterize(ctx, *solid, true, config.walkableSlopeAngle, doodad->Vertices, doodad->Indices, AreaFlags::Doodad))
                        return false;
                }
            }

        FilterGroundBeneathLiquid(*solid);

        // save all span area flags because we dont want the upcoming filtering to apply to ADT terrain
        {
            std::vector<rcSpan *> adtSpans;

            for (int i = 0; i < solid->width * solid->height; ++i)
                for (rcSpan *s = solid->spans[i]; s; s = s->next)
                    if (!!(s->area & AreaFlags::ADT))
                        adtSpans.push_back(s);

            rcFilterLowHangingWalkableObstacles(&ctx, config.walkableClimb, *solid);

            RestoreAdtSpans(adtSpans);

            rcFilterLedgeSpans(&ctx, config.walkableHeight, config.walkableClimb, *solid);

            RestoreAdtSpans(adtSpans);

            rcFilterWalkableLowHeightSpans(&ctx, config.walkableHeight, *solid);

            RestoreAdtSpans(adtSpans);
        }
    }

    std::stringstream str;

    str << m_outputPath << "\\" << m_continent->Name << "_" << adtX << "_" << adtY << ".map";

    return FinishMesh(ctx, config, adtX, adtY, str.str(), *solid);
}
}
}