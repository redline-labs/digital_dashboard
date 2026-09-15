// SPDX-License-Identifier: GPL-3.0-or-later

#include "publishers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <map>
#include <mutex>

#include "mti610.capnp.h"
#include "pub_sub/zenoh_publisher.h"
#include "xbus/mtdata2.h"
#include "xbus_common.capnp.h"
#include "xbus_environment.capnp.h"
#include "xbus_inertial.capnp.h"
#include "xbus_timestamp.capnp.h"

#include "xbus_fields.h"

namespace mti610_node
{
namespace
{

// The identifiers that get their own topic.
//
// This is XBUS_DATA_TABLE minus the five that are PACKET METADATA -- the
// packet counter, the two sample times, and the status byte and word. Those
// five are not topics: they ride on every message published here, as
// XbusSampleHeader. Splitting them out would break the association between a
// measurement and the instant it was taken, which is the whole reason the
// header exists.
//
// Columns are (base identifier, struct in xbus::, schema, topic suffix). The
// schema name is `Xbus` + the xbus:: struct name for every row, which is not a
// coincidence -- the schemas were named to make this true.
#define MTI610_TOPIC_TABLE(X)                                          \
    X(Temperature,    XbusTemperature,    "temperature")               \
    X(UtcTime,        XbusUtcTime,        "utc_time")                  \
    X(BaroPressure,   XbusBaroPressure,   "baro_pressure")             \
    X(DeltaV,         XbusDeltaV,         "delta_v")                   \
    X(Acceleration,   XbusAcceleration,   "acceleration")              \
    X(AccelerationHr, XbusAccelerationHr, "acceleration_hr")           \
    X(RateOfTurn,     XbusRateOfTurn,     "rate_of_turn")              \
    X(DeltaQ,         XbusDeltaQ,         "delta_q")                   \
    X(RateOfTurnHr,   XbusRateOfTurnHr,   "rate_of_turn_hr")           \
    X(MagneticField,  XbusMagneticField,  "magnetic_field")

// The packet metadata gathered from one MTData2, before any measurement in it
// is published. Collected in a first pass over the items, because the device
// is free to put the status word after the acceleration and a consumer should
// not have to care.
struct SampleHeader
{
    bool hasPacketCounter { false };
    std::uint16_t packetCounter { 0 };

    bool hasSampleTimeFine { false };
    std::uint32_t sampleTimeFineTicks { 0 };

    bool hasSampleTimeCoarse { false };
    std::uint32_t sampleTimeCoarseS { 0 };

