#include "../../../../../Versions/Temporary/Engine/Sources/Common_RTS_AI/stdafx.h"
#include "../../../../../Versions/Temporary/Engine/Sources/Image/ImageTGA.h"

namespace
{
#pragma pack(push, 1)
struct TgaHeader
{
    unsigned char id_length = 0;
    unsigned char color_map_type = 0;
    unsigned char image_type = 2;
    unsigned short color_map_first = 0;
    unsigned short color_map_length = 0;
    unsigned char color_map_depth = 0;
    unsigned short x_origin = 0;
    unsigned short y_origin = 0;
    unsigned short width = 0;
    unsigned short height = 0;
    unsigned char pixel_depth = 32;
    unsigned char descriptor = 0x28;
};
#pragma pack(pop)
}

namespace NImage
{
bool SaveImageAsTGA(CDataStream *stream, const CArray2D<DWORD> &image)
{
    if (stream == nullptr || image.IsEmpty() || image.GetSizeX() > 0xffff ||
        image.GetSizeY() > 0xffff)
    {
        return false;
    }

    TgaHeader header;
    header.width = static_cast<unsigned short>(image.GetSizeX());
    header.height = static_cast<unsigned short>(image.GetSizeY());
    stream->Write(&header, sizeof(header));

    for (int y = 0; y < image.GetSizeY(); ++y)
    {
        stream->Write(image[y], image.GetSizeX() * sizeof(DWORD));
    }
    return true;
}
}
