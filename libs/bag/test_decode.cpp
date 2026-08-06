// SPDX-License-Identifier: GPL-3.0-or-later
//
// That a recorded message can actually be DECODED from the recording alone.
//
// Everything else in this library checks bytes: that what went in comes back
// unchanged, and that the container around them is structurally valid. Neither
// establishes the claim the format was chosen for -- that a bag is
// self-describing, so a consumer who does not link this build's generated capnp
// headers can still read it.
//
// That claim has three links, and a test that skips any of them proves nothing:
//
//   1. the writer stores a schema DESCRIPTOR (a serialized CodeGeneratorRequest,
//      pruned to the schema's transitive closure) in the recording;
//   2. a reader can get it back out;
//   3. capnp::SchemaLoader can load it and decode a recorded payload into the
//      same field values that went in.
//
// So this goes the long way round on purpose. It builds messages with the
// generated types (which is how a node publishes), then reads them back and
// decodes them through SchemaLoader + DynamicStruct -- deliberately NOT through
// capnp::Schema::from<EngineRpm>(), because that would just be asking the
// compiler what it already knows and would pass even if the recording contained
// no schema at all.

#include "bag/reader.h"
#include "bag/writer.h"

#include "engine_rpm.capnp.h"
#include "racegrade_tc8_configure.capnp.h"

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/schema-loader.h>
#include <capnp/schema.capnp.h>
#include <capnp/serialize.h>

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

