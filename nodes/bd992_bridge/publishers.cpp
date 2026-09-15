// SPDX-License-Identifier: GPL-3.0-or-later

#include "publishers.h"

#include <array>
#include <cmath>
#include <numbers>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "bd992.capnp.h"
#include "gsof_attitude.capnp.h"
#include "gsof_common.capnp.h"
#include "gsof_ins.capnp.h"
#include "gsof_integrity.capnp.h"
#include "gsof_position.capnp.h"
#include "gsof_satellites.capnp.h"
#include "gsof_status.capnp.h"
#include "pub_sub/zenoh_publisher.h"

#include "gsof_fields.h"

namespace bd992_node
{


// ============================================================================
// The publisher slots
// ============================================================================

struct Publishers::Impl
{
    std::string topicPrefix;
    bool publishUnknownRecords { false };

#define GSOF_PUB_SLOT(id, Name, snake) std::unique_ptr<pub_sub::ZenohPublisher<::Gsof##Name>> Name;
    GSOF_RECORD_TABLE(GSOF_PUB_SLOT)
#undef GSOF_PUB_SLOT

    std::unique_ptr<pub_sub::ZenohPublisher<::GsofRawRecord>> raw;

    struct Counter
    {
        std::string name;
        std::uint64_t count { 0 };
        std::chrono::steady_clock::time_point last {};
    };

    // Guards mCounters and mSerialNumber only. The reader thread writes them;
    // the main loop reads them to build the status message.
    mutable std::mutex mutex;
    std::unordered_map<std::uint8_t, Counter> counters;
    std::optional<std::int32_t> serialNumber;

    void note(std::uint8_t type, const char* name)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        Counter& counter = counters[type];
        if (counter.name.empty())
        {
            counter.name = name;
        }
        ++counter.count;
        counter.last = std::chrono::steady_clock::now();
    }
};

namespace
{

// Maps a record struct to its schema and its publisher slot.
template <typename T>
struct SchemaOf;

// SchemaOf maps a record struct to its schema and its publisher slot. `Gsof` +
// the table's name is the schema for every row, which is not a coincidence --
// the schemas were named to make this true.
#define GSOF_SCHEMA_TRAIT(id, Name, snake)                                             \
    template <>                                                                        \
    struct SchemaOf<gsof::Name>                                                         \
    {                                                                                  \
        using Schema = ::Gsof##Name;                                                   \
        static constexpr std::string_view topic = snake;                               \
        static std::unique_ptr<pub_sub::ZenohPublisher<Schema>>& slot(Publishers::Impl& impl) \
        {                                                                              \
            return impl.Name;                                                          \
        }                                                                              \
    };
GSOF_RECORD_TABLE(GSOF_SCHEMA_TRAIT)
#undef GSOF_SCHEMA_TRAIT

template <typename RecordT>
void publishOne(Publishers::Impl& impl, const RecordT& record)
{
    using Traits = SchemaOf<RecordT>;

    auto& slot = Traits::slot(impl);
    if (!slot)
    {
        // Created on first sight, so the topic's liveliness advertisement
        // names something the receiver is really sending.
        const std::string key = impl.topicPrefix + "/gsof/" + std::string(Traits::topic);
        slot = std::make_unique<pub_sub::ZenohPublisher<typename Traits::Schema>>(key);
        SPDLOG_INFO("bd992: publishing {} on {}", gsof::record_name(RecordT::kType), key);
    }

    fill(slot->fields(), record);
    slot->put();

    impl.note(static_cast<std::uint8_t>(RecordT::kType), gsof::record_name(RecordT::kType));
}

} // namespace

Publishers::Publishers(std::string topicPrefix, bool publishUnknownRecords) :
    mImpl(std::make_unique<Impl>())
{
    mImpl->topicPrefix = std::move(topicPrefix);
    mImpl->publishUnknownRecords = publishUnknownRecords;
}

Publishers::~Publishers() = default;

void Publishers::publish(const gsof::RawRecord& record)
{
    const gsof::Result<void> visited = gsof::visit_record(record, [this](const auto& parsed) {
        publishOne(*mImpl, parsed);

        // GSOF 15 is the only place the serial number appears on the stream,
        // and the receiver-info service would otherwise need a control round
        // trip to answer a question it has already been told the answer to.
        using Parsed = std::decay_t<decltype(parsed)>;
        if constexpr (std::is_same_v<Parsed, gsof::ReceiverSerial>)
        {
            const std::lock_guard<std::mutex> lock(mImpl->mutex);
            mImpl->serialNumber = parsed.serialNumber;
        }
    });

    if (visited.has_value())
    {
        return;
    }

    if (visited.error().kind != gsof::ErrorKind::UnknownRecord)
    {
        // A record we model whose body did not decode: a firmware change, or a
        // bug here. Not published, because a half-decoded position is worse
        // than none.
        SPDLOG_WARN("bd992: record {} did not decode: {}", record.type,
                    gsof::to_string(visited.error().kind));
        return;
    }

    if (!mImpl->publishUnknownRecords)
    {
        return;
    }

    if (!mImpl->raw)
    {
        const std::string key = mImpl->topicPrefix + "/gsof/raw";
        mImpl->raw = std::make_unique<pub_sub::ZenohPublisher<::GsofRawRecord>>(key);
        SPDLOG_INFO("bd992: publishing unmodelled records on {}", key);
    }

    ::GsofRawRecord::Builder out = mImpl->raw->fields();
    out.setRecordType(record.type);
    out.setBytes(::capnp::Data::Reader(record.body.data(), record.body.size()));
    mImpl->raw->put();

    mImpl->note(record.type, "raw");
}

std::vector<Publishers::Seen> Publishers::seen() const
{
    const auto now = std::chrono::steady_clock::now();

    const std::lock_guard<std::mutex> lock(mImpl->mutex);

    std::vector<Seen> out;
    out.reserve(mImpl->counters.size());

    for (const auto& [type, counter] : mImpl->counters)
    {
        out.push_back(Seen {
            type,
            counter.name,
            counter.count,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - counter.last).count()),
        });
    }

    return out;
}

std::optional<std::int32_t> Publishers::serialNumber() const
{
    const std::lock_guard<std::mutex> lock(mImpl->mutex);
    return mImpl->serialNumber;
}

} // namespace bd992_node
