// SPDX-License-Identifier: GPL-3.0-or-later

#include "services.h"

#include <optional>

#include <spdlog/spdlog.h>

#include "bd992.capnp.h"
#include "pub_sub/zenoh_service.h"

namespace bd992_node
{

namespace
{

::Bd992ConfigMode toSchema(ConfigMode mode)
{
    switch (mode)
    {
        case ConfigMode::ReportOnly: return ::Bd992ConfigMode::REPORT_ONLY;
        case ConfigMode::Enforce:    return ::Bd992ConfigMode::ENFORCE;
    }

    return ::Bd992ConfigMode::UNKNOWN;
}

::Bd992ChangeKind toSchema(bd992::ChangeKind kind)
{
    switch (kind)
    {
        case bd992::ChangeKind::Missing:    return ::Bd992ChangeKind::MISSING;
        case bd992::ChangeKind::RateDrift:  return ::Bd992ChangeKind::RATE_DRIFT;
        case bd992::ChangeKind::Unexpected: return ::Bd992ChangeKind::UNEXPECTED;
    }

    return ::Bd992ChangeKind::UNKNOWN;
}

} // namespace

void fillOutputMessage(::Bd992OutputMessage::Builder out, const bd992::OutputMessage& in)
{
    out.setPortIndex(static_cast<std::uint8_t>(in.port));
    out.setOutputType(static_cast<std::uint8_t>(in.outputType));
    out.setOutputTypeName(gsof::appfile::to_string(in.outputType));
    out.setRate(static_cast<std::uint8_t>(in.rate));
    out.setRateName(gsof::appfile::to_string(in.rate));
    out.setOffsetSeconds(in.offsetSeconds);
    out.setIsGsof(in.isGsof);
    out.setGsofRecordType(in.gsofRecordType);
    out.setGsofRecordName(in.isGsof ? gsof::record_name(static_cast<gsof::RecordType>(in.gsofRecordType)) : "");
}

void fillChange(::Bd992ConfigChange::Builder out, const bd992::Change& in)
{
    out.setKind(toSchema(in.kind));
    out.setDescription(bd992::to_string(in));
    fillOutputMessage(out.initDesired(), in.desired);
    fillOutputMessage(out.initActual(), in.actual);
}

void fillChanges(::capnp::List<::Bd992ConfigChange>::Builder out, const std::vector<bd992::Change>& changes)
{
    for (std::size_t i = 0; i < changes.size(); ++i)
    {
        fillChange(out[static_cast<unsigned>(i)], changes[i]);
    }
}

ConfigPass run_config_pass(bd992::ControlClient& control, const NodeConfig& config, bool dryRun)
{
    ConfigPass pass;

    const bd992::Result<gsof::appfile::ApplicationFile> file =
        control.readApplicationFile(config.configuration.applicationFileIndex);

    if (!file.has_value())
    {
        pass.error = bd992::to_string(file.error());
        return pass;
    }

    pass.ok = true;

    const std::vector<bd992::OutputMessage> desired = desired_outputs(config.configuration);
    const auto port = static_cast<gsof::appfile::PortIndex>(config.configuration.portIndex);

    pass.changes = bd992::diff(file->view(), desired, port, config.configuration.portPolicy);

    if (pass.changes.empty())
    {
        // The common case, and the one worth keeping quiet: a healthy receiver
        // that nobody has touched.
        SPDLOG_DEBUG("bd992: receiver configuration matches");
        return pass;
    }

    for (const bd992::Change& change : pass.changes)
    {
        SPDLOG_WARN("bd992: {}", bd992::to_string(change));
    }

    if (dryRun || config.configuration.mode == ConfigMode::ReportOnly)
    {
        return pass;
    }

    const std::vector<std::uint8_t> records =
        bd992::plan_writes(pass.changes, config.configuration.portPolicy);

    if (records.empty())
    {
        // Everything that differed was an unexpected output under the additive
        // policy, which is reported but deliberately not acted on.
        return pass;
    }

    const bd992::Result<void> written = control.writeApplicationFile(records);
    if (!written.has_value())
    {
        pass.ok = false;
        pass.error = bd992::to_string(written.error());
        return pass;
    }

    pass.written = true;
    SPDLOG_INFO("bd992: corrected {} output(s) on the receiver", pass.changes.size());

    return pass;
}

// ============================================================================
// The queryables
// ============================================================================

struct Services::Impl
{
    Deps deps;