    bool hasStatus { false };
    std::uint32_t statusWord { 0 };
};

::XbusPrecision toSchemaPrecision(xbus::Precision precision)
{
    switch (precision)
    {
        case xbus::Precision::Float32: return ::XbusPrecision::FLOAT32;
        case xbus::Precision::Fp1220:  return ::XbusPrecision::FP1220;
        case xbus::Precision::Fp1632:  return ::XbusPrecision::FP1632;
        case xbus::Precision::Float64: return ::XbusPrecision::FLOAT64;
    }

    return ::XbusPrecision::UNKNOWN;
}

void fillHeader(::XbusSampleHeader::Builder builder, const SampleHeader& header,
                xbus::Precision precision)
{
    builder.setHasPacketCounter(header.hasPacketCounter);
    builder.setPacketCounter(header.packetCounter);
    builder.setHasSampleTimeFine(header.hasSampleTimeFine);
    builder.setSampleTimeFineTicks(header.sampleTimeFineTicks);
    builder.setHasSampleTimeCoarse(header.hasSampleTimeCoarse);
    builder.setSampleTimeCoarseS(header.sampleTimeCoarseS);

    builder.setHasStatus(header.hasStatus);
    builder.setStatusWord(header.statusWord);
    builder.setPrecision(toSchemaPrecision(precision));

    if (!header.hasStatus)
    {
        // Leaving the decoded bits at false rather than decoding a status word
        // that was never sent. `hasStatus` is what a consumer checks; a clear
        // `clipping` on a packet with no status word would otherwise read as
        // "not clipping" rather than "not reported".
        return;
    }

    const xbus::StatusWord status { header.statusWord };
    builder.setSelfTestPassed(status.selfTestPassed());
    builder.setFilterValid(status.filterValid());
    builder.setClipping(status.clipping());
    builder.setClipAccelerationX(status.clipAccX());
    builder.setClipAccelerationY(status.clipAccY());
    builder.setClipAccelerationZ(status.clipAccZ());
    builder.setClipGyroscopeX(status.clipGyrX());
    builder.setClipGyroscopeY(status.clipGyrY());
    builder.setClipGyroscopeZ(status.clipGyrZ());
    builder.setClipMagnetometerX(status.clipMagX());
    builder.setClipMagnetometerY(status.clipMagY());
    builder.setClipMagnetometerZ(status.clipMagZ());
    builder.setSyncInMarker(status.syncInMarker());
    builder.setSyncOutMarker(status.syncOutMarker());
    builder.setNoRotationStatus(status.noRotationStatus());
}

// One fill() per measurement. The compiler picks by overload, so forgetting
// one is a compile error rather than a topic that silently never appears.
//
// UNIT CONVERSION HAPPENS HERE AND ONLY HERE. The library structs carry the
// wire's units; these carry the published ones. For an MTi-610 the two happen
// to agree everywhere -- the device already sends SI, and the magnetic field's
// arbitrary units cannot be converted to anything -- so every one of these is
// a straight copy. That is worth saying out loud, because the absence of a
// conversion here is a fact about the device rather than an oversight.

} // namespace

struct Publishers::Impl
{
    std::string topicPrefix;
    bool publishUnknownItems { false };

#define MTI610_PUB_SLOT(Name, Schema, snake) \
    std::unique_ptr<pub_sub::ZenohPublisher<::Schema>> Name;
    MTI610_TOPIC_TABLE(MTI610_PUB_SLOT)
#undef MTI610_PUB_SLOT

    std::unique_ptr<pub_sub::ZenohPublisher<::XbusRawItem>> raw;

    mutable std::mutex mutex;
    std::map<std::uint16_t, SeenItem> seenItems;

    void note(std::uint16_t rawDataId, const char* name)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        SeenItem& entry = seenItems[rawDataId];
        entry.rawDataId = rawDataId;
        if (entry.name.empty())
        {
            entry.name = name;
        }
        ++entry.count;
    }
};

namespace
{

// Maps an xbus:: struct to its schema and its publisher slot.
template <typename T>
struct SchemaOf;

#define MTI610_SCHEMA_TRAIT(Name, Schema, snake)                                          \
    template <>                                                                            \
    struct SchemaOf<xbus::Name>                                                            \
    {                                                                                      \
        using SchemaType = ::Schema;                                                       \
        static constexpr std::string_view topic = snake;                                   \
        static std::unique_ptr<pub_sub::ZenohPublisher<SchemaType>>& slot(                  \
            Publishers::Impl& impl)                                                        \
        {                                                                                  \
            return impl.Name;                                                              \
        }                                                                                  \
    };
MTI610_TOPIC_TABLE(MTI610_SCHEMA_TRAIT)
#undef MTI610_SCHEMA_TRAIT

template <typename T>
void publishOne(Publishers::Impl& impl, const T& value, const SampleHeader& header,
                const xbus::DataItem& item)
{
    using Traits = SchemaOf<T>;
    auto& slot = Traits::slot(impl);

    if (!slot)
    {
        // Created on first sight, so the topic's liveliness advertisement
        // names something the device is really sending.
        const std::string key =
            impl.topicPrefix + "/mtdata2/" + std::string(Traits::topic);
        slot = std::make_unique<pub_sub::ZenohPublisher<typename Traits::SchemaType>>(key);
        SPDLOG_INFO("mti610: publishing {} on {}", Traits::topic, key);
    }

    auto fields = slot->fields();
    fillHeader(fields.initHeader(), header, item.precision());
    fill(fields, value);
    slot->put();

    impl.note(item.rawId, xbus::data_name(T::kId));
}

} // namespace

Publishers::Publishers(std::string topicPrefix, bool publishUnknownItems) :
    mImpl(std::make_unique<Impl>())
{
    mImpl->topicPrefix = std::move(topicPrefix);
    mImpl->publishUnknownItems = publishUnknownItems;
}

Publishers::~Publishers() = default;

