// SPDX-License-Identifier: GPL-3.0-or-later

#include "iap2/byte_order.h"
#include "iap2/csm.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace iap2
{

namespace csm
{

namespace
{

template <typename T>
void addBigEndian(ParamList& params, uint16_t id, T value)
{
    Param param;
    param.id = id;
    param.data.resize(sizeof(T));
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        param.data[sizeof(T) - 1 - i] = static_cast<uint8_t>(value >> (8 * i));
    }
    params.push_back(std::move(param));
}

template <typename T>
std::optional<T> getBigEndian(const ParamList& params, uint16_t id)
{
    const Param* param = find(params, id);
    if (param == nullptr)
    {
        return std::nullopt;
    }
    if (param->data.size() != sizeof(T))
    {
        SPDLOG_WARN("[iap2] parameter {} has {} bytes, expected {}", id, param->data.size(), sizeof(T));
        return std::nullopt;
    }

    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        value = static_cast<T>((static_cast<uint64_t>(value) << 8) | param->data[i]);
    }
    return value;
}

}  // namespace

void addNone(ParamList& params, uint16_t id) { params.push_back(Param{id, {}}); }

void addBool(ParamList& params, uint16_t id, bool value)
{
    params.push_back(Param{id, {static_cast<uint8_t>(value ? 1 : 0)}});
}

void addU8(ParamList& params, uint16_t id, uint8_t value) { params.push_back(Param{id, {value}}); }

void addI8(ParamList& params, uint16_t id, int8_t value)
{
    params.push_back(Param{id, {static_cast<uint8_t>(value)}});
}

void addU16(ParamList& params, uint16_t id, uint16_t value) { addBigEndian<uint16_t>(params, id, value); }

void addI16(ParamList& params, uint16_t id, int16_t value)
{
    addBigEndian<uint16_t>(params, id, static_cast<uint16_t>(value));
}

void addU32(ParamList& params, uint16_t id, uint32_t value) { addBigEndian<uint32_t>(params, id, value); }

void addU64(ParamList& params, uint16_t id, uint64_t value) { addBigEndian<uint64_t>(params, id, value); }

void addEnum(ParamList& params, uint16_t id, uint8_t value) { addU8(params, id, value); }

void addString(ParamList& params, uint16_t id, std::string_view value)
{
    Param param;
    param.id = id;
    param.data.assign(value.begin(), value.end());
    param.data.push_back(0);
    params.push_back(std::move(param));
}

void addBytes(ParamList& params, uint16_t id, const std::vector<uint8_t>& value)
{
    params.push_back(Param{id, value});
}

void addGroup(ParamList& params, uint16_t id, const ParamList& group)
{
    params.push_back(Param{id, encodeParams(group)});
}

std::vector<uint8_t> encodeParams(const ParamList& params)
{
    std::vector<uint8_t> out;
    for (const Param& param : params)
    {
        const size_t total = param.data.size() + kCsmParamHeaderSize;
        if (total > 0xFFFF)
        {
            SPDLOG_ERROR("[iap2] parameter {} is {} bytes, too large to encode", param.id, param.data.size());
            continue;
        }
        put_be16(out, static_cast<uint16_t>(total));
        put_be16(out, param.id);
        out.insert(out.end(), param.data.begin(), param.data.end());
    }
    return out;
}

