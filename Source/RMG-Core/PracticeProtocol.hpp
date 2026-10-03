/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef PRACTICE_PROTOCOL_HPP
#define PRACTICE_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Wire codec for the practice protocol - see docs/PRACTICE_PROTOCOL.md, which
// is the authoritative spec. Pure byte-shuffling: no sockets, no emulator
// state, so it is unit-testable on its own.
namespace PracticeProtocol
{
constexpr uint16_t kProtocolVersion = 1;
constexpr uint32_t kMaxMessageLength = 1u << 20; // bytes after the length field
constexpr int      kSlotCount = 16;

enum class MsgType : uint8_t
{
    Hello      = 0x01,
    MatchBegin = 0x02,
    Frame      = 0x03,
    MatchEnd   = 0x04,
    HelloAck   = 0x81,
    Commands   = 0x82,
};

enum class CmdCode : uint8_t
{
    SetPlayerState = 0x01,
    SetPuppetInput = 0x02,
    SetSetting     = 0x03,
    SetRng         = 0x04,
    SaveState      = 0x05,
    LoadState      = 0x06,
};

enum class Result : uint8_t
{
    Ok        = 0,
    Unknown   = 1,
    Malformed = 2,
    Rejected  = 3,
    SlotEmpty = 4,
    Failed    = 5,
    Pending   = 6,
};

constexpr uint8_t kFrameFlagStateLoaded            = 0x01;
constexpr uint8_t kFrameFlagReplyTimedOutPreviously = 0x02;

#pragma pack(push, 1)
// Command payloads (docs/PRACTICE_PROTOCOL.md 4.4). `mask` bit layout of
// SetPlayerState is ReplayMemory::PlayerStateWriteBit.
struct SetPlayerStateCmd
{
    uint8_t  port;
    uint32_t mask;
    float    positionX;
    float    positionY;
    float    velocityX;
    float    velocityY;
    int32_t  facingDirection;
    uint32_t damagePercent;
    int32_t  shieldHealth;
    int8_t   stocksRemaining;
    int32_t  characterSpecific;
    uint8_t  jumpsUsed;
    uint8_t  hurtboxState;
    uint8_t  specialHitStatus;
};
static_assert(sizeof(SetPlayerStateCmd) == 41, "SetPlayerState must be 41 bytes");

struct SetPuppetInputCmd
{
    uint8_t  port;
    uint16_t buttons;
    int8_t   stickX;
    int8_t   stickY;
    uint16_t durationFrames;
};
static_assert(sizeof(SetPuppetInputCmd) == 7, "SetPuppetInput must be 7 bytes");

struct SetSettingCmd
{
    uint16_t key;
    uint8_t  value;
};
static_assert(sizeof(SetSettingCmd) == 3, "SetSetting must be 3 bytes");

struct SetRngCmd
{
    int32_t seed;
};
static_assert(sizeof(SetRngCmd) == 4, "SetRng must be 4 bytes");

struct SlotCmd // SaveState / LoadState
{
    uint8_t slot;
};
static_assert(sizeof(SlotCmd) == 1, "SaveState/LoadState must be 1 byte");
#pragma pack(pop)

// One framed message off the wire: type byte + payload.
struct Message
{
    uint8_t              type = 0;
    std::vector<uint8_t> payload;
};

// Wraps `payload` as u32 length | u8 type | payload.
std::vector<uint8_t> EncodeMessage(MsgType type, const std::vector<uint8_t>& payload);

// Emulator -> server builders. `eventPayloadsTable` is the "count + entries"
// block from ReplayEventBuilder::AppendEventPayloadsTable; `events` is an
// event stream (code|payload records).
std::vector<uint8_t> BuildHello(const std::string& goodName, uint32_t recorderSchemaVersion,
                                const std::vector<uint8_t>& eventPayloadsTable);
std::vector<uint8_t> BuildMatchBegin(uint32_t matchSerial, const std::vector<uint8_t>& events);
std::vector<uint8_t> BuildFrame(int32_t frame, uint8_t flags, const std::vector<uint8_t>& results,
                                const std::vector<uint8_t>& events);
std::vector<uint8_t> BuildMatchEnd(uint32_t matchSerial, const std::vector<uint8_t>& events);

// Server -> emulator builders. Used by the emulator's own tests and by the
// fake server in them; a real server implements its own.
std::vector<uint8_t> BuildHelloAck(uint16_t protocolVersion, bool accepted);

struct RawCommand
{
    uint8_t              code = 0;
    std::vector<uint8_t> payload;
};
std::vector<uint8_t> BuildCommands(int32_t frame, const std::vector<RawCommand>& commands);

// Server -> emulator parsers.
struct HelloAck
{
    uint16_t protocolVersion = 0;
    bool     accepted = false;
};
std::optional<HelloAck> ParseHelloAck(const std::vector<uint8_t>& payload);

struct CommandsMsg
{
    int32_t                 frame = 0;
    std::vector<RawCommand> commands;
};
// nullopt if the payload is truncated or a command's declared size overruns it.
std::optional<CommandsMsg> ParseCommands(const std::vector<uint8_t>& payload);

// Decodes a fixed-size command payload; false if the size doesn't match.
template <typename T>
bool DecodeCommand(const RawCommand& command, T& out)
{
    if (command.payload.size() != sizeof(T))
    {
        return false;
    }
    std::copy(command.payload.begin(), command.payload.end(), reinterpret_cast<uint8_t*>(&out));
    return true;
}

template <typename T>
RawCommand MakeCommand(CmdCode code, const T& payload)
{
    RawCommand command;
    command.code = static_cast<uint8_t>(code);
    command.payload.assign(reinterpret_cast<const uint8_t*>(&payload),
                           reinterpret_cast<const uint8_t*>(&payload) + sizeof(T));
    return command;
}

// Reassembles framed messages from an arbitrary byte stream.
class MessageReader
{
  public:
    enum class Status
    {
        NeedMore, // no complete message buffered yet
        Message,  // `out` holds the next message
        Error,    // framing violation (length 0 or > kMaxMessageLength); the stream is unusable
    };

    void   Append(const uint8_t* data, size_t size);
    Status Next(Message& out);

  private:
    std::vector<uint8_t> m_Buffer;
};
} // namespace PracticeProtocol

#endif // PRACTICE_PROTOCOL_HPP