void Publishers::publish(const xbus::MessageView& message)
{
    // First pass: the packet metadata. A device is free to put the status word
    // after the acceleration, and a consumer should not have to care -- so
    // nothing is published until the whole body has been read for context.
    SampleHeader header;

    {
        xbus::ItemIterator walk(message.data);
        while (const std::optional<xbus::DataItem> item = walk.next())
        {
            if (item->is(xbus::DataId::PacketCounter))
            {
                if (const xbus::Result<xbus::PacketCounter> value =
                        xbus::PacketCounter::parse(*item))
                {
                    header.hasPacketCounter = true;
                    header.packetCounter = value->counter;
                }
            }
            else if (item->is(xbus::DataId::SampleTimeFine))
            {
                if (const xbus::Result<xbus::SampleTimeFine> value =
                        xbus::SampleTimeFine::parse(*item))
                {
                    header.hasSampleTimeFine = true;
                    header.sampleTimeFineTicks = value->ticks;
                }
            }
            else if (item->is(xbus::DataId::SampleTimeCoarse))
            {
                if (const xbus::Result<xbus::SampleTimeCoarse> value =
                        xbus::SampleTimeCoarse::parse(*item))
                {
                    header.hasSampleTimeCoarse = true;
                    header.sampleTimeCoarseS = value->seconds;
                }
            }
            else if (item->is(xbus::DataId::StatusWord))
            {
                if (const xbus::Result<xbus::StatusWord> value = xbus::StatusWord::parse(*item))
                {
                    header.hasStatus = true;
                    header.statusWord = value->status;
                }
            }
            else if (item->is(xbus::DataId::StatusByte))
            {
                // The short form: bits 0:7 of the status word. Only used when
                // the full word is absent, because a device configured for
                // both would otherwise have the byte overwrite the word's
                // upper bits with zeroes.
                if (!header.hasStatus)
                {
                    if (const xbus::Result<xbus::StatusByte> value =
                            xbus::StatusByte::parse(*item))
                    {
                        header.hasStatus = true;
                        header.statusWord = value->status;
                    }
                }
            }
        }
    }

    // Second pass: the measurements.
    xbus::ItemIterator walk(message.data);
    while (const std::optional<xbus::DataItem> item = walk.next())
    {
        const xbus::DataItem& current = *item;

        xbus::visit_item(current, [this, &header, &current](const auto& value) {
            using T = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<T, xbus::DataItem>)
            {
                if (!mImpl->publishUnknownItems)
                {
                    return;
                }

                if (!mImpl->raw)
                {
                    const std::string key = mImpl->topicPrefix + "/mtdata2/raw";
                    mImpl->raw = std::make_unique<pub_sub::ZenohPublisher<::XbusRawItem>>(key);
                    SPDLOG_INFO("mti610: publishing unmodelled items on {}", key);
                }

                auto fields = mImpl->raw->fields();
                fields.setRawDataId(value.rawId);
                fields.setGroupName(xbus::to_string(value.group()));

                auto payload = fields.initPayload(
                    static_cast<unsigned>(value.payload.size()));
                std::copy(value.payload.begin(), value.payload.end(), payload.begin());

                mImpl->raw->put();
                mImpl->note(value.rawId, "unknown");
                return;
            }
            else
            {
                using Value = typename T::value_type;

                if (!value.has_value())
                {
                    // An identifier this build claims to understand whose
                    // payload did not parse. Counted in the stream client's
                    // stats; not published, because a half-filled capnp
                    // message reads as zeroes rather than as an error.
                    return;
                }

                if constexpr (requires { SchemaOf<Value>::topic; })
                {
                    publishOne(*mImpl, *value, header, current);
                }
                else
                {
                    // Packet metadata: already folded into the header above.
                    mImpl->note(current.rawId, xbus::data_name(Value::kId));
                }
            }
        });
    }
}

std::vector<SeenItem> Publishers::seen() const
{
    const std::lock_guard<std::mutex> lock(mImpl->mutex);

    std::vector<SeenItem> out;
    out.reserve(mImpl->seenItems.size());
    for (const auto& [id, item] : mImpl->seenItems)
    {
        out.push_back(item);
    }
    return out;
}

} // namespace mti610_node
