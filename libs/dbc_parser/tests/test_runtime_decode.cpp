// The run-time decoder against the same cantools goldens as the generated code.
//
// The goldens are a generated sweep over byte order, signedness, length,
// alignment, scaling, multiplexing and IEEE types, so holding a second
// implementation to them is as strong as holding the first: every raw field
// must come back bit for bit, every value must agree, and exactly the signals
// cantools decoded -- no more, which is what pins the multiplex gating.
//
// Then what the goldens cannot show: a payload shorter than the message.

#include "golden_data.h"
#include "golden_replay.h"

#include "dbc_parser/dbc_parser.h"
#include "dbc_parser/decode.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace
{

int g_failures = 0;
int g_reported = 0;

void check(bool condition, const std::string &what)
{
    if (!condition)
    {
        ++g_failures;
        // The first few, then a count: a broken bit walk fails thousands.
        if (++g_reported <= 20)
        {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        }
    }
}

std::optional<dbc_parser::Database> loadDbc(const std::string &name)
{
    std::ifstream in(std::string(DBC_TEST_DIR) + "/" + name + ".dbc");
    std::stringstream text;
    text << in.rdbuf();
    const std::string contents = text.str();
    dbc_parser::Parser parser(contents);
    return parser.parse();
}

const dbc_parser::Message *byId(const dbc_parser::Database &database, uint32_t id)
{
    for (const auto &message : database.messages)
    {
        if (message.id == id)
        {
            return &message;
        }
    }
    return nullptr;
}

void replay(const std::string &name, const char *golden)
{
    const auto database = loadDbc(name);
    check(database.has_value(), name + ": the DBC parses");
    if (!database)
    {
        return;
    }

    std::string error;
    const auto cases = golden::load(name, golden, error);
    check(error.empty() && !cases.empty(), name + ": the goldens load " + error);

    int decoded = 0;
    for (const auto &entry : cases)
    {
        if (entry.kind != golden::CaseKind::Decode)
        {
            continue;
        }
        const std::string where = name + ":" + std::to_string(entry.line) + " " + entry.messageName;
        const dbc_parser::Message *message = byId(*database, entry.messageId);
        check(message != nullptr, where + ": the message is in the DBC");
        if (message == nullptr)
        {
            continue;
        }

        const auto values = dbc_parser::decodeMessage(*message, entry.payload);
        std::set<std::string> expected;
        for (const auto &expectation : entry.expectations)
        {
            expected.insert(expectation.signalName);
            const std::string what = where + " " + expectation.signalName;

            const auto found = values.find(expectation.signalName);
            check(found != values.end(), what + ": decoded");
            if (found == values.end())
            {
                continue;
            }

            const dbc_parser::Signal *signal = nullptr;
            for (const auto &candidate : message->signals)
            {
                if (candidate.name == expectation.signalName)
                {
                    signal = &candidate;
                }
            }
            if (expectation.hasRaw && signal != nullptr)
            {
                const auto raw = dbc_parser::rawBits(*signal, entry.payload);
                check(raw.has_value() && *raw == expectation.raw,
                      what + ": raw bits " + (raw ? std::to_string(*raw) : "none") + " vs " +
                          std::to_string(expectation.raw));
            }

            const double theirs = std::strtod(expectation.valueText.c_str(), nullptr);
            const double tolerance = 1e-12 * std::max(1.0, std::fabs(theirs));
            check(std::fabs(found->second - theirs) <= tolerance || (std::isnan(theirs) && std::isnan(found->second)),
                  what + ": " + std::to_string(found->second) + " vs " + expectation.valueText);
            ++decoded;
        }
        for (const auto &[signal, value] : values)
        {
            check(expected.contains(signal), where + ": " + signal + " decoded but cantools did not (multiplexing)");
        }
    }
    check(decoded > 100, name + ": the sweep actually ran");
}

void shortPayloads()
{
    const std::string dbc = R"(VERSION ""
BU_: A
BO_ 256 Short: 8 A
 SG_ Whole : 0|8@1+ (1,0) [0|255] "" A
 SG_ Across : 4|16@1+ (1,0) [0|65535] "" A
 SG_ Motorola : 23|16@0+ (1,0) [0|65535] "" A
BO_ 257 Mux: 8 A
 SG_ Sel M : 0|8@1+ (1,0) [0|255] "" A
 SG_ InOne m1 : 8|8@1+ (1,0) [0|255] "" A
 SG_ InTwo m2 : 8|8@1+ (1,0) [0|255] "" A
)";
    dbc_parser::Parser parser(dbc);
    const auto database = parser.parse();
    check(database.has_value(), "the inline DBC parses");
    if (!database)
    {
        return;
    }

    const auto *message = dbc_parser::findMessage(*database, 256, false);
    check(message != nullptr, "findMessage by id");
    if (message != nullptr)
    {
        const std::vector<uint8_t> two{0x12, 0x34};
        const auto values = dbc_parser::decodeMessage(*message, two);
        check(values.contains("Whole") && values.at("Whole") == 0x12, "a signal inside a short payload decodes");
        check(!values.contains("Across"), "one reaching past a short payload is left out, not zero-padded");
        check(!values.contains("Motorola"), "so is a Motorola one");
        check(dbc_parser::decodeMessage(*message, std::span<const uint8_t>{}).empty(), "an empty payload decodes nothing");
    }

    const auto *mux = dbc_parser::findMessage(*database, 257, false);
    if (mux != nullptr)
    {
        const std::vector<uint8_t> three{3, 9};
        const auto values = dbc_parser::decodeMessage(*mux, three);
        check(values.contains("Sel") && !values.contains("InOne") && !values.contains("InTwo"),
              "a multiplexor value no group claims decodes only the multiplexor");
        const std::vector<uint8_t> two{2, 9};
        check(dbc_parser::decodeMessage(*mux, two).contains("InTwo"), "and the group it does select decodes");
        check(dbc_parser::decodeMessage(*mux, std::span<const uint8_t>{}).empty(),
              "an unreadable multiplexor decodes no multiplexed signal");
    }
    check(dbc_parser::findMessageWithSignal(*database, "InTwo") == mux, "findMessageWithSignal");
    check(dbc_parser::findMessage(*database, 256, true) == nullptr, "an extended id is a different message");
}

} // namespace

int main()
{
    replay("dbc_test_layout", golden_data::k_dbc_test_layout);
    replay("dbc_test_scaling", golden_data::k_dbc_test_scaling);
    replay("dbc_test_features", golden_data::k_dbc_test_features);
    replay("dbc_test_precision", golden_data::k_dbc_test_precision);
    shortPayloads();

    std::printf("dbc_parser_test_runtime_decode: %d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
