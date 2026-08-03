// SPDX-License-Identifier: GPL-3.0-or-later
//
// What has to survive a restart for a phone to stay paired.
//
// The failure this guards against is not a crash. Without a stable identity
// everything still works: the accessory just presents a different public key on
// every run, which is invisible from the outside and was the state of things
// until 2026-08-02.
//
// Note what this does *not* buy on the wired path -- the phone re-pairs each
// session regardless, because it uses transient pairing (X-Apple-HKP: 0). The
// value is a stable identity and, from it, a pair-verify check that can
// actually be enforced. See pairing_store.h.
#include "airplay/pairing_store.h"

#include <spdlog/spdlog.h>

#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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

namespace fs = std::filesystem;

fs::path scratchDir()
{
    const fs::path dir = fs::temp_directory_path() / "airplay_pairing_store_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

airplay::Bytes key(uint8_t fill)
{
    return airplay::Bytes(32, fill);
}

}  // namespace

int main()
{
    using airplay::Bytes;
    using airplay::PairingStore;

    const fs::path dir = scratchDir();

    // The whole point: the identity is the same across restarts.
    {
        PairingStore first(dir.string());
        const auto a = first.loadOrCreateIdentity();
        expect(!a.private_key.empty() && !a.public_key.empty(), "an identity is produced");

        PairingStore second(dir.string());
        const auto b = second.loadOrCreateIdentity();
        expect(a.private_key == b.private_key, "the private key survives a restart");
        expect(a.public_key == b.public_key, "and so does the public key");
    }

    // Private key material must not be world readable.
    {
        struct stat info{};
        const fs::path identity = dir / "airplay_identity";
        expect(::stat(identity.c_str(), &info) == 0, "the identity file exists");
        expect((info.st_mode & (S_IRWXG | S_IRWXO)) == 0,
               "and is readable only by its owner -- it holds a private key");
    }

    // A different directory is a different accessory.
    {
        const fs::path other = dir / "other";
        PairingStore a(dir.string());
        PairingStore b(other.string());
        expect(a.loadOrCreateIdentity().public_key != b.loadOrCreateIdentity().public_key,
               "a separate state directory gets its own identity");
    }

    // Disabled: no persistence, and a fresh identity every time. This is the
    // old behaviour, kept for tests and for a bring-up session that wants a
    // clean slate.
    {
        PairingStore none("");
        expect(!none.enabled(), "an empty directory disables the store");
        expect(none.loadOrCreateIdentity().public_key != none.loadOrCreateIdentity().public_key,
               "and yields a new identity on every call");
        none.savePhoneKey("phone", key(1));
        expect(!none.phoneKey("phone").has_value(), "saving is a no-op");
        expect(none.phoneCount() == 0, "and nothing is on file");
    }

    // Phones.
    {
        const fs::path phones_dir = dir / "phones";
        PairingStore store(phones_dir.string());
        expect(store.phoneCount() == 0, "no phones to start");
        expect(!store.phoneKey("unknown").has_value(), "an unknown phone has no key");

        store.savePhoneKey("phone-a", key(0xA1));
        store.savePhoneKey("phone-b", key(0xB2));
        expect(store.phoneCount() == 2, "two phones on file");
        expect(store.phoneKey("phone-a") == key(0xA1), "the first phone's key");
        expect(store.phoneKey("phone-b") == key(0xB2), "the second phone's key");
        expect(!store.phoneKey("phone-c").has_value(), "and no others");

        // A reopened store sees them: this is the restart case.
        PairingStore reopened(phones_dir.string());
        expect(reopened.phoneCount() == 2, "a reopened store sees both");
        expect(reopened.phoneKey("phone-a") == key(0xA1), "with their keys intact");

        // A phone that re-pairs has rotated its key; the new one is what will
        // sign from now on, so it must replace rather than duplicate.
        reopened.savePhoneKey("phone-a", key(0xCC));
        expect(reopened.phoneCount() == 2, "re-pairing does not add a second entry");
        expect(reopened.phoneKey("phone-a") == key(0xCC), "and the new key wins");
    }

    // Identifiers come from the phone, so they are hostile input. One
    // containing whitespace would split into two fields on read and could
    // fabricate an entry for a different phone.
    {
        const fs::path hostile_dir = dir / "hostile";
        PairingStore store(hostile_dir.string());
        store.savePhoneKey("good", key(1));
        store.savePhoneKey("evil other-phone", key(2));
        expect(store.phoneCount() == 1, "an identifier with whitespace is refused");
        expect(!store.phoneKey("other-phone").has_value(), "and forges nothing");

        store.savePhoneKey("", key(3));
        expect(store.phoneCount() == 1, "an empty identifier is refused");
        store.savePhoneKey("empty-key", {});
        expect(store.phoneCount() == 1, "an empty key is refused");
    }

    // A corrupt file must not take the whole store with it.
    {
        const fs::path corrupt_dir = dir / "corrupt";
        fs::create_directories(corrupt_dir);
        {
            std::ofstream out(corrupt_dir / "airplay_pairings");
            out << "good " << std::string(64, 'a') << "\n"
                << "this line is not valid\n"
                << "oddlength abc\n"
                << "nonhex zzzz\n"
                << "also-good " << std::string(64, 'b') << "\n";
        }
        PairingStore store(corrupt_dir.string());
        expect(store.phoneCount() == 2, "unparseable lines are skipped, the rest survive");
        expect(store.phoneKey("good").has_value() && store.phoneKey("also-good").has_value(),
               "and the good entries either side of them are kept");
    }

    fs::remove_all(dir);

    if (failures == 0)
    {
        SPDLOG_INFO("pairing store tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
