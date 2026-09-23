// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turning the bridge's per-record GSOF topics into the estimator's epochs.
//
// The bridge fuses nothing (see nodes/bd992_bridge/publishers.h), and the
// rule this node follows is map_match's: pair by arrival age. What the
// estimator needs that map_match does not is the GPS TIME of each fix, to
// line it up against the IMU -- and neither position (GSOF 2) nor velocity
// (8) carries one. The time comes from GSOF 1, which the receiver sends in the
// same transmission, so it arrives microseconds from them.
//
// So: records arriving within `burst` of the first one form an epoch, and a
// record type arriving twice closes it early. The epoch's time is GSOF 1's,
// or failing that the dual-antenna record's own time of week (GSOF 27 carries
// one, with the week taken from the last GSOF 1 or 16). An epoch with no time
// is dropped and counted, never guessed at: an IMU aligned to a guessed time
// is aligned wrong.
//
// Slow records -- accuracy (12), the fix type (38) -- are used by age, the way
// map_match uses them: fresh enough, or absent. A receiver at 10 Hz position
// and 1 Hz accuracy is the normal configuration, and batch membership would
// leave nine epochs in ten without a sigma.
//
// Pure: no zenoh, no capnp. The node decodes into these structs, and so does
// the offline tool reading a bag, so the pairing is the same in both.

#ifndef STATE_ESTIMATOR_GNSS_ASSEMBLER_H
#define STATE_ESTIMATOR_GNSS_ASSEMBLER_H

#include "vehicle_estimator/measurements.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace state_estimator
{

struct GpsTimeRecord  // GSOF 1 or 16
{
    std::uint16_t week = 0;
    std::uint32_t timeOfWeekMs = 0;
};

struct LatLongHeightRecord  // GSOF 2
{
    double latitudeDeg = 0.0, longitudeDeg = 0.0, ellipsoidHeightM = 0.0;
};

struct SigmaRecord  // GSOF 12
{
    double sigmaEastM = 0.0, sigmaNorthM = 0.0, covarianceEastNorth = 0.0, sigmaUpM = 0.0;
};

struct VelocityRecord  // GSOF 8
{
    bool valid = false;
    double horizontalSpeedMps = 0.0, headingDeg = 0.0, verticalVelocityMps = 0.0;
};

struct AttitudeRecord  // GSOF 27
{
    std::uint32_t timeOfWeekMs = 0;
    bool pitchValid = false, yawValid = false;
    double pitchDeg = 0.0, yawDeg = 0.0;
    bool hasVariance = false;
    // Assumed rad^2: the record's angles are radians on the wire, and the ICD
    // is not in this tree to say otherwise. Clamped on use.
    double pitchVariance = 0.0, yawVariance = 0.0, pitchYawCovariance = 0.0;
};

struct AssemblerOptions
{
    double burst = 0.015;         // s; well under half the output period at 20 Hz
    double sigmaFreshFor = 2.5;   // s
    double fixFreshFor = 5.0;     // s
    // Used when no fresh GSOF 12 is at hand. A receiver not sending accuracy
    // at all still gets used, at the uncertainty of a poor autonomous fix.
    double defaultSigmaHorizontalM = 2.0;
    double defaultSigmaVerticalM = 4.0;
};

class GnssAssembler
{
  public:
    explicit GnssAssembler(AssemblerOptions options = {});

    // `arrival` is the host time the record arrived, in seconds; the clock is
    // the caller's, only differences matter.
    void addTime(const GpsTimeRecord& r, double arrival);
    // The week alone (GSOF 16): dates a dual-antenna record when no GSOF 1
    // has been seen.
    void addWeek(std::uint16_t week) { week_ = week; }
    void addPosition(const LatLongHeightRecord& r, double arrival);
    void addVelocity(const VelocityRecord& r, double arrival);
    void addAttitude(const AttitudeRecord& r, double arrival);
    void addSigma(const SigmaRecord& r, double arrival);
    void addFix(vehicle_estimator::FixQuality fix, double arrival);

    // Epochs completed by `now`: every burst that has been quiet for `burst`.
    std::vector<vehicle_estimator::GnssEpoch> poll(double now);

    struct Stats
    {
        std::uint64_t epochs = 0;
        std::uint64_t noTime = 0;      // bursts with nothing to date them
        std::uint64_t noWeek = 0;      // a GSOF 27 time of week before any week was known
    };
    const Stats& stats() const { return stats_; }

  private:
    struct Burst
    {
        double first = 0.0;
        std::optional<GpsTimeRecord> time;
        std::optional<LatLongHeightRecord> position;
        std::optional<VelocityRecord> velocity;
        std::optional<AttitudeRecord> attitude;
    };

    // Starts a new burst if `arrival` is past the current one's window, or
    // the current one already holds this kind of record.
    Burst& burstFor(double arrival, bool already_has);
    void close();

    AssemblerOptions options_;
    std::optional<Burst> open_;
    std::vector<vehicle_estimator::GnssEpoch> ready_;
    std::optional<std::uint16_t> week_;
    std::optional<std::pair<SigmaRecord, double>> sigma_;
    std::optional<std::pair<vehicle_estimator::FixQuality, double>> fix_;
    Stats stats_;
};

}  // namespace state_estimator

#endif  // STATE_ESTIMATOR_GNSS_ASSEMBLER_H
