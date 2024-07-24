#ifndef MEDIA_TYPE_H_
#define MEDIA_TYPE_H_

#include <string_view>

enum class MediaType
{
    Data = 1,
    AlbumCover = 3
};


constexpr std::string_view media_type_to_string(MediaType type)
{
    switch (type)
    {
        case (MediaType::Data):
            return "Data";

        case (MediaType::AlbumCover):
            return "AlbumCover";

        default:
            return "Unknown";
    }
}

#endif  // MEDIA_TYPE_H_