#pragma once

// Replays a golden file produced by gen_golden.py through a generated database
// and reports every disagreement.
//
// The goldens are cantools' answers, so this is a differential test: a failure
// means our bit walk, sign extension, scaling, multiplex gating or encode
// rounding disagrees with an independent implementation of the same format.
// That is what makes it worth more than a hand-written expectation, which can
// only ever encode what the author already believed.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
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
    double value{};
};

struct Case
{
    uint32_t messageId{};
    std::string messageName;
    std::vector<uint8_t> payload;
    std::vector<Expectation> expectations;
    std::vector<uint8_t> expectedEncoding; // empty when the golden had no encode line
    int line{};
};

inline std::optional<std::vector<uint8_t>> parseHex(std::string_view text)
{
    if ((text.size() % 2) != 0)
    {
        return std::nullopt;
    }

    std::vector<uint8_t> out;
    out.reserve(text.size() / 2);

    for (size_t i = 0; i < text.size(); i += 2)
    {
        unsigned value = 0;
        for (size_t j = 0; j < 2; ++j)
        {
            const char c = text[i + j];
            value <<= 4u;
            if ((c >= '0') && (c <= '9'))
            {
                value |= static_cast<unsigned>(c - '0');
            }
            else if ((c >= 'a') && (c <= 'f'))
            {
                value |= static_cast<unsigned>(c - 'a' + 10);
            }
            else if ((c >= 'A') && (c <= 'F'))
            {
                value |= static_cast<unsigned>(c - 'A' + 10);
            }
            else
            {
                return std::nullopt;
            }
        }
        out.push_back(static_cast<uint8_t>(value));
    }

    return out;
}

inline std::vector<Case> load(const std::string &path, const char *text, std::string &errorOut)
{
    std::vector<Case> cases;

    std::istringstream in(text);

    uint32_t currentId = 0;
    std::string currentName;
    int lineNumber = 0;
    std::string line;

    while (std::getline(in, line))
    {
        lineNumber += 1;
        if (line.empty() || (line[0] == '#'))
        {
            continue;
        }

        std::istringstream fields(line);
        std::string keyword;

        // Signal expectations are the only indented lines.
        if ((line[0] == ' ') || (line[0] == '\t'))
        {
            if (cases.empty())
            {
                errorOut = path + ":" + std::to_string(lineNumber) +
                           ": value before any decode line";
                return {};
            }

            Expectation expectation;
            if (!(fields >> expectation.signalName >> expectation.value))
            {
                errorOut = path + ":" + std::to_string(lineNumber) + ": malformed value line";
                return {};
            }
            cases.back().expectations.push_back(std::move(expectation));
            continue;
        }

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
                errorOut = path + ":" + std::to_string(lineNumber) + ": malformed message line";
                return {};
            }
            currentId = static_cast<uint32_t>(std::strtoul(idText.c_str(), nullptr, 16));
            continue;
        }

        if (keyword == "decode")
        {
            std::string payloadHex;
            fields >> payloadHex;
            auto payload = parseHex(payloadHex);
            if (!payload)
            {
                errorOut = path + ":" + std::to_string(lineNumber) + ": malformed payload";
                return {};
            }

            Case entry;
            entry.messageId = currentId;
            entry.messageName = currentName;
            entry.payload = std::move(*payload);
            entry.line = lineNumber;
            cases.push_back(std::move(entry));
            continue;
        }

        if (keyword == "encode")
        {
            if (cases.empty())
            {
                errorOut = path + ":" + std::to_string(lineNumber) +
                           ": encode before any decode line";
                return {};
            }

            std::string payloadHex;
            fields >> payloadHex;
            auto payload = parseHex(payloadHex);
            if (!payload)
            {
                errorOut = path + ":" + std::to_string(lineNumber) + ": malformed encoding";
                return {};
            }
            cases.back().expectedEncoding = std::move(*payload);
            continue;
        }

        errorOut = path + ":" + std::to_string(lineNumber) + ": unknown keyword '" + keyword + "'";
        return {};
    }

    return cases;
}

inline std::string hexOf(const std::vector<uint8_t> &bytes)
{
    std::string out;
    for (uint8_t byte : bytes)
    {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", byte);
        out += buf;
    }
    return out;
}

// cantools and the generated code both compute raw * scale + offset in IEEE
// doubles, so agreement should be near exact. The tolerance is only here to
// absorb the last bit or two of a decimal round trip through the golden file --
// any real decoding error moves a value by far more than this.
inline bool closeEnough(double lhs, double rhs)
{
    const double magnitude = (std::abs(rhs) > 1.0) ? std::abs(rhs) : 1.0;
    return std::abs(lhs - rhs) <= (1e-9 * magnitude);
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
    size_t checkedSignals = 0;
    size_t checkedEncodings = 0;

    for (const Case &entry : cases)
    {
        const auto decoded = db.decode(entry.messageId, entry.payload);
        if (decoded == Db::Messages::Unknown)
        {
            std::fprintf(stderr, "%s:%d: %s (0x%X) did not decode\n", goldenPath.c_str(),
                         entry.line, entry.messageName.c_str(), entry.messageId);
            failures += 1;
            continue;
        }

        db.visit_message(decoded, [&](auto &message) {
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
                    double actual = 0.0;
                    if constexpr (std::is_enum_v<std::decay_t<decltype(value)>>)
                    {
                        actual = static_cast<double>(static_cast<int64_t>(value));
                    }
                    else
                    {
                        actual = static_cast<double>(value);
                    }

                    checkedSignals += 1;
                    if (!closeEnough(actual, expectation.value))
                    {
                        std::fprintf(stderr,
                                     "%s:%d: %s.%s from %s: got %.17g, cantools says %.17g\n",
                                     goldenPath.c_str(), entry.line, entry.messageName.c_str(),
                                     expectation.signalName.c_str(), hexOf(entry.payload).c_str(),
                                     actual, expectation.value);
                        failures += 1;
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

            if (!entry.expectedEncoding.empty())
            {
                const auto encoded = message.encode();
                const std::vector<uint8_t> actual(encoded.begin(), encoded.end());
                checkedEncodings += 1;
                if (actual != entry.expectedEncoding)
                {
                    std::fprintf(stderr, "%s:%d: %s re-encoded to %s, cantools says %s\n",
                                 goldenPath.c_str(), entry.line, entry.messageName.c_str(),
                                 hexOf(actual).c_str(), hexOf(entry.expectedEncoding).c_str());
                    failures += 1;
                }
            }
        });
    }

    std::printf("%s: %zu cases, %zu signal values, %zu encodings, %d failures\n",
                goldenPath.c_str(), cases.size(), checkedSignals, checkedEncodings, failures);

    return failures;
}

} // namespace golden