    std::optional<pub_sub::ZenohService<::Bd992GetOutputConfigRequest, ::Bd992GetOutputConfigResponse>> getOutput;
    std::optional<pub_sub::ZenohService<::Bd992SetOutputConfigRequest, ::Bd992SetOutputConfigResponse>> setOutput;
    std::optional<pub_sub::ZenohService<::Bd992ApplyConfigRequest, ::Bd992ApplyConfigResponse>> applyConfig;
    std::optional<pub_sub::ZenohService<::Bd992GetReceiverInfoRequest, ::Bd992GetReceiverInfoResponse>> receiverInfo;
    std::optional<pub_sub::ZenohService<::Bd992SendCommandRequest, ::Bd992SendCommandResponse>> sendCommand;
};

Services::Services(Deps deps) : mImpl(std::make_unique<Impl>())
{
    mImpl->deps = deps;

    const std::string prefix = deps.config->publish.topicPrefix;

    mImpl->getOutput.emplace(
        prefix + "/get_output_config",
        [impl = mImpl.get()](const ::Bd992GetOutputConfigRequest::Reader& request,
                             ::Bd992GetOutputConfigResponse::Builder& response) {
            const std::uint16_t index = request.getApplicationFileIndex() != 0
                                            ? request.getApplicationFileIndex()
                                            : impl->deps.config->configuration.applicationFileIndex;

            const bd992::Result<gsof::appfile::ApplicationFile> file =
                impl->deps.control->readApplicationFile(index);

            if (!file.has_value())
            {
                response.setOk(false);
                response.setError(bd992::to_string(file.error()));
                return;
            }

            response.setOk(true);
            response.setOtherRecordCount(static_cast<std::uint16_t>(file->otherRecordCount));
            response.setDeviceType(file->control.deviceType);

            auto outputs = response.initOutputs(static_cast<unsigned>(file->outputCount));
            for (std::size_t i = 0; i < file->outputCount; ++i)
            {
                fillOutputMessage(outputs[static_cast<unsigned>(i)], file->outputs[i]);
            }
        });

    mImpl->setOutput.emplace(
        prefix + "/set_output_config",
        [impl = mImpl.get()](const ::Bd992SetOutputConfigRequest::Reader& request,
                             ::Bd992SetOutputConfigResponse::Builder& response) {
            const NodeConfig& config = *impl->deps.config;

            // A service that could write while the node is in report-only mode
            // would make the mode a suggestion. It is not one.
            if (config.configuration.mode == ConfigMode::ReportOnly && !request.getDryRun())
            {
                response.setOk(false);
                response.setError("the node is in report_only mode; only dryRun requests are accepted");
                return;
            }

            const auto port = request.getPortIndex() != 0
                                  ? static_cast<gsof::appfile::PortIndex>(request.getPortIndex())
                                  : static_cast<gsof::appfile::PortIndex>(config.configuration.portIndex);

            bd992::PortPolicy policy = config.configuration.portPolicy;
            const std::string requested = request.getPortPolicy();
            if (requested == "additive")
            {
                policy = bd992::PortPolicy::Additive;
            }
            else if (requested == "exclusive")
            {
                policy = bd992::PortPolicy::Exclusive;
            }
            else if (!requested.empty())
            {
                response.setOk(false);
                response.setError("portPolicy must be 'additive', 'exclusive' or empty");
                return;
            }

            std::vector<bd992::OutputMessage> desired;
            for (const ::Bd992OutputMessage::Reader entry : request.getOutputs())
            {
                bd992::OutputMessage message {};
                message.port = static_cast<gsof::appfile::PortIndex>(entry.getPortIndex());
                message.outputType = static_cast<gsof::appfile::OutputType>(entry.getOutputType());
                message.rate = static_cast<gsof::appfile::Frequency>(entry.getRate());
                message.offsetSeconds = entry.getOffsetSeconds();
                message.isGsof = entry.getIsGsof();
                message.gsofRecordType = entry.getGsofRecordType();
                desired.push_back(message);
            }

            const bd992::Result<gsof::appfile::ApplicationFile> file =
                impl->deps.control->readApplicationFile(config.configuration.applicationFileIndex);
            if (!file.has_value())
            {
                response.setOk(false);
                response.setError(bd992::to_string(file.error()));
                return;
            }

            const std::vector<bd992::Change> changes = bd992::diff(file->view(), desired, port, policy);

            response.setOk(true);
            fillChanges(response.initChanges(static_cast<unsigned>(changes.size())), changes);

            if (request.getDryRun() || changes.empty())
            {
                response.setWritten(false);
                return;
            }

            const std::vector<std::uint8_t> records = bd992::plan_writes(changes, policy);
            if (records.empty())
            {
                response.setWritten(false);
                return;
            }

            const bd992::Result<void> written = impl->deps.control->writeApplicationFile(records);
            if (!written.has_value())
            {
                response.setOk(false);
                response.setError(bd992::to_string(written.error()));
                return;
            }

            response.setWritten(true);
            impl->deps.outputsCorrected->fetch_add(changes.size());
        });

    mImpl->applyConfig.emplace(
        prefix + "/apply_config",
        [impl = mImpl.get()](const ::Bd992ApplyConfigRequest::Reader& request,
                             ::Bd992ApplyConfigResponse::Builder& response) {
            const ConfigPass pass =
                run_config_pass(*impl->deps.control, *impl->deps.config, request.getDryRun());

            response.setOk(pass.ok);
            response.setError(pass.error);
            response.setMode(toSchema(impl->deps.config->configuration.mode));
            response.setWritten(pass.written);
            fillChanges(response.initChanges(static_cast<unsigned>(pass.changes.size())), pass.changes);

            if (pass.written)
            {
                impl->deps.outputsCorrected->fetch_add(pass.changes.size());
            }
        });

    mImpl->receiverInfo.emplace(
        prefix + "/get_receiver_info",
        [impl = mImpl.get()](const ::Bd992GetReceiverInfoRequest::Reader& request,
                             ::Bd992GetReceiverInfoResponse::Builder& response) {
            // The serial number comes off the GSOF stream if record 15 is
            // enabled, so it is answered without a round trip when it can be.
            const std::optional<std::int32_t> serial = impl->deps.publishers->serialNumber();
            response.setSerialNumberKnown(serial.has_value());
            response.setSerialNumber(serial.value_or(0));
            response.setDeviceType(impl->deps.control->deviceType());

            const bd992::Result<bd992::ControlClient::Reply> options =
                impl->deps.control->readOptions(request.getOptionsPage());

            if (!options.has_value())
            {
                response.setOk(false);
                response.setError(bd992::to_string(options.error()));
                return;
            }

            response.setOk(true);
            response.setOptionsRaw(::capnp::Data::Reader(options->data.data(), options->data.size()));
        });

    mImpl->sendCommand.emplace(
        prefix + "/send_command",
        [impl = mImpl.get()](const ::Bd992SendCommandRequest::Reader& request,
                             ::Bd992SendCommandResponse::Builder& response) {
            const ::capnp::Data::Reader data = request.getData();

            const bd992::Result<bd992::ControlClient::Reply> reply = impl->deps.control->sendRaw(
                request.getPacketType(), std::span<const std::uint8_t>(data.begin(), data.size()));

            if (!reply.has_value())
            {
                response.setOk(false);
                response.setError(bd992::to_string(reply.error()));
                return;
            }

            response.setOk(true);
            response.setReplyStatus(reply->status);
            response.setReplyType(reply->type);
            response.setReplyData(::capnp::Data::Reader(reply->data.data(), reply->data.size()));
        });

    SPDLOG_INFO("bd992: services on {}/{{get_output_config,set_output_config,apply_config,"
                "get_receiver_info,send_command}}",
                prefix);
}

Services::~Services() = default;

} // namespace bd992_node
