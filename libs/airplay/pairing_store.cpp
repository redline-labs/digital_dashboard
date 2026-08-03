// SPDX-License-Identifier: GPL-3.0-or-later

#include "airplay/pairing_store.h"

#include <spdlog/spdlog.h>

#include <sys/stat.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace airplay
{
namespace
{

namespace fs = std::filesystem;

// A line-oriented text format rather than JSON: two fields and a map of hex
// strings do not need a parser, and the store has to be readable by a human
// staring at a vehicle that will not pair.
//
//   identity   "<private hex> <public hex>"
//   phones     one "<identifier> <public hex>" per line
//
// Identifiers come from the phone and are opaque; they are written last on the
// line only after being checked for whitespace, so a hostile one cannot forge
// an extra entry.

std::string toHex(const Bytes& bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t byte : bytes)
    {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

std::optional<Bytes> fromHex(const std::string& text)
{
    if (text.size() % 2 != 0)
    {
        return std::nullopt;
    }
    const auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Bytes out;
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2)
    {
        const int hi = value(text[i]);
        const int lo = value(text[i + 1]);
        if (hi < 0 || lo < 0)
        {
            return std::nullopt;
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

// 0600 from the moment it exists, not after it is written: the private key is
// in the first write.
bool writePrivate(const fs::path& path, const std::string& contents)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            SPDLOG_WARN("[airplay] cannot write {}", temporary.string());
            return false;
        }
        out << contents;
    }
    if (::chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0)
    {
        SPDLOG_WARN("[airplay] cannot set permissions on {}", temporary.string());
    }
    // Replace atomically, so an interrupted write cannot leave a half-file that
    // reads as a corrupt store and loses every pairing.
    fs::rename(temporary, path, ec);
    if (ec)
    {
        SPDLOG_WARN("[airplay] cannot replace {}: {}", path.string(), ec.message());
        return false;
    }
    return true;
}

std::optional<std::string> readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::map<std::string, Bytes> readPhones(const fs::path& path)
{
    std::map<std::string, Bytes> phones;
    const auto contents = readFile(path);
    if (!contents)
    {
        return phones;
    }
    std::istringstream lines(*contents);
    std::string line;
    while (std::getline(lines, line))
    {
        std::istringstream fields(line);
        std::string identifier;
        std::string hex;
        if (!(fields >> identifier >> hex))
        {
            continue;  // blank or malformed line; the rest of the file stands
        }
        if (const auto key = fromHex(hex); key && !key->empty())
        {
            phones[identifier] = *key;
        }
    }
    return phones;
}

}  // namespace

PairingStore::PairingStore(std::string directory) : directory_(std::move(directory))
{
}

std::string PairingStore::identityPath() const
{
    return (fs::path(directory_) / "airplay_identity").string();
}

std::string PairingStore::phonesPath() const
{
    return (fs::path(directory_) / "airplay_pairings").string();
}

crypto::Ed25519Pair PairingStore::loadOrCreateIdentity()
{
    if (!enabled())
    {
        SPDLOG_DEBUG("[airplay] pairing store disabled; using a session-only identity");
        return crypto::ed25519Generate();
    }

    if (const auto contents = readFile(identityPath()); contents)
    {
        std::istringstream fields(*contents);
        std::string private_hex;
        std::string public_hex;
        if (fields >> private_hex >> public_hex)
        {
            const auto private_key = fromHex(private_hex);
            const auto public_key = fromHex(public_hex);
            if (private_key && public_key && !private_key->empty() && !public_key->empty())
            {
                SPDLOG_INFO("[airplay] loaded the accessory identity from {}", identityPath());
                crypto::Ed25519Pair pair;
                pair.private_key = *private_key;
                pair.public_key = *public_key;
                return pair;
            }
        }
        // Present but unreadable. Generating a new one here would silently
        // invalidate every phone paired against the old key, so say so loudly.
        SPDLOG_ERROR("[airplay] {} exists but could not be parsed; generating a new identity. "
                     "Every previously paired phone will have to pair again.",
                     identityPath());
    }

    const crypto::Ed25519Pair pair = crypto::ed25519Generate();
    if (writePrivate(identityPath(), toHex(pair.private_key) + " " + toHex(pair.public_key) + "\n"))
    {
        SPDLOG_INFO("[airplay] created a new accessory identity in {}", identityPath());
    }
    return pair;
}

std::optional<Bytes> PairingStore::phoneKey(const std::string& identifier) const
{
    if (!enabled())
    {
        return std::nullopt;
    }
    const auto phones = readPhones(phonesPath());
    const auto found = phones.find(identifier);
    return found == phones.end() ? std::nullopt : std::optional<Bytes>(found->second);
}

void PairingStore::savePhoneKey(const std::string& identifier, const Bytes& ltpk)
{
    if (!enabled() || identifier.empty() || ltpk.empty())
    {
        return;
    }
    // An identifier with whitespace in it would split into two fields on read
    // and could fabricate an entry for another phone.
    if (identifier.find_first_of(" \t\r\n") != std::string::npos)
    {
        SPDLOG_WARN("[airplay] refusing to store a pairing whose identifier contains whitespace");
        return;
    }

    auto phones = readPhones(phonesPath());
    phones[identifier] = ltpk;

    std::string contents;
    for (const auto& [id, key] : phones)
    {
        contents += id + " " + toHex(key) + "\n";
    }
    if (writePrivate(phonesPath(), contents))
    {
        SPDLOG_INFO("[airplay] pairing saved for '{}' ({} phone(s) on file)", identifier,
                    phones.size());
    }
}

size_t PairingStore::phoneCount() const
{
    return enabled() ? readPhones(phonesPath()).size() : 0;
}

}  // namespace airplay
