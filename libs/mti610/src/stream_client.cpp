// SPDX-License-Identifier: GPL-3.0-or-later

#include "mti610/stream_client.h"

#include <spdlog/spdlog.h>

#include <array>
#include <utility>

namespace mti610
{
namespace
{

std::chrono::milliseconds backoffFor(const std::vector<std::chrono::milliseconds>& schedule,
                                     std::size_t attempt)
{
    if (schedule.empty())
    {
        return std::chrono::milliseconds(1000);
    }

    return schedule[std::min(attempt, schedule.size() - 1)];
}

// Sleep, but wake up promptly when asked to stop. A five-second backoff that
// could not be interrupted would make Ctrl-C take five seconds, which is long
// enough that people start using SIGKILL.
void interruptibleSleep(std::chrono::milliseconds total, const std::atomic<bool>& running)
{
    constexpr auto kSlice = std::chrono::milliseconds(50);
    auto remaining = total;

    while (remaining.count() > 0 && running.load())
    {
        const auto slice = std::min(kSlice, remaining);
        std::this_thread::sleep_for(slice);
        remaining -= slice;
    }
}

} // namespace

StreamClient::StreamClient(StreamFactory factory, Options options, DataHandler onData) :
    mFactory(std::move(factory)),
    mOptions(std::move(options)),
    mOnData(std::move(onData))
{
}

StreamClient::~StreamClient()
{
    stop();
}

void StreamClient::setByteTap(ByteTap tap)
{
    mByteTap = std::move(tap);
}

void StreamClient::start()
{
    if (mRunning.exchange(true))
    {
        return;
    }

    mThread = std::thread(&StreamClient::run, this);
}

void StreamClient::stop()
{
    // The store and the join are deliberately NOT guarded by each other. The
    // reader thread clears mRunning itself when the stream ends -- a replay
    // running out, or stopWhenStreamEnds on a closed port -- so an early
    // return when mRunning was already false would leave the thread joinable
    // and the destructor would call std::terminate. A pty test found this,
    // which is the argument for having one.
    mRunning.store(false);

    if (mThread.joinable())
    {
        mThread.join();
    }

    mMeasuring.store(false);
}

void StreamClient::requestReconfigure()
{
    mReconfigureWanted.store(true);
}

StreamClient::Stats StreamClient::stats() const
{
    const std::lock_guard<std::mutex> lock(mStateMutex);
    return mStats;
}

DeviceInfo StreamClient::deviceInfo() const
{
    const std::lock_guard<std::mutex> lock(mStateMutex);
    return mDeviceInfo;
}

std::vector<OutputEntry> StreamClient::effectiveOutputs() const
{
    const std::lock_guard<std::mutex> lock(mStateMutex);
    return mEffectiveOutputs;
}

std::vector<Change> StreamClient::lastChanges() const
{
    const std::lock_guard<std::mutex> lock(mStateMutex);
    return mLastChanges;
}

void StreamClient::run()
{
    std::size_t attempt = 0;

    while (mRunning.load())
    {
        Result<std::unique_ptr<ByteStream>> stream = mFactory();

        if (!stream)
        {
            {
                const std::lock_guard<std::mutex> lock(mStateMutex);
                mStats.lastError = to_string(stream.error());
            }

            if (mOptions.stopWhenStreamEnds)
            {
                SPDLOG_ERROR("mti610: {}", to_string(stream.error()));
                break;
            }

            const auto wait = backoffFor(mOptions.reopenBackoff, attempt);
            SPDLOG_WARN("mti610: {}; retrying in {} ms", to_string(stream.error()), wait.count());
            interruptibleSleep(wait, mRunning);
            ++attempt;
            continue;
        }

        attempt = 0;
        {
            const std::lock_guard<std::mutex> lock(mStateMutex);
            ++mStats.opens;
            mStats.lastError.clear();
        }

        serve(**stream);

        mMeasuring.store(false);

        if (!mRunning.load())
        {
            break;
        }

        if (mOptions.stopWhenStreamEnds)
        {
            SPDLOG_INFO("mti610: the stream ended");
            break;
        }

        const auto wait = backoffFor(mOptions.reopenBackoff, 0);
        SPDLOG_WARN("mti610: the port closed; reopening in {} ms", wait.count());
        interruptibleSleep(wait, mRunning);
    }

    mRunning.store(false);
}

bool StreamClient::handshake(DeviceSession& session)
{
    if (const Result<void> config = session.goToConfig(); !config)
    {
        // Any failure here ends this connection, including a timeout, and the
        // reopen loop retries from scratch.
        //
        // Carrying on and reading anyway was the first shape of this, and it
        // is wrong in the way that matters: a device that did not answer
        // GoToConfig has not been configured and is in a state we cannot name,
        // so publishing from it would be publishing whatever its stored
        // configuration happens to be while reporting success. Closing and
        // reopening also flushes a port full of data from a device that was
        // mid-measurement, which is the most likely reason the acknowledgement
        // was missed in the first place -- and the session's own retries have
        // already covered the case where it was simply late.
        SPDLOG_ERROR("mti610: cannot enter config state: {}", to_string(config.error()));
        const std::lock_guard<std::mutex> lock(mStateMutex);
        mStats.lastError = to_string(config.error());
        return false;
    }

    if (const Result<DeviceInfo> info = session.identify())
    {
        SPDLOG_INFO("mti610: {} id {:#x}, firmware {}, hardware {}.{}",
                    info->productCode.empty() ? "device" : info->productCode, info->deviceId,
                    info->firmwareVersion(), info->hardwareMajor, info->hardwareMinor);

        if (!info->productCode.empty() && !info->looksLikeMti610())
        {
            // Reported, not refused. A 620, 630 or 670 speaks the same
            // protocol; its extra outputs land on the raw topic and everything
            // this library does model still works. Refusing to talk to one
            // would be worse than saying so.
            SPDLOG_WARN("mti610: '{}' is not an MTi-610. The outputs this build models will "
                        "still decode; anything else lands on the raw topic.",
                        info->productCode);
        }

        const std::lock_guard<std::mutex> lock(mStateMutex);
        mDeviceInfo = *info;
    }
    else
    {
        SPDLOG_WARN("mti610: cannot identify the device: {}", to_string(info.error()));
    }

    {
        const std::lock_guard<std::mutex> lock(mStateMutex);
        ++mStats.configChecks;
    }

    if (const Result<ConfigureResult> configured = session.configure(mOptions.desiredOutputs))
    {
        const std::lock_guard<std::mutex> lock(mStateMutex);
        mLastChanges = configured->changes;
        mEffectiveOutputs = configured->effective;
        if (configured->wrote)
        {
            ++mStats.configWrites;
        }
    }
    else
    {
        SPDLOG_ERROR("mti610: cannot read or set the output configuration: {}",
                     to_string(configured.error()));
        const std::lock_guard<std::mutex> lock(mStateMutex);
        mStats.lastError = to_string(configured.error());
    }

    mConfigGeneration.fetch_add(1);

    if (const Result<void> measure = session.goToMeasurement(); !measure)
    {
        SPDLOG_ERROR("mti610: cannot enter measurement state: {}", to_string(measure.error()));
        const std::lock_guard<std::mutex> lock(mStateMutex);
        mStats.lastError = to_string(measure.error());
        return false;
    }

    mMeasuring.store(true);
    return true;
}

void StreamClient::serve(ByteStream& stream)
{
    // One framer for the whole connection, shared with the session: a reply
    // and a data message can arrive in the same read, and two framers would
    // each see half a stream.
    xbus::Framer framer;

    DeviceSession session(stream, framer, mOptions.session);
    session.onData([this](const xbus::MessageView& message) { handleMessage(message); });
    session.onWarning([](std::uint32_t code, const std::string& text) {
        SPDLOG_WARN("mti610: device warning {:#x}: {}", code, text);
    });

    if (!mOptions.readOnly)
    {
        if (!handshake(session))
        {
            return;
        }
    }
    else
    {
        mMeasuring.store(true);
    }

    std::array<std::uint8_t, 4096> buffer {};

    while (mRunning.load() && stream.isOpen())
    {
        if (mReconfigureWanted.exchange(false) && !mOptions.readOnly)
        {
            SPDLOG_INFO("mti610: re-checking the output configuration");
            mMeasuring.store(false);
            if (!handshake(session))
            {
                return;
            }
        }

        const ssize_t n = stream.recvSome(
            buffer, static_cast<unsigned>(mOptions.readTimeout.count()));

        if (n < 0)
        {
            return;
        }

        if (n == 0)
        {
            continue;
        }

        const std::span<const std::uint8_t> received(buffer.data(), static_cast<std::size_t>(n));

        if (mByteTap)
        {
            mByteTap(received);
        }

        {
            const std::lock_guard<std::mutex> lock(mStateMutex);
            mStats.bytesRead += static_cast<std::uint64_t>(n);
        }

        framer.push(received);

        while (const std::optional<xbus::MessageView> message = framer.next())
        {
            handleMessage(*message);
        }

        const std::lock_guard<std::mutex> lock(mStateMutex);
        mStats.framer = framer.stats();
    }
}

void StreamClient::handleMessage(const xbus::MessageView& message)
{
    if (message.is(xbus::MessageId::WakeUp))
    {
        // The device reset. Answer inside the 500 ms window so it waits in
        // Config rather than starting up with its stored configuration, then
        // ask for a full re-check -- because the stored configuration is
        // exactly what we spent the handshake replacing.
        SPDLOG_WARN("mti610: the device reset; re-running the handshake");
        {
            const std::lock_guard<std::mutex> lock(mStateMutex);
            ++mStats.deviceResets;
        }
        mMeasuring.store(false);
        mReconfigureWanted.store(true);
        return;
    }

    if (message.is(xbus::MessageId::ErrorReport))
    {
        if (const xbus::Result<xbus::ErrorReply> reply = xbus::ErrorReply::parse(message.data))
        {
            SPDLOG_ERROR("mti610: device error: {}", xbus::to_string(reply->deviceError()));
            const std::lock_guard<std::mutex> lock(mStateMutex);
            mStats.lastError = xbus::to_string(reply->deviceError());
        }
        return;
    }

    if (message.is(xbus::MessageId::WarningReport))
    {
        return;
    }

    if (!message.is(xbus::MessageId::MtData2))
    {
        return;
    }

    // Count what is in the body before handing it on, so the status message
    // can distinguish "this device sends something we do not model" from
    // "something we model does not parse". They look identical in a plot.
    {
        std::uint64_t items = 0;
        std::uint64_t unknown = 0;
        std::uint64_t malformed = 0;

        xbus::ItemIterator walk(message.data);
        while (const std::optional<xbus::DataItem> item = walk.next())
        {
            ++items;

            if (!xbus::is_known_data_id(item->rawId))
            {
                ++unknown;
                continue;
            }

            xbus::visit_item(*item, [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (!std::is_same_v<T, xbus::DataItem>)
                {
                    if (!value.has_value())
                    {
                        ++malformed;
                    }
                }
            });
        }

        const std::lock_guard<std::mutex> lock(mStateMutex);
        ++mStats.dataMessages;
        mStats.items += items;
        mStats.unknownItems += unknown;
        mStats.malformedItems += malformed;
        if (!walk.ok())
        {
            ++mStats.truncatedBodies;
        }
    }

    if (mOnData)
    {
        mOnData(message);
    }
}

} // namespace mti610
