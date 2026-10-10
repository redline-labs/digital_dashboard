#ifndef PUB_SUB_SAMPLE_CHECK_H_
#define PUB_SUB_SAMPLE_CHECK_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pub_sub
{

// What a publisher stamped on a sample, judged against the schema a consumer is
// about to decode it as.
enum class SampleVerdict
{
    accept,        // Names the expected schema, at this build's revision.
    unnamed,       // Names no schema (an older build, or not ours): decoded as expected.
    wrong_schema,  // Names a different schema: decoding it yields plausible wrong values.
    wrong_layout,  // Same name, written against another revision of the schema.
};

// The decision alone, with no logging, so it can be pinned by a unit test.
//
// `published_name` is the half after the ';' of the encoding, exactly as
// schemaNameFromEncoding() gives it: "" when the publisher named none, and the
// whole string for an encoding that is not ours at all ("zenoh/bytes"), which
// is then a wrong schema rather than an unnamed one. An `expected_layout` of 0
// (kNoLayout, spelled out to keep capnp out of this header) means this build
// does not know the schema, which is not a mismatch; nor is an unstamped layout.
constexpr SampleVerdict judgeSample(std::string_view expected_name, std::uint64_t expected_layout,
                                    std::string_view published_name,
                                    std::optional<std::uint64_t> published_layout)
{
    if (published_name.empty())
    {
        return SampleVerdict::unnamed;
    }
    if (published_name != expected_name)
    {
        return SampleVerdict::wrong_schema;
    }
    if (published_layout && expected_layout != 0 && *published_layout != expected_layout)
    {
        return SampleVerdict::wrong_layout;
    }
    return SampleVerdict::accept;
}

// One per subscription: the schema it decodes as, the key it reads, and the
// once-only complaints about samples it had to refuse.
//
// Every typed consumer in the tree runs the same four checks before trusting a
// payload -- schema name, schema revision, whole capnp words, a decode that does
// not throw -- and they used to be written three times, each missing a
// different one. A sample of the wrong schema decodes silently into plausible
// numbers, so the only safe answer to any of them is to drop it and say so.
//
// Each refusal is reported once per gate: a stream at hundreds of hertz would
// otherwise fill the log with the same line. Safe to call from the zenoh thread;
// the latches are atomic.
//
// No capnp and no spdlog in this header. pub_sub/typed_decode.h adds the
// decode step for a schema known as a type.
class SampleGate
{
  public:
    SampleGate(std::string keyexpr, std::string_view schema_name, std::uint64_t layout);

    SampleGate(const SampleGate&) = delete;
    SampleGate& operator=(const SampleGate&) = delete;

    // True when a sample stamped like this may be decoded. Logs the first
    // refusal of each kind, and the first unnamed sample at debug level.
    bool admit(std::string_view published_name, std::optional<std::uint64_t> published_layout);

    // The revision alone, for a caller that judged the name on the first sample
    // and latched it: the name costs a string copy out of zenoh, the revision an
    // integer.
    bool admitRevision(std::optional<std::uint64_t> published_layout)
    {
        return admit(schema_name_, published_layout);
    }

    // The refusals a decoder finds after admit(): a payload that is not a whole
    // number of capnp words, and one capnp rejected while it was being read.
    void refusePartialWord(std::size_t bytes);
    void refuseMalformed(std::string_view why);

    const std::string& keyexpr() const { return keyexpr_; }
    const std::string& schemaName() const { return schema_name_; }

  private:
    std::string keyexpr_;
    std::string schema_name_;
    std::uint64_t layout_;

    std::atomic<bool> said_unnamed_{false};
    std::atomic<bool> said_wrong_schema_{false};
    std::atomic<bool> said_wrong_layout_{false};
    std::atomic<bool> said_partial_word_{false};
    std::atomic<bool> said_malformed_{false};
};

}  // namespace pub_sub

#endif  // PUB_SUB_SAMPLE_CHECK_H_
