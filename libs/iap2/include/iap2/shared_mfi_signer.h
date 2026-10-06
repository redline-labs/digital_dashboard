// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef IAP2_SHARED_MFI_SIGNER_H_
#define IAP2_SHARED_MFI_SIGNER_H_

#include "iap2/mfi_signer.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace iap2
{

// The one coprocessor, for both of its consumers, from whenever it turns up.
//
// iAP2 authentication and AirPlay /auth-setup both sign with the same chip on
// one I2C bus, from different threads. Every call here holds one lock, so the
// two never interleave transactions -- the iAP2 side used to reach the chip
// without the lock the AirPlay side took. And the chip is opened on demand: a
// USB-I2C bridge that enumerates after the node starts, at a cold boot, used
// to leave AirPlay without MFi for the life of the process, because the open
// was tried once. A failed open is tried again, at most once per `retry_every`.
class SharedMfiSigner final : public MfiSigner
{
  public:
    // Returns an opened, probed signer, or null if the chip is not there yet.
    using Factory = std::function<std::unique_ptr<MfiSigner>()>;

    SharedMfiSigner(Factory factory, std::chrono::milliseconds retry_every);

    // Whether the chip is open, opening it if a retry is due.
    bool ready();

    std::optional<std::vector<uint8_t>> certificate() override;
    std::optional<std::vector<uint8_t>> signChallenge(const std::vector<uint8_t>& challenge) override;
    // 0 while the chip is not open.
    int protocolMajor() override;

  private:
    // Requires mutex_.
    MfiSigner* openLocked();

    Factory factory_;
    const std::chrono::milliseconds retry_every_;
    std::mutex mutex_;
    std::unique_ptr<MfiSigner> signer_;
    std::optional<std::chrono::steady_clock::time_point> last_attempt_;
};

}  // namespace iap2

#endif  // IAP2_SHARED_MFI_SIGNER_H_
