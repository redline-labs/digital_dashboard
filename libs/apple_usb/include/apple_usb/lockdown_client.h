// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef APPLE_USB_LOCKDOWN_CLIENT_H_
#define APPLE_USB_LOCKDOWN_CLIENT_H_

#include "apple_usb/byte_stream.h"
#include "apple_usb/pair_record.h"
#include "plist/value.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace apple_usb
{

// Lockdown's own error vocabulary, as it appears in the "Error" field of a
// failed reply. Only the ones a caller acts on differently are named; the rest
// arrive as Other and are distinguishable through lastErrorName().
enum class LockdownError
{
    None,
    // The user has not answered "Trust This Computer?" yet. Not a failure --
    // the request should be repeated until they do.
    PairingDialogResponsePending,
    // The screen is locked. Trust cannot be granted until it is unlocked, and
    // tapping Trust does not clear this.
    PasswordProtected,
    // The device rejected the pair record we hold. Re-pairing fixes it.
    InvalidHostId,
    InvalidPairRecord,
    MissingPairRecord,
    // The user tapped "Don't Trust". Retrying will never succeed.
    UserDeniedPairing,
    // Transport-level: the connection died, or the reply was not a plist.
    Transport,
    Other,
};

const char* toString(LockdownError error);

// What StartService reports back.
struct LockdownService
{
    uint16_t port = 0;
    bool ssl_enabled = false;
};

// A lockdown session with one device, over a stream that reaches port 62078.
//
// The lifecycle the carkit path needs is narrow: check the peer really is
// lockdown, start a session against an existing pair record (which switches the
// stream to TLS), then ask for a service port. Pairing itself is not here yet --
// a device with no record is reported as MissingPairRecord.
class LockdownClient
{
  public:
    // Takes the connection to the lockdown port. `label` is what the device
    // shows in its list of trusted computers.
    LockdownClient(std::unique_ptr<ByteStream> stream, int fd, std::string label);
    ~LockdownClient();

    LockdownClient(const LockdownClient&) = delete;
    LockdownClient& operator=(const LockdownClient&) = delete;

    // Confirms the peer is lockdown. Worth doing first: a mux that connected to
    // the wrong port fails here with a clear answer rather than at StartSession
    // with a confusing one.
    bool queryType(std::string* type_out = nullptr);

    // Reads a device property. Empty domain means the global one.
    std::optional<plist::Value> getValue(const std::string& domain, const std::string& key);

    // Pairs with a device that has no record yet: reads its public key, mints an
    // identity around it, and asks the device to accept it. `error_out` carries
    // why it failed, which the caller needs -- PairingDialogResponsePending and
    // PasswordProtected both mean "ask again shortly", everything else does not.
    //
    // Must be called before startSession(); pairing is what produces the HostID
    // a session is opened with. The returned record already carries the escrow
    // bag from the device's answer and is ready to store.
    std::optional<PairRecord> pair(const std::string& system_buid, const std::string& host_id,
                                   LockdownError* error_out = nullptr);

    // Starts a session against `record`, enabling TLS on the stream if the
    // device asks for it (it always does). LockdownError::None means the client
    // is ready for startService().
    LockdownError startSession(const PairRecord& record);

    // Asks for a service port. Requires a started session. `escrow_bag` is sent
    // when non-empty, which services needing data-protection access require.
    std::optional<LockdownService> startService(const std::string& identifier,
                                                const std::vector<uint8_t>& escrow_bag = {});

    // Ends the session politely. Harmless if none was started.
    void stopSession();

    bool sessionActive() const { return !session_id_.empty(); }

    // The raw "Error" string from the last failed reply, for logging cases the
    // enum folds into Other.
    const std::string& lastErrorName() const { return last_error_name_; }

  private:
    // One request/reply round trip. Returns nullopt on a transport failure.
    std::optional<plist::Value> transact(const plist::Value& request);
    bool sendPlist(const plist::Value& value);
    std::optional<plist::Value> receivePlist(unsigned timeout_ms);

    // Fills in the fields every lockdown request carries.
    plist::Value requestDict(const char* request) const;

    // Reads the "Error" out of a reply, recording it for lastErrorName().
    LockdownError checkResult(const plist::Value& reply, const char* expected_request);

    std::unique_ptr<ByteStream> stream_;
    int fd_ = -1;
    std::string label_;
    std::string session_id_;
    std::string last_error_name_;
};

}  // namespace apple_usb

#endif  // APPLE_USB_LOCKDOWN_CLIENT_H_
