/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "PracticeProtocol.hpp"

#include <algorithm>
#include <cstring>

namespace PracticeProtocol
{
namespace
{
template <typename T>
void Put(std::vector<uint8_t>& out, const T& value)
{
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

void PutBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

template <typename T>
bool Take(const std::vector<uint8_t>& in, size_t& offset, T& value)
{
    if (in.size() - offset < sizeof(T))
    {
        return false;
    }
    std::memcpy(&value, in.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}
} // namespace

std::vector<uint8_t> EncodeMessage(MsgType type, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> out;
    out.reserve(5 + payload.size());
    Put(out, static_cast<uint32_t>(1 + payload.size()));
    Put(out, static_cast<uint8_t>(type));
    PutBytes(out, payload);
    return out;
}

std::vector<uint8_t> BuildHello(const std::string& goodName, uint32_t recorderSchemaVersion,
                                const std::vector<uint8_t>& eventPayloadsTable)
{
    std::vector<uint8_t> payload;
    Put(payload, kProtocolVersion);
    char name[64] = {};
    std::memcpy(name, goodName.data(), std::min(goodName.size(), sizeof(name)));
    payload.insert(payload.end(), name, name + sizeof(name));
    Put(payload, recorderSchemaVersion);
    PutBytes(payload, eventPayloadsTable);
    return EncodeMessage(MsgType::Hello, payload);
}

std::vector<uint8_t> BuildMatchBegin(uint32_t matchSerial, const std::vector<uint8_t>& events)
{
    std::vector<uint8_t> payload;
    Put(payload, matchSerial);
    PutBytes(payload, events);
    return EncodeMessage(MsgType::MatchBegin, payload);
}

std::vector<uint8_t> BuildFrame(int32_t frame, uint8_t flags, const std::vector<uint8_t>& results,
                                const std::vector<uint8_t>& events)
{
    std::vector<uint8_t> payload;
    Put(payload, frame);
    Put(payload, flags);
    Put(payload, static_cast<uint8_t>(results.size()));
    PutBytes(payload, results);
    PutBytes(payload, events);
    return EncodeMessage(MsgType::Frame, payload);
}

std::vector<uint8_t> BuildMatchEnd(uint32_t matchSerial, const std::vector<uint8_t>& events)
{
    std::vector<uint8_t> payload;
    Put(payload, matchSerial);
    PutBytes(payload, events);
    return EncodeMessage(MsgType::MatchEnd, payload);
}

std::vector<uint8_t> BuildHelloAck(uint16_t protocolVersion, bool accepted)
{
    std::vector<uint8_t> payload;
    Put(payload, protocolVersion);
    Put(payload, static_cast<uint8_t>(accepted ? 1 : 0));
    return EncodeMessage(MsgType::HelloAck, payload);
}

std::vector<uint8_t> BuildCommands(int32_t frame, const std::vector<RawCommand>& commands)
{
    std::vector<uint8_t> payload;
    Put(payload, frame);
    Put(payload, static_cast<uint8_t>(commands.size()));
    for (const RawCommand& command : commands)
    {
        Put(payload, command.code);
        Put(payload, static_cast<uint16_t>(command.payload.size()));
        PutBytes(payload, command.payload);
    }
    return EncodeMessage(MsgType::Commands, payload);
}

std::optional<HelloAck> ParseHelloAck(const std::vector<uint8_t>& payload)
{
    size_t   offset = 0;
    HelloAck ack;
    uint8_t  accepted = 0;
    if (!Take(payload, offset, ack.protocolVersion) || !Take(payload, offset, accepted))
    {
        return std::nullopt;
    }
    ack.accepted = accepted != 0;
    return ack;
}

std::optional<CommandsMsg> ParseCommands(const std::vector<uint8_t>& payload)
{
    size_t      offset = 0;
    CommandsMsg msg;
    uint8_t     count = 0;
    if (!Take(payload, offset, msg.frame) || !Take(payload, offset, count))
    {
        return std::nullopt;
    }

    for (uint8_t i = 0; i < count; i++)
    {
        RawCommand command;
        uint16_t   size = 0;
        if (!Take(payload, offset, command.code) || !Take(payload, offset, size) ||
            payload.size() - offset < size)
        {
            return std::nullopt;
        }
        command.payload.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                               payload.begin() + static_cast<std::ptrdiff_t>(offset + size));
        offset += size;
        msg.commands.push_back(std::move(command));
    }
    return msg;
}

void MessageReader::Append(const uint8_t* data, size_t size)
{
    m_Buffer.insert(m_Buffer.end(), data, data + size);
}

MessageReader::Status MessageReader::Next(Message& out)
{
    if (m_Buffer.size() < 4)
    {
        return Status::NeedMore;
    }

    uint32_t length = 0;
    std::memcpy(&length, m_Buffer.data(), sizeof(length));
    if (length == 0 || length > kMaxMessageLength)
    {
        return Status::Error;
    }
    if (m_Buffer.size() < 4 + static_cast<size_t>(length))
    {
        return Status::NeedMore;
    }

    out.type = m_Buffer[4];
    out.payload.assign(m_Buffer.begin() + 5, m_Buffer.begin() + 4 + static_cast<std::ptrdiff_t>(length));
    m_Buffer.erase(m_Buffer.begin(), m_Buffer.begin() + 4 + static_cast<std::ptrdiff_t>(length));
    return Status::Message;
}
} // namespace PracticeProtocol