class TempDir
{
  public:
    explicit TempDir(const std::string& label)
    {
        path_ = std::filesystem::temp_directory_path() /
                ("redline_bag_decode_" + label + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    std::string str() const { return path_.string(); }

  private:
    std::filesystem::path path_;
};

constexpr std::uint64_t kBase = 1'785'000'000'000'000'000ull;

// Serialises a capnp message the way ZenohPublisher does, so what lands in the
// bag is byte-identical to what a real node would have published.
std::vector<std::uint8_t> encodeRpm(std::uint64_t rpm, float oil_pressure)
{
    capnp::MallocMessageBuilder message;
    auto builder = message.initRoot<EngineRpm>();
    builder.setRpm(static_cast<std::uint32_t>(rpm));
    builder.setOilPressurePsi(oil_pressure);
    builder.setTimestamp(kBase / 1'000'000ull);

    const kj::Array<capnp::word> words = capnp::messageToFlatArray(message);
    const kj::ArrayPtr<const kj::byte> bytes = words.asBytes();
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

// Loads a descriptor into a SchemaLoader exactly as a foreign consumer would,
// and returns the struct schema for `display_name` (capnp's qualified form).
bool loadSchema(std::span<const std::uint8_t> descriptor, const std::string& display_name,
                capnp::SchemaLoader& loader, capnp::StructSchema& out)
{
    if (descriptor.empty() || descriptor.size() % sizeof(capnp::word) != 0)
    {
        return false;
    }

    // A std::uint8_t array has no word alignment guarantee, and capnp's readers
    // require it -- the same reason pub_sub::WordAlignedPayload exists. A real
    // consumer reading these bytes out of a file faces exactly this.
    kj::Array<capnp::word> words =
        kj::heapArray<capnp::word>(descriptor.size() / sizeof(capnp::word));
    std::memcpy(words.begin(), descriptor.data(), descriptor.size());

    capnp::ReaderOptions options;
    options.traversalLimitInWords = 1 << 30;
    capnp::FlatArrayMessageReader reader(words.asPtr(), options);
    auto request = reader.getRoot<capnp::schema::CodeGeneratorRequest>();

    std::uint64_t root_id = 0;
    for (auto node : request.getNodes())
    {
        loader.load(node);
        if (node.getDisplayName() == display_name.c_str())
        {
            root_id = node.getId();
        }
    }

    if (root_id == 0)
    {
        return false;
    }

    try
    {
        out = loader.get(root_id).asStruct();
    }
    catch (const kj::Exception&)
    {
        return false;
    }
    return true;
}

// Reads a field out of a DynamicStruct by name.
std::optional<std::uint64_t> uintField(capnp::DynamicStruct::Reader reader, const char* name)
{
    KJ_IF_MAYBE(field, reader.getSchema().findFieldByName(name))
    {
        const capnp::DynamicValue::Reader value = reader.get(*field);
        if (value.getType() == capnp::DynamicValue::UINT)
        {
            return value.as<std::uint64_t>();
        }
    }
    return std::nullopt;
}

std::optional<double> floatField(capnp::DynamicStruct::Reader reader, const char* name)
{
    KJ_IF_MAYBE(field, reader.getSchema().findFieldByName(name))
    {
        const capnp::DynamicValue::Reader value = reader.get(*field);
        if (value.getType() == capnp::DynamicValue::FLOAT)
        {
            return value.as<double>();
        }
    }
    return std::nullopt;
}

// ------------------------------------------------------------------ the cases

// THE claim: record real messages, then decode them using only what the
// recording carries.
void testMessagesDecodeFromTheRecordingAlone()
{
    const TempDir dir("decode");
    constexpr int kCount = 25;

    {
        bag::WriterOptions options;
        options.name = "d";
        bag::BagWriter writer(dir.str(), options);

        for (int i = 0; i < kCount; ++i)
        {
            writer.write("vehicle/engine/rpm", "EngineRpm",
                         encodeRpm(1000ull + static_cast<std::uint64_t>(i) * 100ull,
                                   40.0F + static_cast<float>(i)),
                         kBase + static_cast<std::uint64_t>(i) * 1'000'000ull, std::nullopt, "");
        }
        expect(writer.close(), "the recording closes");
    }

    bag::BagReader reader(dir.str());
    expect(reader.isValid(), "the recording opens");
    if (!reader.isValid())
    {
        return;
    }

    // Link 1 and 2: the descriptor is in the file and comes back out.
    const std::span<const std::uint8_t> descriptor = reader.descriptorFor("EngineRpm");
    expect(!descriptor.empty(),
           "the recording carries a schema descriptor for EngineRpm (" +
               std::to_string(descriptor.size()) + " bytes)");
    if (descriptor.empty())
    {
        return;
    }

    // Link 3: it loads standalone.
    capnp::SchemaLoader loader;
    capnp::StructSchema schema;
    expect(loadSchema(descriptor, "engine_rpm.capnp:EngineRpm", loader, schema),
           "the descriptor loads into a SchemaLoader and yields the struct");
    if (schema == capnp::StructSchema())
    {
        return;
    }

    // And decodes the payloads into the values that went in.
    int index = 0;
    bool all_correct = true;

    reader.forEach(
        [&](const bag::BagMessage& message)
        {
            kj::Array<capnp::word> words =
                kj::heapArray<capnp::word>(message.payload.size() / sizeof(capnp::word));
            std::memcpy(words.begin(), message.payload.data(), message.payload.size());

            capnp::FlatArrayMessageReader payload(words.asPtr());
            const capnp::DynamicStruct::Reader decoded = payload.getRoot<capnp::DynamicStruct>(schema);

            const auto rpm = uintField(decoded, "rpm");
            const auto oil = floatField(decoded, "oilPressurePsi");

            const std::uint64_t expected_rpm = 1000ull + static_cast<std::uint64_t>(index) * 100ull;
            const double expected_oil = 40.0 + static_cast<double>(index);

            if (!rpm || *rpm != expected_rpm)
            {
                std::fprintf(stderr, "  message %d: rpm decoded as %llu, expected %llu\n", index,
                             rpm ? static_cast<unsigned long long>(*rpm) : 0ull,
                             static_cast<unsigned long long>(expected_rpm));
                all_correct = false;
            }
            if (!oil || std::abs(*oil - expected_oil) > 0.001)
            {
                all_correct = false;
            }

            ++index;
            return true;
        });

    expect(index == kCount, "every message came back (" + std::to_string(index) + ")");
    expect(all_correct,
           "every message decodes to the values it was written with, using only the schema the "
           "recording carries");
}

// The descriptor has to be COMPLETE, not merely present. A schema whose field
// refers to an enum is the case that catches a writer storing the root node
// alone: the struct loads, the field appears, and resolving its type throws.
void testDescriptorCoversReferencedTypes()
{
    const TempDir dir("closure");

    {
        bag::WriterOptions options;
        options.name = "d";
        bag::BagWriter writer(dir.str(), options);

        // RaceGradeTc8ConfigureRequest has two enum-typed fields.
        capnp::MallocMessageBuilder message;
        auto builder = message.initRoot<RaceGradeTc8ConfigureRequest>();
        builder.setCanId(1234);

        const kj::Array<capnp::word> words = capnp::messageToFlatArray(message);
        const kj::ArrayPtr<const kj::byte> bytes = words.asBytes();

        writer.write("test/configure", "RaceGradeTc8ConfigureRequest",
                     std::vector<std::uint8_t>(bytes.begin(), bytes.end()), kBase, std::nullopt,
                     "");
        writer.close();
    }

    bag::BagReader reader(dir.str());
    const std::span<const std::uint8_t> descriptor =
        reader.descriptorFor("RaceGradeTc8ConfigureRequest");
    expect(!descriptor.empty(), "the recording carries the descriptor");
    if (descriptor.empty())
    {
        return;
    }

    capnp::SchemaLoader loader;
    capnp::StructSchema schema;
    expect(loadSchema(descriptor, "racegrade_tc8_configure.capnp:RaceGradeTc8ConfigureRequest",
                      loader, schema),
           "it loads");
    if (schema == capnp::StructSchema())
    {
        return;
    }

    std::size_t enums_resolved = 0;
    bool all_resolvable = true;

    for (auto field : schema.getFields())
    {
        const auto proto = field.getProto();
        if (!proto.isSlot() || !proto.getSlot().getType().isEnum())
        {
            continue;
        }
        try
        {
            // Throws if the enum's node was not stored alongside the struct's.
            const capnp::EnumSchema enum_schema =
                loader.get(proto.getSlot().getType().getEnum().getTypeId()).asEnum();
            expect(enum_schema.getEnumerants().size() > 0,
                   std::string("enum field '") + proto.getName().cStr() + "' has its enumerants");
            ++enums_resolved;
        }
        catch (const kj::Exception&)
        {
            std::fprintf(stderr, "  enum field '%s' refers to a type the recording did not store\n",
                         proto.getName().cStr());
            all_resolvable = false;
        }
    }

    expect(enums_resolved == 2, "both enum fields were found");
    expect(all_resolvable,
           "every type a recorded schema refers to is stored in the recording too -- a partial "
           "descriptor loads and then cannot decode the field");
}

// A message whose schema this build does not know is still recorded verbatim.
// The bytes are the irreplaceable part; the definition is not.
void testUnknownSchemaIsStillRecorded()
{
    const TempDir dir("unknown");

    const std::vector<std::uint8_t> payload(64, 0xAB);

    {
        bag::WriterOptions options;
        options.name = "d";
        bag::BagWriter writer(dir.str(), options);
        writer.write("test/future", "SchemaFromTheFuture", payload, kBase, std::nullopt, "");
        writer.close();
    }

    bag::BagReader reader(dir.str());
    expect(reader.isValid(), "the recording opens");

    std::size_t seen = 0;
    bool bytes_intact = true;
    reader.forEach(
        [&](const bag::BagMessage& message)
        {
            ++seen;
            if (message.payload.size() != payload.size() ||
                !std::equal(payload.begin(), payload.end(), message.payload.begin()))
            {
                bytes_intact = false;
            }
            if (message.schema != "SchemaFromTheFuture")
            {
                bytes_intact = false;
            }
            return true;
        });

    expect(seen == 1, "the message was recorded despite the unknown schema");
    expect(bytes_intact, "its bytes and schema name come back exactly");
    expect(reader.descriptorFor("SchemaFromTheFuture").empty(),
           "and no descriptor is invented for it");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    testMessagesDecodeFromTheRecordingAlone();
    testDescriptorCoversReferencedTypes();
    testUnknownSchemaIsStillRecorded();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
