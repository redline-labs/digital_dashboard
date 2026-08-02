// SPDX-License-Identifier: GPL-3.0-or-later
//
// RTSP message framing -- the parser every byte the phone sends passes through
// first.
//
// It is fed straight off a TCP socket, so the cases that matter are the ones a
// hand-test never produces: a request split across segments at an awkward byte,
// two requests in one read, a binary body that happens to contain the header
// terminator or a NUL. Getting any of those wrong presents as "the phone hung",
// several layers away from here, with nothing in a log.
#include "airplay/rtsp.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>
#include <string_view>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

airplay::rtsp::Bytes bytes(std::string_view text)
{
    return airplay::rtsp::Bytes(text.begin(), text.end());
}

std::string text(const airplay::rtsp::Bytes& data)
{
    return std::string(data.begin(), data.end());
}

}  // namespace

int main()
{
    using airplay::rtsp::Bytes;
    using airplay::rtsp::makeResponse;
    using airplay::rtsp::Message;
    using airplay::rtsp::parseRequest;
    using airplay::rtsp::serializeResponse;

    // A complete request, and the byte count the caller erases.
    {
        const Bytes wire = bytes("GET /info RTSP/1.0\r\n"
                                 "CSeq: 3\r\n"
                                 "Content-Length: 5\r\n"
                                 "\r\n"
                                 "hello");
        Message request;
        const auto consumed = parseRequest(wire, request);
        expect(consumed.has_value() && *consumed == wire.size(), "consumes exactly the request");
        expect(request.method == "GET", "method");
        expect(request.uri == "/info", "uri");
        expect(request.version == "RTSP/1.0", "version");
        expect(text(request.body) == "hello", "body");
        const std::string* cseq = request.header("CSeq");
        expect(cseq != nullptr && *cseq == "3", "header value");
    }

    // No headers and no body at all.
    {
        const Bytes wire = bytes("OPTIONS * RTSP/1.0\r\n\r\n");
        Message request;
        const auto consumed = parseRequest(wire, request);
        expect(consumed.has_value() && *consumed == wire.size(), "bare request consumes fully");
        expect(request.method == "OPTIONS" && request.uri == "*", "bare request parses");
        expect(request.body.empty(), "and has no body");
    }

    // Incomplete input asks for more rather than failing. Both stages of it:
    // headers not finished, and headers finished but body still arriving.
    {
        Message request;
        expect(parseRequest(bytes(""), request) == std::optional<size_t>(0), "empty asks for more");
        expect(parseRequest(bytes("GET /info RTSP/1.0\r\n"), request) == std::optional<size_t>(0),
               "partial headers ask for more");
        expect(parseRequest(bytes("GET /info RTSP/1.0\r\nContent-Length: 4\r\n\r\nab"), request) ==
                   std::optional<size_t>(0),
               "partial body asks for more");
    }

    // The real delivery pattern: one byte at a time. It must ask for more at
    // every prefix and succeed exactly once, on the last byte.
    {
        const Bytes whole = bytes("SETUP rtsp://x RTSP/1.0\r\n"
                                  "CSeq: 9\r\n"
                                  "Content-Length: 3\r\n"
                                  "\r\n"
                                  "abc");
        bool premature = false;
        for (size_t n = 0; n < whole.size(); ++n)
        {
            Message request;
            const Bytes prefix(whole.begin(), whole.begin() + static_cast<long>(n));
            const auto consumed = parseRequest(prefix, request);
            if (!consumed.has_value() || *consumed != 0)
            {
                premature = true;
            }
        }
        expect(!premature, "no prefix of a request parses as complete");

        Message request;
        const auto consumed = parseRequest(whole, request);
        expect(consumed.has_value() && *consumed == whole.size(), "the last byte completes it");
        expect(text(request.body) == "abc", "and the body is intact");
    }

    // Two requests in one read. The count is what lets the caller find the
    // second, so an off-by-one here loses every request after the first.
    {
        const std::string first = "GET /info RTSP/1.0\r\nCSeq: 1\r\n\r\n";
        const std::string second = "RECORD rtsp://x RTSP/1.0\r\nCSeq: 2\r\n\r\n";
        Bytes wire = bytes(first + second);

        Message a;
        const auto consumed_a = parseRequest(wire, a);
        expect(consumed_a.has_value() && *consumed_a == first.size(),
               "the first request consumes only itself");
        expect(a.method == "GET", "first request method");

        wire.erase(wire.begin(), wire.begin() + static_cast<long>(*consumed_a));
        Message b;
        const auto consumed_b = parseRequest(wire, b);
        expect(consumed_b.has_value() && *consumed_b == second.size(), "then the second parses");
        expect(b.method == "RECORD", "second request method");
    }

    // A binary body containing the header terminator. The parser scans the
    // whole buffer for "\r\n\r\n", so a body carrying one must not be mistaken
    // for the end of the headers -- and a plist can contain any byte sequence.
    {
        const std::string head = "POST /command RTSP/1.0\r\nContent-Length: 8\r\n\r\n";
        Bytes wire = bytes(head);
        const Bytes body{0x62, '\r', '\n', '\r', '\n', 0x00, 0x7F, 0x41};
        wire.insert(wire.end(), body.begin(), body.end());

        Message request;
        const auto consumed = parseRequest(wire, request);
        expect(consumed.has_value() && *consumed == wire.size(),
               "a body containing the terminator still frames correctly");
        expect(request.body == body, "and survives byte for byte, NUL included");
    }

    // Malformed request lines are fatal, not "wait for more": the connection
    // cannot recover, and returning 0 would spin forever on the same bytes.
    {
        Message request;
        expect(parseRequest(bytes("GARBAGE\r\n\r\n"), request) == std::nullopt,
               "a request line with no spaces is fatal");
        expect(parseRequest(bytes("GET /only-one-space\r\n\r\n"), request) == std::nullopt,
               "a request line with one space is fatal");
    }

    // A Content-Length that is not a number is fatal for the same reason.
    {
        Message request;
        expect(parseRequest(bytes("GET / RTSP/1.0\r\nContent-Length: abc\r\n\r\n"), request) ==
                   std::nullopt,
               "a non-numeric Content-Length is fatal");
        expect(parseRequest(bytes("GET / RTSP/1.0\r\nContent-Length: -1\r\n\r\n"), request) ==
                   std::nullopt,
               "a negative Content-Length is fatal");
        expect(parseRequest(bytes("GET / RTSP/1.0\r\nContent-Length: 99999999999999999999\r\n\r\n"),
                            request) == std::nullopt,
               "a Content-Length that overflows is fatal");
    }

    // An enormous but valid Content-Length must simply wait, not allocate.
    {
        Message request;
        expect(parseRequest(bytes("GET / RTSP/1.0\r\nContent-Length: 4000000000\r\n\r\n"),
                            request) == std::optional<size_t>(0),
               "an oversized Content-Length waits rather than failing or allocating");
    }

    // Header handling details the dispatch depends on.
    {
        const Bytes wire = bytes("GET / RTSP/1.0\r\n"
                                 "content-TYPE:   application/x-apple-binary-plist   \r\n"
                                 "Host: [fe80::1%en0]:7000\r\n"
                                 "Malformed line with no colon\r\n"
                                 "CSeq: 7\r\n"
                                 "\r\n");
        Message request;
        expect(parseRequest(wire, request).has_value(), "the request parses");

        expect(request.contentType() == "application/x-apple-binary-plist",
               "header lookup is case insensitive and the value is trimmed");
        const std::string* host = request.header("host");
        expect(host != nullptr && *host == "[fe80::1%en0]:7000",
               "a value containing colons is not split at the wrong one");
        expect(request.header("CSeq") != nullptr, "a malformed line does not stop later headers");
        expect(request.header("Nonexistent") == nullptr, "a missing header reads as absent");
        expect(request.headers.size() == 3, "the line with no colon is dropped");
    }

    // setHeader replaces in place rather than appending a duplicate -- the
    // response path sets CSeq and Server on messages the handlers built.
    {
        Message response = makeResponse(200, "OK", "text/plain", bytes("x"));
        response.setHeader("CSeq", "1");
        response.setHeader("cseq", "2");
        expect(response.headers.size() == 2, "setHeader replaces a case-insensitive match");
        const std::string* cseq = response.header("CSeq");
        expect(cseq != nullptr && *cseq == "2", "and keeps the new value");
    }

    // Responses.
    {
        const Message response = makeResponse(200, "OK", "application/octet-stream", bytes("body"));
        const std::string wire = text(serializeResponse(response));
        expect(wire.starts_with("RTSP/1.0 200 OK\r\n"), "status line");
        expect(wire.find("Content-Type: application/octet-stream\r\n") != std::string::npos,
               "content type");
        expect(wire.find("Content-Length: 4\r\n") != std::string::npos,
               "Content-Length is added from the body");
        expect(wire.ends_with("\r\n\r\nbody"), "headers end with a blank line, then the body");

        // Zero-length bodies still declare a length, or the phone cannot frame.
        const std::string empty = text(serializeResponse(makeResponse(200, "OK", "", {})));
        expect(empty.find("Content-Length: 0\r\n") != std::string::npos,
               "an empty body still declares Content-Length: 0");
        expect(empty.find("Content-Type") == std::string::npos,
               "and no content type is invented");

        // An explicit Content-Length is not doubled.
        Message manual = makeResponse(200, "OK", "", bytes("12345"));
        manual.setHeader("Content-Length", "5");
        const std::string once = text(serializeResponse(manual));
        expect(once.find("Content-Length") == once.rfind("Content-Length"),
               "an explicit Content-Length is not emitted twice");
    }

    if (failures == 0)
    {
        SPDLOG_INFO("rtsp tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
