// The RAUC client, against tools/rauc_stub -- the fake Installer on the session
// bus. The test starts its own stub, so ctest covers all three of its modes
// rather than only whichever one a developer happened to leave running.
//
// What this is really testing is the SIGNATURES. Every one of them fails at
// runtime rather than at compile time, and a board is the worst place to find
// out that GetSlotStatus is a(sa{sv}), that installed.timestamp is a 't', or
// that Mark returns two out-args.
//
// SKIPS rather than fails when there is no session bus or no stub binary: a
// machine without either is not a broken machine, and a green run that quietly
// exercised nothing is worse than a reported skip.

#include "rauc_client/rauc_client.h"

#include <spdlog/spdlog.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace
{

using namespace rauc_client;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// Owns the stub process for the life of the test. A well-known bus name cannot
// be shared, which is why the three registrations carry a RESOURCE_LOCK.
class Stub
{
public:
    explicit Stub(const std::string& mode)
    {
        pid_ = ::fork();
        if (pid_ == 0)
        {
            // Quiet in the child: its chatter is not this test's output. The
            // result is checked because freopen is warn_unused_result and this
            // tree builds tests with -Werror; if the redirect fails there is
            // nothing useful to do about it in a forked child, so it carries on
            // noisily rather than pretending.
            if (::freopen("/dev/null", "w", stderr) == nullptr)
            {
                // Deliberately empty: see above.
            }
            if (mode == "ok")
            {
                ::execl(RAUC_STUB, RAUC_STUB, static_cast<char*>(nullptr));
            }
            else
            {
                const std::string flag = "--" + mode;
                ::execl(RAUC_STUB, RAUC_STUB, flag.c_str(), static_cast<char*>(nullptr));
            }
            ::_exit(127);
        }
    }

    ~Stub()
    {
        if (pid_ > 0)
        {
            ::kill(pid_, SIGTERM);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
    }

    Stub(const Stub&) = delete;
    Stub& operator=(const Stub&) = delete;

    bool started() const { return pid_ > 0; }

private:
    ::pid_t pid_ { -1 };
};

// Before the stub owns the name: what the console sees on a board whose
// bus-activated rauc.service has not been started yet.
void testBeforeAnythingOwnsTheName()
{
    Installer installer(Installer::Bus::Session, {});
    std::string error;
    check(installer.connect(error), "a proxy is built for a name nobody owns (" + error + ")");
    if (installer.serviceAvailable())
    {
        SPDLOG_WARN("something already owns de.pengutronix.rauc; skipping the unowned checks");
        return;
    }
    // The console used to gate every call on an owner, which on the image
    // meant RAUC was never activated.
    check(installer.connected(), "connected() does not depend on an owner");
    check(installer.operation().empty(), "Operation is empty until RAUC answers");
    std::string slotsError;
    check(installer.slots(&slotsError).empty(), "no slots without RAUC");
    check(!slotsError.empty(), "and the call says why");
}

// g_main_loop_quit() before g_main_loop_run() is forgotten, so destroying an
// Installer straight after connect() could hang in the join. A watchdog turns
// that hang into a failure rather than a ctest timeout.
void testDestroyingRightAfterConnectDoesNotHang()
{
    std::atomic<bool> finished { false };
    std::thread watchdog([&finished] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!finished && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!finished)
        {
            std::fprintf(stderr, "FAIL: ~Installer hung after connect()\n");
            std::_Exit(1);
        }
    });

    int connected = 0;
    for (int i = 0; i < 200; ++i)
    {
        Installer installer(Installer::Bus::Session, {});
        std::string error;
        if (installer.connect(error)) { ++connected; }
    }
    finished = true;
    watchdog.join();
    check(connected == 200, "every connect succeeded (" + std::to_string(connected) + " of 200)");
}

}  // namespace

