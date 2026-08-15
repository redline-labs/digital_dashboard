// SPDX-License-Identifier: GPL-3.0-or-later
//
// What goes wrong opening or building a road graph.
#ifndef ROAD_GRAPH_ERROR_H
#define ROAD_GRAPH_ERROR_H

#include <cstddef>
#include <expected>
#include <string>

namespace road_graph
{

struct Error
{
    enum class Kind
    {
        NotFound,
        NotReadable,
        // Wrong magic, or a version this build does not write. Refused rather
        // than reinterpreted: the artifact is rebuilt offline in minutes, so
        // there is never a reason to read an old one wrongly.
        NotAGraph,
        VersionMismatch,
        // A section that should be there is not, or its length is not a
        // multiple of its element size.
        Malformed,
        InvalidArgument,
    };

    Kind kind { Kind::Malformed };
    std::string message;
};

const char* to_string(Error::Kind kind);
std::string to_string(const Error& error);

template <typename T>
using Result = std::expected<T, Error>;

std::unexpected<Error> not_found(std::string message);
std::unexpected<Error> not_readable(std::string message);
std::unexpected<Error> not_a_graph(std::string message);
std::unexpected<Error> version_mismatch(std::string message);
std::unexpected<Error> malformed(std::string message);
std::unexpected<Error> invalid_argument(std::string message);

} // namespace road_graph

#endif // ROAD_GRAPH_ERROR_H
