// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/control_session_message/*.py
//
// The control session message codec: how an iAP2 control message is laid out on
// the wire, and how its parameters are read and written.
//
// This is the wire format only -- what the bytes mean is messages.h's business,
// and keeping the two apart is what lets the codec be tested against byte
// strings without a catalogue of message identifiers in the way.
//
// Parameters are a flat list rather than a map because the protocol allows the
// same id more than once (a device reporting two transport components, say),
// and the order it sent them in is sometimes the only thing distinguishing
// them.
#ifndef IAP2_CSM_H_
#define IAP2_CSM_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace iap2
{

// ---------------------------------------------------------------------------
// Control session message (CSM) framing.
//
//   struct  ">HHH"  = start(0x4040), length(total, incl. this 6 byte header),
//                     message id
//   params  ">HH"   = length(total, incl. this 4 byte header), parameter id
//                     followed by (length - 4) payload bytes
// ---------------------------------------------------------------------------
constexpr uint16_t kCsmStart = 0x4040;
constexpr size_t kCsmHeaderSize = 6;
constexpr size_t kCsmParamHeaderSize = 4;


namespace csm
{

// One serialised parameter. `data` is the parameter payload without the
// 4 byte parameter header.
struct Param
{
    uint16_t id = 0;
    std::vector<uint8_t> data;

    bool operator==(const Param& other) const = default;
};

using ParamList = std::vector<Param>;

// --- encoding helpers -------------------------------------------------------
//
// "None-like" parameters carry no payload at all; on the wire their presence is
// the value (LIVI models them as `NoneLike`). Used by every Start*Updates
// message to select which fields the device should report.
void addNone(ParamList& params, uint16_t id);
void addBool(ParamList& params, uint16_t id, bool value);
void addU8(ParamList& params, uint16_t id, uint8_t value);
void addI8(ParamList& params, uint16_t id, int8_t value);
void addU16(ParamList& params, uint16_t id, uint16_t value);
void addI16(ParamList& params, uint16_t id, int16_t value);
void addU32(ParamList& params, uint16_t id, uint32_t value);
void addU64(ParamList& params, uint16_t id, uint64_t value);
// IntEnum in LIVI is always serialised as a single byte.
void addEnum(ParamList& params, uint16_t id, uint8_t value);
// Strings are UTF-8 with a trailing NUL.
void addString(ParamList& params, uint16_t id, std::string_view value);
void addBytes(ParamList& params, uint16_t id, const std::vector<uint8_t>& value);
// A nested parameter group (LIVI's dataclass-valued parameters).
void addGroup(ParamList& params, uint16_t id, const ParamList& group);

std::vector<uint8_t> encodeParams(const ParamList& params);
std::vector<uint8_t> encodeMessage(uint16_t msg_id, const ParamList& params);

// --- decoding helpers -------------------------------------------------------
struct Message
{
    uint16_t id = 0;
    ParamList params;
};

// Parses the parameter area of a message (everything after the 6 byte header).
std::optional<ParamList> parseParams(const uint8_t* data, size_t len);

// Parses a complete CSM frame (header included). Trailing bytes past the
// declared length are ignored.
std::optional<Message> parseMessage(const uint8_t* data, size_t len);
std::optional<Message> parseMessage(const std::vector<uint8_t>& frame);

// If `data` holds a complete CSM header, returns the total frame length.
// Returns nullopt when there are fewer than 6 bytes; returns 0 when the header
// is present but malformed (bad start marker or a length below the header).
std::optional<size_t> peekLength(const uint8_t* data, size_t len);

const Param* find(const ParamList& params, uint16_t id);
bool has(const ParamList& params, uint16_t id);

std::optional<bool> getBool(const ParamList& params, uint16_t id);
std::optional<uint8_t> getU8(const ParamList& params, uint16_t id);
std::optional<int8_t> getI8(const ParamList& params, uint16_t id);
std::optional<uint16_t> getU16(const ParamList& params, uint16_t id);
std::optional<int16_t> getI16(const ParamList& params, uint16_t id);
std::optional<uint32_t> getU32(const ParamList& params, uint16_t id);
std::optional<uint64_t> getU64(const ParamList& params, uint16_t id);
std::optional<std::string> getString(const ParamList& params, uint16_t id);
std::optional<std::vector<uint8_t>> getBytes(const ParamList& params, uint16_t id);
std::optional<ParamList> getGroup(const ParamList& params, uint16_t id);

}  // namespace csm

}  // namespace iap2

#endif  // IAP2_CSM_H_
