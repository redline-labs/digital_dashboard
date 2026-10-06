// SPDX-License-Identifier: GPL-3.0-or-later
//
// SharedMfiSigner: a coprocessor that turns up late is used once it does, and
// its two consumers never reach it at the same time.
#include "iap2/shared_mfi_signer.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

// A chip that notices being driven from two threads at once: the I2C
// transactions of two signatures would interleave on the bus.
class FakeChip final : public iap2::MfiSigner
{
  public:
    explicit FakeChip(std::atomic<int>& overlaps) : overlaps_(overlaps) {}

    std::optional<std::vector<uint8_t>> certificate() override { return std::vector<uint8_t>{1, 2, 3}; }

    std::optional<std::vector<uint8_t>> signChallenge(const std::vector<uint8_t>& challenge) override
    {
        if (busy_.exchange(true))
        {
            ++overlaps_;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        busy_.store(false);
        return challenge;
    }

    int protocolMajor() override { return 2; }

  private:
    std::atomic<int>& overlaps_;
    std::atomic<bool> busy_{false};
};

}  // namespace

int main()
{
    using namespace std::chrono_literals;
    std::atomic<int> overlaps{0};
    std::atomic<bool> plugged{false};
    const auto chip = [&](std::atomic<int>& opens) {
        return [&]() -> std::unique_ptr<iap2::MfiSigner> {
            ++opens;
            return plugged.load() ? std::make_unique<FakeChip>(overlaps) : nullptr;
        };
    };

    // An interval no test run can outlast: whatever the host does, the second
    // call is inside it.
    {
        std::atomic<int> opens{0};
        iap2::SharedMfiSigner patient(chip(opens), 10s);
        expect(!patient.ready(), "no chip yet: not ready");
        expect(patient.protocolMajor() == 0 && !patient.signChallenge({1}).has_value(),
               "and nothing is signed");
        plugged.store(true);
        expect(!patient.ready(), "a retry is not attempted before the interval is up");
        expect(opens.load() == 1, "so the bus is opened once, not on every call");
        plugged.store(false);
    }

    // A short one, waited out: the bridge enumerating late.
    std::atomic<int> opens{0};
    iap2::SharedMfiSigner shared(chip(opens), 20ms);
    expect(!shared.ready(), "a chip that is not there yet is not ready");
    plugged.store(true);
    std::this_thread::sleep_for(30ms);  // a late wake-up only makes this longer
    expect(shared.ready(), "once it is up, the chip is used");
    expect(opens.load() == 2, "after exactly one more open");
    expect(shared.protocolMajor() == 2 && shared.certificate().has_value(),
           "and answers from then on");

    // Both consumers at once: iAP2 authentication and AirPlay /auth-setup.
    std::vector<std::thread> consumers;
    for (int i = 0; i < 4; ++i)
    {
        consumers.emplace_back([&] {
            for (int n = 0; n < 10; ++n)
            {
                shared.signChallenge({static_cast<uint8_t>(n)});
            }
        });
    }
    for (auto& consumer : consumers)
    {
        consumer.join();
    }
    expect(overlaps.load() == 0, "concurrent consumers never reach the chip at the same time");
    expect(opens.load() == 2, "and a working chip is opened once");

    if (failures == 0)
    {
        SPDLOG_INFO("all shared MFi signer checks passed");
    }
    return failures == 0 ? 0 : 1;
}
