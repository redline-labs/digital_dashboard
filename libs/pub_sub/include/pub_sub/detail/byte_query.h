#ifndef PUB_SUB_DETAIL_BYTE_QUERY_H_
#define PUB_SUB_DETAIL_BYTE_QUERY_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pub_sub::detail
{

// A request or a reply as it crosses the zenoh boundary: the bytes, and what
// the sender stamped on them.
struct ByteMessage
{
    std::vector<std::uint8_t> payload;
    // The half after the ';' of the encoding; "" when none was named.
    std::string schema_name;
    // The sender's schema revision, when it stamped one.
    std::optional<std::uint64_t> layout;
};

// The zenoh half of a service, with no zenoh in the header.
//
// Same reasoning as BytePublisher and ByteSubscriber: ZenohService is a template
// over two capnp schemas and so lives in a header, and <zenoh.hxx> is 89,000
// preprocessed lines that every translation unit offering a service paid for.
// What zenoh does here -- declare a queryable, advertise it, hand over request
// bytes, send reply bytes -- is not templated.
//
// Nothing outside pub_sub should name this type; it is the seam, not the API.
class ByteQueryable
{
  public:
    // A reply, or a reason it could not be given (sent as a zenoh error reply).
    struct Answer
    {
        std::vector<std::uint8_t> payload;
        std::string error;
    };

    // Runs on a zenoh thread, once per query. Must not throw; anything that
    // escapes is caught and answered as an error.
    using Handler = std::function<Answer(const ByteMessage& request)>;

    // Replies are stamped `response_schema` and `response_layout`, so a client
    // can check them exactly as a subscriber checks a sample.
    ByteQueryable(std::string keyexpr, std::string_view request_schema,
                  std::string_view response_schema, std::uint64_t response_layout,
                  Handler handler);

    // Undeclares the queryable, joining any query in flight.
    ~ByteQueryable();

    ByteQueryable(const ByteQueryable&) = delete;
    ByteQueryable& operator=(const ByteQueryable&) = delete;

    bool isValid() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Sends one query, stamped `request_schema` and `request_layout`.
//
// `on_reply` runs on a zenoh thread for every successful reply; `on_done` runs
// once, after the last reply or the timeout. Returns false, having called
// neither, when the query could not be sent at all.
bool queryBytes(const std::string& keyexpr, std::string_view request_schema,
                std::uint64_t request_layout, std::vector<std::uint8_t> request,
                std::uint64_t timeout_ms, std::function<void(const ByteMessage&)> on_reply,
                std::function<void()> on_done);

}  // namespace pub_sub::detail

#endif  // PUB_SUB_DETAIL_BYTE_QUERY_H_