int main()
{
    if (std::getenv("DBUS_SESSION_BUS_ADDRESS") == nullptr)
    {
        SPDLOG_WARN("SKIP: no session bus");
        return PROJECT_TEST_SKIP_CODE;
    }
    if (!std::filesystem::exists(RAUC_STUB))
    {
        SPDLOG_WARN("SKIP: no stub at {} -- build the rauc_stub target", RAUC_STUB);
        return PROJECT_TEST_SKIP_CODE;
    }

    testBeforeAnythingOwnsTheName();
    testDestroyingRightAfterConnectDoesNotHang();

    const char* modeEnv = std::getenv("REDLINE_RAUC_STUB_MODE");
    const std::string mode = modeEnv != nullptr ? modeEnv : "ok";
    SPDLOG_INFO("[test] stub mode: {}", mode);

    Stub stub(mode);
    if (!stub.started())
    {
        SPDLOG_WARN("SKIP: could not fork the stub");
        return PROJECT_TEST_SKIP_CODE;
    }
    // The stub has to own the name before the proxy is built.
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    std::atomic<int> completions { 0 };
    std::atomic<int> progressEvents { 0 };
    std::atomic<int> lastResult { -1 };
    std::string lastError;

    Installer::Callbacks callbacks;
    callbacks.onProgress = [&](const Progress& progress) {
        ++progressEvents;
        SPDLOG_DEBUG("[test] progress {}% {}", progress.percentage, progress.message);
    };
    callbacks.onCompleted = [&](std::int32_t result, const std::string& error) {
        lastResult = result;
        lastError = error;
        ++completions;
    };

    Installer installer(Installer::Bus::Session, callbacks);

    std::string error;
    if (!installer.connect(error))
    {
        SPDLOG_WARN("SKIP: could not connect ({})", error);
        return PROJECT_TEST_SKIP_CODE;
    }
    if (!installer.serviceAvailable())
    {
        SPDLOG_WARN("SKIP: nothing owns de.pengutronix.rauc -- did the stub start?");
        return PROJECT_TEST_SKIP_CODE;
    }
    check(installer.connected(), "connected to the stub");

    // --- true of every mode ------------------------------------------------
    const auto status = installer.status();
    check(status.has_value(), "status reads");
    if (status)
    {
        check(status->compatible == "redline-lattepanda-mu", "Compatible is the board's string");
        check(status->bootSlot == "A", "BootSlot is read");
        check(status->primary == "rootfs.0", "GetPrimary returns the booted slot");
        // The one property that differs by mode, and the guard installBundle
        // consults before it calls anything.
        check(status->operation == (mode == "busy" ? "installing" : "idle"),
              "Operation reflects the stub's mode (got '" + status->operation + "')");
    }

    check(installer.operation() == (mode == "busy" ? "installing" : "idle"),
          "operation() reads the cached property");

    std::string slotsError;
    const std::vector<SlotStatus> slots = installer.slots(&slotsError);
    check(slotsError.empty(), "GetSlotStatus reports no error (got '" + slotsError + "')");
    check(slots.size() == 2, "both slots are reported");
    if (slots.size() == 2)
    {
        check(slots[0].name == "rootfs.0" && slots[0].bootname == "A", "slot names and bootnames");
        check(slots[0].state == "booted", "the booted slot says so");
        check(slots[1].state == "inactive", "the other slot is inactive");
        check(slots[0].bundleVersion == "1.0-20260917013010", "bundle.version is lifted out");
        check(slots[0].installedTimestamp.has_value(), "installed.timestamp is a 't', not an 's'");
    }

    // --- the install ---------------------------------------------------------
    // SEQUENCED DELIBERATELY. Writing this as
    //     check(installer.installBundle(path, error), "..." + error)
    // reads the message argument before the call in an unspecified order, so the
    // reason printed was whatever `error` held BEFORE the attempt -- which is to
    // say, empty. That cost a real debugging detour.
    const bool accepted = installer.installBundle("/data/updates/bundle.raucb", error);

    if (mode == "busy")
    {
        check(!accepted, "an install is refused while RAUC is busy");
        check(error.find("busy") != std::string::npos,
              "and the refusal says why (got '" + error + "')");
        // Refused locally from the cached Operation, so nothing was sent: the
        // stub never logs an InstallBundle in this mode.
        check(completions.load() == 0, "no install ran");
    }
    else
    {
        check(accepted, "InstallBundle is accepted (error: '" + error + "')");

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (completions.load() == 0 && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        check(completions.load() == 1, "Completed arrived exactly once");
        check(progressEvents.load() >= 3, "progress was reported as it went, not only at the end");

        if (mode == "fail")
        {
            check(lastResult.load() != 0, "a rejected bundle completes with a non-zero result");
            check(lastError.find("signature") != std::string::npos,
                  "and LastError says what went wrong (got '" + lastError + "')");
        }
        else
        {
            check(lastResult.load() == 0, "a good bundle completes with zero");
            check(lastError.empty(), "and leaves no error behind");
        }
    }

    // --- Mark, in every mode -------------------------------------------------
    std::string slotName;
    std::string message;
    const bool marked = installer.mark("good", "booted", slotName, message, error);
    check(marked, "Mark succeeds (error: '" + error + "')");
    check(slotName == "rootfs.0", "Mark's first out-arg is the slot name");
    check(!message.empty(), "Mark's second out-arg is the message");

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
