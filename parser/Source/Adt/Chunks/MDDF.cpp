#include "Doodad/DoodadPlacement.hpp"
#include "ADT/Chunks/MDDF.hpp"

#include "utility/Include/BinaryStream.hpp"

namespace parser
{
namespace input
{
MDDF::MDDF(size_t position, utility::BinaryStream *reader) : AdtChunk(position, reader)
{
    if (!Size)
        return;

    Doodads.resize(Size / sizeof(DoodadPlacement));

    reader->SetPosition(position + 8);
    reader->ReadBytes(&Doodads[0], sizeof(DoodadPlacement) * Doodads.size());
}
}
}