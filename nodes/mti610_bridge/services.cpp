// SPDX-License-Identifier: GPL-3.0-or-later

#include "services.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <optional>
#include <thread>

#include "mti610.capnp.h"
#include "pub_sub/zenoh_service.h"
#include "xbus/data_id.h"

namespace mti610_node
{
namespace
{

using namespace std::chrono_literals;

const char* nameOf(std::uint16_t rawDataId)
{
    if (!xbus::is_known_data_id(rawDataId))
    {
        return "unknown";
    }
    return xbus::data_name(static_cast<xbus::DataId>(xbus::data_type(rawDataId)));
}

::Mti610ChangeKind toSchemaKind(mti610::ChangeKind kind)
{
    switch (kind)
    {
        case mti610::ChangeKind::Missing:    return ::Mti610ChangeKind::MISSING;
        case mti610::ChangeKind::RateDrift:  return ::Mti610ChangeKind::RATE_DRIFT;
        case mti610::ChangeKind::Unexpected: return ::Mti610ChangeKind::UNEXPECTED;
    }

    return ::Mti610ChangeKind::UNKNOWN;
}

// Wait for the reader thread to finish a configuration pass.
//
// Bounded, because the reader may be between reopen attempts with a device
// that is not there, and a service that blocked forever would take a zenoh
// thread with it.
bool waitForConfigPass(mti610::StreamClient& client, std::uint64_t before,
                       std::chrono::milliseconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (client.configGeneration() > before)
        {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

} // namespace

void fill_output_entry(auto builder, const mti610::OutputEntry& entry)
{
    builder.setRawDataId(entry.rawId);
    builder.setName(nameOf(entry.rawId));
    builder.setFrequencyHz(entry.frequencyHz);
}

void fill_change(auto builder, const mti610::Change& change)
{
    builder.setKind(toSchemaKind(change.kind));
    builder.setRawDataId(change.rawId);
    builder.setName(nameOf(change.rawId));
    builder.setActualHz(change.actualHz);
    builder.setDesiredHz(change.desiredHz);
}

// Explicit instantiations, so the two helpers above can live here and still be
// used from main.cpp's status message.
template void fill_output_entry<::Mti610OutputEntry::Builder>(::Mti610OutputEntry::Builder,
                                                              const mti610::OutputEntry&);
template void fill_change<::Mti610ConfigChange::Builder>(::Mti610ConfigChange::Builder,
                                                         const mti610::Change&);

struct Services::Impl
{
    mti610::StreamClient& client;
    mti610::ConfigMode mode;
    std::vector<mti610::OutputEntry> desired;

    std::optional<pub_sub::ZenohService<::Mti610GetOutputConfigRequest,
                                        ::Mti610GetOutputConfigResponse>>
        getOutputConfig;
    std::optional<pub_sub::ZenohService<::Mti610SetOutputConfigRequest,
                                        ::Mti610SetOutputConfigResponse>>
        setOutputConfig;
    std::optional<pub_sub::ZenohService<::Mti610ApplyConfigRequest, ::Mti610ApplyConfigResponse>>
        applyConfig;
    std::optional<pub_sub::ZenohService<::Mti610GetDeviceInfoRequest,
                                        ::Mti610GetDeviceInfoResponse>>
        getDeviceInfo;

    Impl(mti610::StreamClient& c, mti610::ConfigMode m, std::vector<mti610::OutputEntry> d) :
        client(c), mode(m), desired(std::move(d))
    {
    }
};

Services::Services(mti610::StreamClient& client, const std::string& topicPrefix,
                   mti610::ConfigMode mode, std::vector<mti610::OutputEntry> desired) :
    mImpl(std::make_unique<Impl>(client, mode, std::move(desired)))
{
    Impl* impl = mImpl.get();

    impl->getOutputConfig.emplace(
        topicPrefix + "/get_output_config",
        [impl](::Mti610GetOutputConfigRequest::Reader request,
               ::Mti610GetOutputConfigResponse::Builder response) {
            if (request.getRefresh())
            {
                // Costs a trip through Config state, which stops the data.
                const std::uint64_t before = impl->client.configGeneration();
                impl->client.requestReconfigure();

                if (!waitForConfigPass(impl->client, before, 5s))
                {
                    response.setOk(false);
                    response.setError("timed out waiting for the reader thread to re-read the "
                                      "configuration; the device may be disconnected");
                    return;
                }
            }

            const std::vector<mti610::OutputEntry> entries = impl->client.effectiveOutputs();
            const std::vector<mti610::Change> changes = impl->client.lastChanges();

            auto list = response.initEntries(static_cast<unsigned>(entries.size()));
            for (unsigned i = 0; i < entries.size(); ++i)
            {
                fill_output_entry(list[i], entries[i]);
            }

            auto changeList = response.initChanges(static_cast<unsigned>(changes.size()));
            for (unsigned i = 0; i < changes.size(); ++i)
            {
                fill_change(changeList[i], changes[i]);
            }

            response.setOk(true);
        });

    impl->setOutputConfig.emplace(
        topicPrefix + "/set_output_config",
        [impl](::Mti610SetOutputConfigRequest::Reader request,
               ::Mti610SetOutputConfigResponse::Builder response) {
            if (impl->mode == mti610::ConfigMode::ReportOnly)
            {
                // report_only exists for a device somebody else owns.
                // Honouring a write here would defeat the point of setting it.
                response.setOk(false);
                response.setError("the node is in report_only mode and will not write to the "
                                  "device");
                return;
            }

            std::vector<mti610::OutputEntry> entries;
            for (const ::Mti610OutputEntry::Reader entry : request.getEntries())
            {
                entries.push_back(
                    mti610::OutputEntry { entry.getRawDataId(), entry.getFrequencyHz() });
            }

            if (entries.size() > xbus::kMaxOutputEntries)
            {
                response.setOk(false);
                response.setError("at most " + std::to_string(xbus::kMaxOutputEntries) +
                                  " entries fit in one SetOutputConfiguration");
                return;
            }

            // Replaces the node's desired list, so a later periodic re-check
            // enforces what was just asked for rather than reverting to the
            // YAML. Anything else would make a service call last exactly until
            // the next recheck_interval_s, which is a surprise nobody wants to
            // debug.
            impl->desired = entries;

            const std::uint64_t before = impl->client.configGeneration();
            impl->client.requestReconfigure();

            if (!waitForConfigPass(impl->client, before, 5s))
            {
                response.setOk(false);
                response.setError("timed out waiting for the write to be applied");
                return;
            }

            const std::vector<mti610::OutputEntry> effective = impl->client.effectiveOutputs();
            auto list = response.initEffective(static_cast<unsigned>(effective.size()));
            for (unsigned i = 0; i < effective.size(); ++i)
            {
                fill_output_entry(list[i], effective[i]);
            }

            response.setOk(true);
        });

    impl->applyConfig.emplace(
        topicPrefix + "/apply_config",
        [impl](::Mti610ApplyConfigRequest::Reader, ::Mti610ApplyConfigResponse::Builder response) {
            const std::uint64_t before = impl->client.configGeneration();
            impl->client.requestReconfigure();

            if (!waitForConfigPass(impl->client, before, 5s))
            {
                response.setOk(false);
                response.setError("timed out waiting for the configuration pass");
                return;
            }

            const std::vector<mti610::Change> changes = impl->client.lastChanges();
            const std::vector<mti610::OutputEntry> effective = impl->client.effectiveOutputs();

            auto changeList = response.initChanges(static_cast<unsigned>(changes.size()));
            for (unsigned i = 0; i < changes.size(); ++i)
            {
                fill_change(changeList[i], changes[i]);
            }

            auto list = response.initEffective(static_cast<unsigned>(effective.size()));
            for (unsigned i = 0; i < effective.size(); ++i)
            {
                fill_output_entry(list[i], effective[i]);
            }

            response.setWrote(impl->client.stats().configWrites > 0);
            response.setOk(true);
        });

    impl->getDeviceInfo.emplace(
        topicPrefix + "/get_device_info",
        [impl](::Mti610GetDeviceInfoRequest::Reader,
               ::Mti610GetDeviceInfoResponse::Builder response) {
            const mti610::DeviceInfo info = impl->client.deviceInfo();

            if (info.deviceId == 0 && info.productCode.empty())
            {
                response.setOk(false);
                response.setError("the device has not been identified yet");
                return;
            }

            response.setDeviceId(info.deviceId);
            response.setProductCode(info.productCode);
            response.setFirmwareVersion(info.firmwareVersion());
            response.setHardwareVersion(std::to_string(info.hardwareMajor) + "." +
                                        std::to_string(info.hardwareMinor));
            response.setIsMti610(info.looksLikeMti610());
            response.setOk(true);
        });

    SPDLOG_INFO("mti610: services on {}/{{get_output_config,set_output_config,apply_config,"
                "get_device_info}}",
                topicPrefix);
}

Services::~Services() = default;

} // namespace mti610_node
