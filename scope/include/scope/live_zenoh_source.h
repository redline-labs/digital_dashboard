#ifndef SCOPE_LIVE_ZENOH_SOURCE_H_
#define SCOPE_LIVE_ZENOH_SOURCE_H_

#include "scope/data_source.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace scope
{

// A DataSource over the live zenoh bus.
//
// One subscription per distinct key, however many signals are bound to it. Ten
// fields of one MoTeC topic on screen means one subscription and ten
// expressions evaluated per sample, not ten subscriptions each decoding the
// same message. That is what pub_sub::RawSubscriber and the standalone
// ExpressionEvaluator exist for.
//
// TIMESTAMPS. Samples are stamped with std::chrono::steady_clock on arrival,
// as seconds since this object was constructed. Neither alternative works:
//
//   - zenoh's own timestamping is off. SessionManager::buildConfig() never sets
//     timestamping.enabled, and detail::SampleMeta exposes only the encoding,
//     so there is nothing to read even if it were on.
//   - payload timestamps exist on exactly five schemas -- the mock and legacy
//     ones (EngineRpm, EngineTemperature, VehicleSpeed, VehicleOdometer,
//     VehicleWarnings). Every real telemetry schema has none: MotecM1 across
//     its thirty-odd structs, MotecPdm, Megasquirt, RacegradeTc8Signals,
//     MotecLtc. CanFrame has one, in microseconds on a backend-defined epoch
//     with 0 meaning "no timestamp".
//
// So a payload-timestamp fast path would be right for five schemas and wrong
// for the rest, and mixing two clocks on one axis is worse than one honest
// arrival clock. Stamping is the *source's* job precisely so that a recorded
// source can supply recorded times instead, without any panel knowing.
class LiveZenohSource : public DataSource
{
  public:
    LiveZenohSource();
    ~LiveZenohSource() override;

    LiveZenohSource(const LiveZenohSource&) = delete;
    LiveZenohSource& operator=(const LiveZenohSource&) = delete;

    SourceCaps caps() const override;
    std::vector<TopicInfo> topics() const override;
    std::uint64_t topicsRevision() const override;
    SignalHandle bind(const SignalKey& key, std::shared_ptr<SignalBuffer> into) override;
    void release(SignalHandle handle) override;
    double now() const override;

    // How many distinct zenoh keys are currently subscribed. The point of the
    // fan-out is that this stays well below the number of bound signals, and a
    // test that cannot see it cannot check that.
    std::size_t subscriptionCount() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace scope

#endif  // SCOPE_LIVE_ZENOH_SOURCE_H_