std::vector<uint8_t> encodeMessage(uint16_t msg_id, const ParamList& params)
{
    const std::vector<uint8_t> body = encodeParams(params);
    const size_t total = body.size() + kCsmHeaderSize;
    if (total > 0xFFFF)
    {
        // No name here on purpose: the codec does not have the catalogue, and
        // acquiring one for a log line would invert the dependency.
        SPDLOG_ERROR("[iap2] message 0x{:04X} is {} bytes, too large to encode", msg_id, total);
        return {};
    }

    std::vector<uint8_t> out;
    out.reserve(total);
    put_be16(out, kCsmStart);
    put_be16(out, static_cast<uint16_t>(total));
    put_be16(out, msg_id);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::optional<ParamList> parseParams(const uint8_t* data, size_t len)
{
    ParamList params;
    size_t offset = 0;
    while (offset < len)
    {
        if (len - offset < kCsmParamHeaderSize)
        {
            SPDLOG_WARN("[iap2] truncated parameter header: {} trailing bytes", len - offset);
            return std::nullopt;
        }

        const uint16_t total = get_be16(data + offset);
        const uint16_t id = get_be16(data + offset + 2);
        if (total < kCsmParamHeaderSize || total > len - offset)
        {
            SPDLOG_WARN("[iap2] parameter {} declares length {} but only {} bytes remain", id, total,
                        len - offset);
            return std::nullopt;
        }

        Param param;
        param.id = id;
        param.data.assign(data + offset + kCsmParamHeaderSize, data + offset + total);
        params.push_back(std::move(param));
        offset += total;
    }
    return params;
}

std::optional<Message> parseMessage(const uint8_t* data, size_t len)
{
    if (len < kCsmHeaderSize)
    {
        SPDLOG_WARN("[iap2] control message is only {} bytes, need at least {}", len, kCsmHeaderSize);
        return std::nullopt;
    }

    const uint16_t start = get_be16(data);
    const uint16_t total = get_be16(data + 2);
    const uint16_t msg_id = get_be16(data + 4);
    if (start != kCsmStart)
    {
        SPDLOG_WARN("[iap2] control message has bad start marker 0x{:04X} (expected 0x{:04X})", start,
                    kCsmStart);
        return std::nullopt;
    }
    if (total < kCsmHeaderSize || total > len)
    {
        SPDLOG_WARN("[iap2] control message 0x{:04X} declares length {} but {} bytes are available", msg_id,
                    total, len);
        return std::nullopt;
    }

    auto params = parseParams(data + kCsmHeaderSize, total - kCsmHeaderSize);
    if (!params)
    {
        SPDLOG_WARN("[iap2] control message 0x{:04X} has malformed parameters", msg_id);
        return std::nullopt;
    }

    Message message;
    message.id = msg_id;
    message.params = std::move(*params);
    return message;
}

std::optional<Message> parseMessage(const std::vector<uint8_t>& frame)
{
    return parseMessage(frame.data(), frame.size());
}

std::optional<size_t> peekLength(const uint8_t* data, size_t len)
{
    if (len < kCsmHeaderSize)
    {
        return std::nullopt;
    }
    if (get_be16(data) != kCsmStart)
    {
        return size_t{0};
    }
    const uint16_t total = get_be16(data + 2);
    if (total < kCsmHeaderSize)
    {
        return size_t{0};
    }
    return static_cast<size_t>(total);
}

const Param* find(const ParamList& params, uint16_t id)
{
    for (const Param& param : params)
    {
        if (param.id == id)
        {
            return &param;
        }
    }
    return nullptr;
}

bool has(const ParamList& params, uint16_t id) { return find(params, id) != nullptr; }

std::optional<bool> getBool(const ParamList& params, uint16_t id)
{
    const Param* param = find(params, id);
    if (param == nullptr)
    {
        return std::nullopt;
    }
    if (param->data.empty())
    {
        // LIVI decodes a zero-length boolean as None. Kept faithful, but logged
        // because the iAP2 spec does allow presence-as-value here.
        SPDLOG_DEBUG("[iap2] boolean parameter {} is zero length, treating as absent", id);
        return std::nullopt;
    }
    return param->data[0] != 0;
}

std::optional<uint8_t> getU8(const ParamList& params, uint16_t id)
{
    return getBigEndian<uint8_t>(params, id);
}

std::optional<int8_t> getI8(const ParamList& params, uint16_t id)
{
    const auto value = getBigEndian<uint8_t>(params, id);
    if (!value)
    {
        return std::nullopt;
    }
    return static_cast<int8_t>(*value);
}

std::optional<uint16_t> getU16(const ParamList& params, uint16_t id)
{
    return getBigEndian<uint16_t>(params, id);
}

std::optional<int16_t> getI16(const ParamList& params, uint16_t id)
{
    const auto value = getBigEndian<uint16_t>(params, id);
    if (!value)
    {
        return std::nullopt;
    }
    return static_cast<int16_t>(*value);
}

std::optional<uint32_t> getU32(const ParamList& params, uint16_t id)
{
    return getBigEndian<uint32_t>(params, id);
}

std::optional<uint64_t> getU64(const ParamList& params, uint16_t id)
{
    return getBigEndian<uint64_t>(params, id);
}

std::optional<std::string> getString(const ParamList& params, uint16_t id)
{
    const Param* param = find(params, id);
    if (param == nullptr)
    {
        return std::nullopt;
    }

    size_t len = param->data.size();
    if (len > 0 && param->data[len - 1] == 0)
    {
        --len;
    }
    return std::string(reinterpret_cast<const char*>(param->data.data()), len);
}

std::optional<std::vector<uint8_t>> getBytes(const ParamList& params, uint16_t id)
{
    const Param* param = find(params, id);
    if (param == nullptr)
    {
        return std::nullopt;
    }
    return param->data;
}

std::optional<ParamList> getGroup(const ParamList& params, uint16_t id)
{
    const Param* param = find(params, id);
    if (param == nullptr)
    {
        return std::nullopt;
    }
    auto group = parseParams(param->data.data(), param->data.size());
    if (!group)
    {
        SPDLOG_WARN("[iap2] nested parameter group {} is malformed ({} bytes)", id, param->data.size());
    }
    return group;
}

}  // namespace csm

}  // namespace iap2
