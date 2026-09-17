#ifndef RAUC_CLIENT_RAUC_CLIENT_H_
#define RAUC_CLIENT_RAUC_CLIENT_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Driving RAUC over D-Bus, so the web console can reflash the board.
//
// WHY A PROXY BY NAME RATHER THAN GENERATED CODE. The interface XML ships in
// rauc-dev, which is not on the image, so there is nothing to run gdbus-codegen
// against at build time on the target. The proxy is created with info == NULL
// and methods are called by name with GVariant. The signatures are pinned here
// instead, from de.pengutronix.rauc.Installer.xml:
//
//   InstallBundle(s source, a{sv} args)
//   GetSlotStatus() -> a(sa{sv})
//   Mark(s state, s slot_identifier) -> (s slot_name, s message)
//   GetPrimary() -> s
//   properties: Operation s, Progress (isi), LastError s,
//               Compatible s, Variant s, BootSlot s
//   signal:     Completed(i result)
//
// A wrong signature here fails at RUNTIME ON THE BOARD, not at compile time, so
// they are written down rather than remembered.
//
// THREADING. GDBus delivers property changes and signals to the GMainContext
// that was thread-default WHEN THE PROXY WAS CREATED. This class therefore owns
// a thread, pushes its own context, and creates the proxy there; callbacks are
// marshalled out through the caller's std::function. Creating the proxy on the
// main thread and then running a loop elsewhere is the bug this arrangement
// exists to avoid -- it looks like it works and delivers nothing.
namespace rauc_client
{

// RAUC's own words, not ours: "idle", "installing". Anything other than idle
// means an install is already running and a second one must be refused.
struct Status
{
    std::string operation;
    std::string lastError;
    std::string compatible;
    std::string variant;
    std::string bootSlot;
    std::string primary;
};

// One entry of GetSlotStatus()'s a(sa{sv}). The dictionary is open-ended, so
// only the keys the console shows are lifted out; the rest are ignored rather
// than guessed at.
struct SlotStatus
{
    std::string name;         // "rootfs.0"
    std::string device;
    std::string bootname;     // "A" / "B"
    std::string state;        // "booted", "inactive"
    std::string bootStatus;   // "good", "bad"
    std::string status;       // "ok"
    std::string bundleVersion;
    std::optional<std::uint64_t> installedTimestamp;
};

// The (isi) of the Progress property: percentage, message, nesting depth.
struct Progress
{
    std::int32_t percentage { 0 };
    std::string message;
    std::int32_t nesting { 0 };
};

class Installer
{
public:
    // Which bus to reach RAUC on. The session bus is how this is exercised off
    // the board: a stub owns de.pengutronix.rauc there and the whole install
    // path -- progress, completion, failure -- runs on a workstation.
    enum class Bus { System, Session };

    struct Callbacks
    {
        std::function<void(const Progress&)> onProgress;
        // RAUC's result code: 0 is success, anything else is a failure whose
        // description is in LastError.
        std::function<void(std::int32_t result, const std::string& lastError)> onCompleted;
    };

    explicit Installer(Bus bus, Callbacks callbacks);
    ~Installer();

    Installer(const Installer&) = delete;
    Installer& operator=(const Installer&) = delete;

    // Connects and starts the callback thread. False means RAUC is not on the
    // bus -- on the board that is rauc.service failing to activate.
    bool connect(std::string& error);

    bool connected() const;

    // Whether anything currently OWNS de.pengutronix.rauc on the bus.
    //
    // Distinct from connected() on purpose. g_dbus_proxy_new_for_bus_sync()
    // happily builds a proxy for a name nobody owns -- it does not fail until a
    // call times out -- so "the proxy exists" says nothing about whether RAUC is
    // there. And this must NOT gate connect(): on the image rauc.service is
    // Type=dbus and bus-activated, so the name legitimately has no owner until
    // the first method call starts it. Use this to report availability and to
    // skip tests, never to refuse to connect.
    bool serviceAvailable() const;

    std::optional<Status> status() const;
    std::vector<SlotStatus> slots() const;

    // Returns false if RAUC is busy or the call is refused; `error` says which.
    // Completion arrives through onCompleted, not from here.
    bool installBundle(const std::string& path, std::string& error);

    // Mark(state, slot) -> (slot_name, message).
    bool mark(const std::string& state, const std::string& slot, std::string& slotName,
              std::string& message, std::string& error);

    // Public only so the file-scope GDBus callbacks can name it. It stays an
    // opaque forward declaration here, so nothing glib leaks into consumers --
    // which is the property that actually matters, not the access specifier.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace rauc_client

#endif  // RAUC_CLIENT_RAUC_CLIENT_H_
