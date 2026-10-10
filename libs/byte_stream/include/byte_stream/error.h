#ifndef BYTE_STREAM_ERROR_H_
#define BYTE_STREAM_ERROR_H_

#include <expected>
#include <string>

namespace byte_stream
{

// Why a stream could not be opened. Small on purpose: each device library maps
// this onto its own error type at the factory, where it knows what the stream
// was for.
struct Error
{
    enum class Kind
    {
        NotFound,       // no such host, no such file
        ConnectFailed,  // the host is there and did not accept
        Io,             // opened, then could not be used (an empty capture)
    };

    Kind kind{Kind::Io};
    std::string message;
    // errno, when there is one.
    int code{0};
};

template <typename T>
using Result = std::expected<T, Error>;

}  // namespace byte_stream

#endif  // BYTE_STREAM_ERROR_H_
