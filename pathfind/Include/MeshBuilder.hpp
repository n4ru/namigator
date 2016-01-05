#pragma once

#include "utility/Include/LinearAlgebra.hpp"
#include "DataManager.hpp"

#include <vector>
#include <string>

namespace pathfind
{
namespace build
{
class MeshBuilder
{
    private:
    static constexpr float TileSize = 533.f + (1.f / 3.f);
    static constexpr float CellHeight = 0.4f;

    static constexpr float WalkableHeight = 1.6f;   // agent height in world units (yards)
    static constexpr float WalkableRadius = 0.3f;   // narrowest allowable hallway in world units (yards)

    static constexpr float MaxSimplificationError = 1.3f;

    static const int TileVoxelSize = 1800;
    static const int MinRegionSize = 20;
    static const int MergeRegionSize = 40;

    static void ConvertVerticesToRecast(const std::vector<utility::Vertex> &input, std::vector<float> &output);
    static void ConvertVerticesToWow(const std::vector<float> &input, std::vector<utility::Vertex> &output);

    static void ConvertToShort(const std::vector<int> &input, std::vector<unsigned short> &output);

    DataManager *const m_dataManager;

    public:
    MeshBuilder(DataManager *dataManager);

    bool GenerateTile(int adtX, int adtY);
};
}
}