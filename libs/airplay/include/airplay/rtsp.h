// SPDX-License-Identifier: GPL-3.0-or-later
//
// RTSP/1.0 message framing for the AirPlay receiver. CarPlay speaks RTSP over
// TCP on port 7000, with binary bodies (TLV8 or binary plist) rather than the
// SDP that RTSP normally carries.
#ifndef AIRPLAY_RTSP_H_
#define AIRPLAY_RTSP_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace airplay::rtsp
{

using Bytes = std::vector<uint8_t>;

// One request or response. The same type serves both directions; which fields
// are meaningful depends on how it was produced.
struct Message
{
    // Request line.
    std::string method;
    std::string uri;
    std::string version = "RTSP/1.0";

    // Status line (responses).
    int status = 200;
    std::string reason = "OK";

    // Preserved in order; RTSP allows repeats, so this is not a map.
    std::vector<std::pair<std::string, std::string>> headers;
    Bytes body;

    // Case-insensitive lookup, as required by RFC 2326.
    const std::string* header(std::string_view name) const;
    void setHeader(std::string name, std::string value);

    // Content-Type of the body, or empty.
    std::string contentType() const;
};

// Parses one complete request from the front of `buffer`.
//
// Returns the number of bytes consumed, or 0 when more data is needed. A
// malformed request line or an unparseable Content-Length yields nullopt, which
// the caller should treat as fatal for that connection.
std::optional<size_t> parseRequest(const Bytes& buffer, Message& out);

// Serialises a response. Content-Length is always emitted so the peer can frame
// the body, including when it is empty.
Bytes serializeResponse(const Message& response);

// Convenience: a response carrying a binary body.
Message makeResponse(int status, std::string reason, std::string content_type, Bytes body);

}  // namespace airplay::rtsp

#endif  // AIRPLAY_RTSP_H_
