// SPDX-License-Identifier: GPL-3.0-or-later
#include "airplay/rtsp.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>

namespace airplay::rtsp
{
namespace
{

bool iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

const std::string* Message::header(std::string_view name) const
{
    for (const auto& [key, value] : headers)
    {
        if (iequals(key, name))
        {
            return &value;
        }
    }
    return nullptr;
}

void Message::setHeader(std::string name, std::string value)
{
    for (auto& [key, existing] : headers)
    {
        if (iequals(key, name))
        {
            existing = std::move(value);
            return;
        }
    }
    headers.emplace_back(std::move(name), std::move(value));
}

std::string Message::contentType() const
{
    const std::string* value = header("Content-Type");
    return value != nullptr ? *value : std::string{};
}

std::optional<size_t> parseRequest(const Bytes& buffer, Message& out)
{
    // Headers end at the first blank line.
    static constexpr std::string_view kTerminator = "\r\n\r\n";
    const std::string_view text(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    const size_t header_end = text.find(kTerminator);
    if (header_end == std::string_view::npos)
    {
        return 0;  // incomplete
    }
    const size_t body_start = header_end + kTerminator.size();

    std::string_view head = text.substr(0, header_end);

    // Request line: METHOD SP URI SP VERSION
    const size_t line_end = head.find("\r\n");
    const std::string_view request_line = head.substr(0, line_end);
    const size_t first_space = request_line.find(' ');
    if (first_space == std::string_view::npos)
    {
        return std::nullopt;
    }
    const size_t second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos)
    {
        return std::nullopt;
    }

    Message message;
    message.method = std::string(request_line.substr(0, first_space));
    message.uri =
        std::string(request_line.substr(first_space + 1, second_space - first_space - 1));
    message.version = std::string(trim(request_line.substr(second_space + 1)));

    // Header lines.
    size_t cursor = (line_end == std::string_view::npos) ? head.size() : line_end + 2;
    while (cursor < head.size())
    {
        size_t next = head.find("\r\n", cursor);
        if (next == std::string_view::npos)
        {
            next = head.size();
        }
        const std::string_view line = head.substr(cursor, next - cursor);
        const size_t colon = line.find(':');
        if (colon != std::string_view::npos)
        {
            message.headers.emplace_back(std::string(trim(line.substr(0, colon))),
                                         std::string(trim(line.substr(colon + 1))));
        }
        cursor = next + 2;
    }

    size_t content_length = 0;
    if (const std::string* value = message.header("Content-Length"); value != nullptr)
    {
        const std::string_view digits = trim(*value);
        if (std::from_chars(digits.data(), digits.data() + digits.size(), content_length).ec !=
            std::errc{})
        {
            return std::nullopt;
        }
    }

    if (buffer.size() < body_start + content_length)
    {
        return 0;  // body still arriving
    }

    message.body.assign(buffer.begin() + static_cast<long>(body_start),
                        buffer.begin() + static_cast<long>(body_start + content_length));
    out = std::move(message);
    return body_start + content_length;
}

Bytes serializeResponse(const Message& response)
{
    std::string head = response.version + " " + std::to_string(response.status) + " " +
                       response.reason + "\r\n";
    for (const auto& [key, value] : response.headers)
    {
        head += key + ": " + value + "\r\n";
    }
    // Always present, so the peer can frame a zero-length body too.
    if (response.header("Content-Length") == nullptr)
    {
        head += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    }
    head += "\r\n";

    Bytes out(head.begin(), head.end());
    out.insert(out.end(), response.body.begin(), response.body.end());
    return out;
}

Message makeResponse(int status, std::string reason, std::string content_type, Bytes body)
{
    Message message;
    message.status = status;
    message.reason = std::move(reason);
    message.body = std::move(body);
    if (!content_type.empty())
    {
        message.setHeader("Content-Type", std::move(content_type));
    }
    return message;
}

}  // namespace airplay::rtsp
