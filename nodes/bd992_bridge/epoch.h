// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gathering the records that describe one instant.
//
// GSOF batches records into a transmission, and a receiver puts one epoch's
// records into one transmission. That grouping is knowable at
// bd992::StreamClient's transmission boundary and NOWHERE downstream -- which
// is the whole reason this class exists. See schemas/gsof_epoch.capnp for what
// goes wrong without it.
//
// Deliberately free of zenoh and capnp so the fusion RULE can be tested on its
// own: which records pair with which, what happens when one is missing, and
// what happens when a transmission is not shaped the way we assume. The
// translation to a schema is publishers.cpp's job.

#ifndef BD992_NODE_EPOCH_H
#define BD992_NODE_EPOCH_H

#include <cstdint>
#include <optional>
#include <type_traits>

#include "gsof/records.h"

namespace bd992_node
{

// One instant, as far as the receiver told us about it.
//
// `position` is not optional: an epoch without one is never produced, because
// there would be nothing for the other fields to be about.
struct FusedEpoch
{
    gsof::LatLongHeight position {};

    std::optional<gsof::PositionTime> time;
    std::optional<gsof::Velocity> velocity;
    std::optional<gsof::PositionType> fixType;
    std::optional<gsof::PositionSigma> sigma;
};

class EpochAccumulator
{
  public:
    // How the transmissions that went past were shaped. Persistent anomalies
    // here mean the receiver's output configuration is wrong, which this node
    // cannot fix and must therefore report.
    struct Counts
    {
        std::uint64_t transmissions { 0 };
        std::uint64_t epochs { 0 };
        // Transmissions with no position record. Legitimate in small numbers:
        // status records keep their own schedule.
        std::uint64_t withoutPosition { 0 };
        // A record of a kind already seen in THIS transmission. Means the
        // transmission carried more than one epoch, which cannot be fused
        // correctly -- see the first-wins note on add().
        std::uint64_t duplicates { 0 };
    };

    // Offer a decoded record. Anything that is not part of an epoch is
    // ignored, so this can be called for every record without a filter.
    //
    // FIRST OF EACH KIND WINS. A well-formed transmission carries at most one
    // of each, so this only matters when the assumption is violated -- and
    // then keeping the first at least pairs a position with the velocity that
    // followed it, where last-wins would pair the LAST position with an
    // earlier velocity and produce a heading from a different instant. Neither
    // is right; this one is defined, and `duplicates` makes it visible.
    template <typename RecordT>
    void add(const RecordT& record)
    {
        using Record = std::decay_t<RecordT>;

        if constexpr (std::is_same_v<Record, gsof::LatLongHeight>)
        {
            keep(mEpoch.position, mHasPosition, record);
        }
        else if constexpr (std::is_same_v<Record, gsof::PositionTime>)
        {
            keep(mEpoch.time, record);
        }
        else if constexpr (std::is_same_v<Record, gsof::Velocity>)
        {
            keep(mEpoch.velocity, record);
        }
        else if constexpr (std::is_same_v<Record, gsof::PositionType>)
        {
            keep(mEpoch.fixType, record);
        }
        else if constexpr (std::is_same_v<Record, gsof::PositionSigma>)
        {
            keep(mEpoch.sigma, record);
        }
    }

    // End of transmission. Returns the epoch when a position was among it, and
    // clears what was accumulated EITHER WAY -- a record must never survive
    // into the next transmission, because presenting a stale heading as
    // current is precisely the failure this class exists to prevent.
    std::optional<FusedEpoch> take()
    {
        ++mCounts.transmissions;

        std::optional<FusedEpoch> out;
        if (mHasPosition)
        {
            ++mCounts.epochs;
            out = mEpoch;
        }
        else
        {
            ++mCounts.withoutPosition;
        }

        mEpoch = FusedEpoch {};
        mHasPosition = false;
        return out;
    }

    const Counts& counts() const { return mCounts; }

  private:
    template <typename T>
    void keep(std::optional<T>& slot, const T& value)
    {
        if (slot.has_value())
        {
            ++mCounts.duplicates;
            return;
        }
        slot = value;
    }

    // The position is not held in an optional -- FusedEpoch promises it is
    // always there -- so its presence flag is separate.
    void keep(gsof::LatLongHeight& slot, bool& present, const gsof::LatLongHeight& value)
    {
        if (present)
        {
            ++mCounts.duplicates;
            return;
        }
        slot = value;
        present = true;
    }

    FusedEpoch mEpoch;
    bool mHasPosition { false };
    Counts mCounts;
};

} // namespace bd992_node

#endif // BD992_NODE_EPOCH_H
