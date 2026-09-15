#pragma once

// Replays a golden file produced by gen_golden.py through a generated database
// and reports every disagreement.
//
// The goldens are cantools' answers, so this is a differential test: a failure
// means our bit walk, sign extension, scaling, multiplex gating or encode
// rounding disagrees with an independent implementation of the same format.
// That is what makes it worth more than a hand-written expectation, which can
// only ever encode what the author already believed.
//
// Four things are checked for every decoded signal and every frame:
//   * the raw bits recovered from the decoded value equal cantools' raw bits.
//     This is the exact check, and the one that makes the value check allowed
//     to be approximate: a float32 value is not cantools' double, and fused
//     multiply-add changes its last bits between targets, but every raw step
//     must still come back.
//   * the decoded value is within a few representable steps of cantools';
//     exact for anything integer-typed.
//   * the decoded frame re-encodes to cantools' bytes.
//   * cantools' physical values, assigned into a fresh message, encode to the
//     same bytes. That is the path a node transmitting a value takes.

#include "helpers/hex.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace golden
{

struct Expectation
{
    std::string signalName;
    std::string valueText;
    bool hasRaw{false};
    uint64_t raw{};
};

enum class CaseKind
{
    Decode,         // bytes in, values out
    EncodePhysical, // values in, bytes out
};

struct Case
{
    CaseKind kind{CaseKind::Decode};
    uint32_t messageId{};
    std::string messageName;
    std::vector<uint8_t> payload; // the frame decoded, or the frame expected
    std::vector<Expectation> expectations;
    std::vector<uint8_t> expectedEncoding; // Decode only; empty when there was no encode line
    int line{};
};

inline bool parseHex(const std::string &text, std::vector<uint8_t> &out)
{
    auto bytes = helpers::fromHex(text);
    if (!bytes)
    {
        return false;
    }
    out = std::move(*bytes);
    return true;
}

inline std::vector<Case> load(const std::string &path, const char *text, std::string &errorOut)
{
    std::vector<Case> cases;

    std::istringstream in(text);

    uint32_t currentId = 0;
    std::string currentName;
    int lineNumber = 0;
    std::string line;

    const auto fail = [&](const std::string &what) {
        errorOut = path + ":" + std::to_string(lineNumber) + ": " + what;
        return std::vector<Case>{};
    };

    while (std::getline(in, line))
    {
        lineNumber += 1;
        if (line.empty() || (line[0] == '#'))
        {
            continue;
        }

        std::istringstream fields(line);

        // Signal values are the only indented lines: name, value, and on a
        // decode case the raw bits.
        if ((line[0] == ' ') || (line[0] == '\t'))
        {
            if (cases.empty())
            {
                return fail("value before any case");
            }

            Expectation expectation;
            std::string rawText;
            if (!(fields >> expectation.signalName >> expectation.valueText))
            {
                return fail("malformed value line");
            }
            if (fields >> rawText)
            {
                if (rawText.rfind("0x", 0) != 0)
                {
                    return fail("raw bits must be written as 0x...");
                }
                char *end = nullptr;
                errno = 0;
                expectation.raw = std::strtoull(rawText.c_str() + 2, &end, 16);
                if ((errno != 0) || (end == rawText.c_str() + 2) || (*end != '\0'))
                {
                    return fail("malformed raw bits '" + rawText + "'");
                }
                expectation.hasRaw = true;
            }
            cases.back().expectations.push_back(std::move(expectation));
            continue;
        }

        std::string keyword;
        fields >> keyword;

        if (keyword == "database")
        {
            continue;
        }

        if (keyword == "message")
        {
            std::string idText;
            uint32_t dlc = 0;
            if (!(fields >> idText >> currentName >> dlc))
            {
                return fail("malformed message line");
            }
            currentId = static_cast<uint32_t>(std::strtoul(idText.c_str(), nullptr, 16));
            continue;
        }

        if ((keyword == "decode") || (keyword == "encode_physical"))
        {
            std::string payloadHex;
            fields >> payloadHex;

            Case entry;
            entry.kind = (keyword == "decode") ? CaseKind::Decode : CaseKind::EncodePhysical;
            entry.messageId = currentId;
            entry.messageName = currentName;
            entry.line = lineNumber;
            if (!parseHex(payloadHex, entry.payload))
            {
                return fail("malformed payload");
            }
            cases.push_back(std::move(entry));
            continue;
        }

        if (keyword == "encode")
        {
            if (cases.empty() || (cases.back().kind != CaseKind::Decode))
            {
                return fail("encode without a decode case before it");
            }

            std::string payloadHex;
            fields >> payloadHex;
            if (!parseHex(payloadHex, cases.back().expectedEncoding))
            {
                return fail("malformed encoding");
            }
            continue;
        }

        return fail("unknown keyword '" + keyword + "'");
    }

    return cases;
}

// A golden value, parsed straight into the type the generated field has.
// Integers never go through a double, so a 64 bit value stays exact.
template <typename T>
bool parseInto(const std::string &text, T &out)
{
    const char *begin = text.c_str();
    char *end = nullptr;
    errno = 0;

    if constexpr (std::is_enum_v<T>)
    {
        std::underlying_type_t<T> underlying{};
        if (!parseInto(text, underlying))
        {
            return false;
        }
        out = static_cast<T>(underlying);
        return true;
    }
    else if constexpr (std::is_same_v<T, bool>)
    {
        const long long value = std::strtoll(begin, &end, 10);
        if ((errno != 0) || (end == begin) || (*end != '\0') || ((value != 0) && (value != 1)))
        {
            return false;
        }
        out = (value != 0);
        return true;
    }
    else if constexpr (std::is_floating_point_v<T>)
    {
        const double value = std::strtod(begin, &end);
        if ((end == begin) || (*end != '\0'))
        {
            return false;
        }
        out = static_cast<T>(value);
        return true;
    }
    else if constexpr (std::is_signed_v<T>)
    {
        const long long value = std::strtoll(begin, &end, 10);
        if ((errno != 0) || (end == begin) || (*end != '\0') ||
            (value < static_cast<long long>(std::numeric_limits<T>::min())) ||
            (value > static_cast<long long>(std::numeric_limits<T>::max())))
        {
            return false;
        }
        out = static_cast<T>(value);
        return true;
    }
    else
    {
        if (text.rfind('-', 0) == 0)
        {
            return false;
        }
        const unsigned long long value = std::strtoull(begin, &end, 10);
        if ((errno != 0) || (end == begin) || (*end != '\0') ||
            (value > static_cast<unsigned long long>(std::numeric_limits<T>::max())))
        {
            return false;
        }
        out = static_cast<T>(value);
        return true;
    }
}

template <typename T>
std::string describe(const T &value)
{
    char buffer[64];
    if constexpr (std::is_enum_v<T>)
    {
        return describe(static_cast<std::underlying_type_t<T>>(value));
    }
    else if constexpr (std::is_same_v<T, bool>)
    {
        return value ? "1" : "0";
    }
    else if constexpr (std::is_floating_point_v<T>)
    {
        std::snprintf(buffer, sizeof(buffer), "%.17g", static_cast<double>(value));
    }
    else if constexpr (std::is_signed_v<T>)
    {
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    }
    else
    {
        std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    }
    return buffer;
}

// Whether a decoded value agrees with cantools'.
//
// Integer-typed fields must match exactly. A floating-point field only has to
// land within a few of its own representable steps of the largest magnitude the
// signal can produce: the raw check alongside is what proves it decoded the
// right step, and demanding more would fail a float32 field for being a float32.
template <typename Sig, typename T>
bool valueMatches(const T &actual, const std::string &expectedText)
{
    if constexpr (std::is_floating_point_v<T>)
    {
        char *end = nullptr;
        const double expected = std::strtod(expectedText.c_str(), &end);
        if ((end == expectedText.c_str()) || (*end != '\0'))
        {
            return false;
        }
        if (std::isinf(expected))
        {
            return static_cast<double>(actual) == expected;
        }

        const double maxAbsRaw =
            Sig::is_signed ? std::ldexp(1.0, static_cast<int>(Sig::length) - 1)
                           : (std::ldexp(1.0, static_cast<int>(Sig::length)) - 1.0);
        const double linear = std::abs(static_cast<double>(Sig::scale)) * maxAbsRaw +
                              std::abs(static_cast<double>(Sig::offset));
        const double magnitude = std::max(linear, std::abs(expected));
        const double steps = (sizeof(T) == sizeof(float)) ? 0x1p-21 : 0x1p-50;
        return std::abs(static_cast<double>(actual) - expected) <= (magnitude * steps);
    }
    else
    {
        T expected{};
        return parseInto(expectedText, expected) && (actual == expected);
    }
}

// The raw bits a decoded value maps back to. One place, so the day the
// generator's helper is renamed only this changes.
template <typename Message, typename Sig>
uint64_t recoverRaw(const typename Sig::Type &value)
{
    return static_cast<uint64_t>(Message::template to_raw_u<Sig>(value));
}

struct Counters
{
    size_t values{};
    size_t raws{};
    size_t encodings{};
    size_t physicalEncodings{};
};

// Assigns every expectation into a default-constructed message and encodes it.
template <typename Message>
bool encodeFromValues(const std::string &goldenPath, const Case &entry,
                      std::vector<uint8_t> &out, int &failures)
{
    Message fresh{};
    bool ok = true;

    for (const Expectation &expectation : entry.expectations)
    {
        bool found = false;
        fresh.visit([&](auto &field, auto tag) {
            using Sig = decltype(tag);
            if (Sig::name != expectation.signalName)
            {
                return;
            }
            found = true;
            if (!parseInto(expectation.valueText, field))
            {
                std::fprintf(stderr, "%s:%d: %s.%s: cannot hold cantools' value %s\n",
                             goldenPath.c_str(), entry.line, entry.messageName.c_str(),
                             expectation.signalName.c_str(), expectation.valueText.c_str());
                failures += 1;
                ok = false;
            }
        });
        if (!found)
        {
            std::fprintf(stderr, "%s:%d: %s has no signal named %s\n", goldenPath.c_str(),
                         entry.line, entry.messageName.c_str(), expectation.signalName.c_str());
            failures += 1;
            ok = false;
        }
    }

    const auto encoded = fresh.encode();
    out.assign(encoded.begin(), encoded.end());
    return ok;
}

// Runs every case and returns the number of failures, printing each one.
template <typename Db>
int replay(const std::string &goldenPath, const char *goldenText)
{
    std::string error;
    const std::vector<Case> cases = load(goldenPath, goldenText, error);
    if (!error.empty())
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    if (cases.empty())
    {
        std::fprintf(stderr, "%s: no cases, which cannot be right\n", goldenPath.c_str());
        return 1;
    }

    Db db;
    int failures = 0;
    Counters counted;

    for (const Case &entry : cases)
    {
        // Routing only. For a physical encode the payload is the expected frame,
        // and decoding it is just the way to reach the right message type.
        const auto decoded = db.decode(entry.messageId, entry.payload);
        if (decoded == Db::Messages::Unknown)
        {
            std::fprintf(stderr, "%s:%d: %s (0x%X) did not decode\n", goldenPath.c_str(),
                         entry.line, entry.messageName.c_str(), entry.messageId);
            failures += 1;
            continue;
        }

        db.visit_message(decoded, [&](auto &message) {
            using Message = std::remove_cvref_t<decltype(message)>;

            if (entry.kind == CaseKind::EncodePhysical)
            {
                std::vector<uint8_t> actual;
                if (encodeFromValues<Message>(goldenPath, entry, actual, failures))
                {
                    counted.physicalEncodings += 1;
                    if (actual != entry.payload)
                    {
                        std::fprintf(stderr,
                                     "%s:%d: %s from physical values encoded to %s, cantools says %s\n",
                                     goldenPath.c_str(), entry.line, entry.messageName.c_str(),
                                     helpers::toHex(actual).c_str(),
                                     helpers::toHex(entry.payload).c_str());
                        failures += 1;
                    }
                }
                return;
            }

            for (const Expectation &expectation : entry.expectations)
            {
                bool found = false;

                message.visit([&](const auto &value, auto tag) {
                    using Sig = decltype(tag);
                    if (Sig::name != expectation.signalName)
                    {
                        return;
                    }
                    found = true;

                    counted.values += 1;
                    if (!valueMatches<Sig>(value, expectation.valueText))
                    {
                        std::fprintf(stderr,
                                     "%s:%d: %s.%s from %s: got %s, cantools says %s\n",
                                     goldenPath.c_str(), entry.line, entry.messageName.c_str(),
                                     expectation.signalName.c_str(),
                                     helpers::toHex(entry.payload).c_str(),
                                     describe(value).c_str(), expectation.valueText.c_str());
                        failures += 1;
                    }

                    if (expectation.hasRaw)
                    {
                        counted.raws += 1;
                        const uint64_t raw = recoverRaw<Message, Sig>(value);
                        if (raw != expectation.raw)
                        {
                            std::fprintf(stderr,
                                         "%s:%d: %s.%s from %s: value maps back to raw 0x%llx, "
                                         "cantools read 0x%llx\n",
                                         goldenPath.c_str(), entry.line,
                                         entry.messageName.c_str(),
                                         expectation.signalName.c_str(),
                                         helpers::toHex(entry.payload).c_str(),
                                         static_cast<unsigned long long>(raw),
                                         static_cast<unsigned long long>(expectation.raw));
                            failures += 1;
                        }
                    }
                });

                if (!found)
                {
                    std::fprintf(stderr, "%s:%d: %s has no signal named %s\n", goldenPath.c_str(),
                                 entry.line, entry.messageName.c_str(),
                                 expectation.signalName.c_str());
                    failures += 1;
                }
            }

            if (entry.expectedEncoding.empty())
            {
                return;
            }

            const auto encoded = message.encode();
            const std::vector<uint8_t> reencoded(encoded.begin(), encoded.end());
            counted.encodings += 1;
            if (reencoded != entry.expectedEncoding)
            {
                std::fprintf(stderr, "%s:%d: %s re-encoded to %s, cantools says %s\n",
                             goldenPath.c_str(), entry.line, entry.messageName.c_str(),
                             helpers::toHex(reencoded).c_str(),
                             helpers::toHex(entry.expectedEncoding).c_str());
                failures += 1;
            }

            std::vector<uint8_t> fromValues;
            if (encodeFromValues<Message>(goldenPath, entry, fromValues, failures))
            {
                counted.physicalEncodings += 1;
                if (fromValues != entry.expectedEncoding)
                {
                    std::fprintf(stderr,
                                 "%s:%d: %s from cantools' decoded values encoded to %s, "
                                 "cantools says %s\n",
                                 goldenPath.c_str(), entry.line, entry.messageName.c_str(),
                                 helpers::toHex(fromValues).c_str(),
                                 helpers::toHex(entry.expectedEncoding).c_str());
                    failures += 1;
                }
            }
        });
    }

    std::printf("%s: %zu cases, %zu values, %zu raw fields, %zu re-encodes, "
                "%zu encodes from values, %d failures\n",
                goldenPath.c_str(), cases.size(), counted.values, counted.raws,
                counted.encodings, counted.physicalEncodings, failures);

    return failures;
}

} // namespace golden
