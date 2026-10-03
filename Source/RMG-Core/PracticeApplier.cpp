/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "PracticeApplier.hpp"
#include "ReplayMemory.hpp"

using namespace PracticeProtocol;

void PuppetTable::Set(int port, uint16_t buttons, int8_t stickX, int8_t stickY, uint16_t frames)
{
    if (port < 0 || port > 3)
    {
        return;
    }
    m_Entries[port] = Entry{buttons, stickX, stickY, frames};
}

void PuppetTable::Clear()
{
    for (Entry& entry : m_Entries)
    {
        entry = Entry{};
    }
}

void PuppetTable::EndFrame()
{
    for (Entry& entry : m_Entries)
    {
        if (entry.framesLeft > 0)
        {
            entry.framesLeft--;
        }
    }
}

bool PuppetTable::Peek(int port, uint16_t& buttons, int8_t& stickX, int8_t& stickY) const
{
    if (port < 0 || port > 3 || m_Entries[port].framesLeft == 0)
    {
        return false;
    }
    buttons = m_Entries[port].buttons;
    stickX  = m_Entries[port].stickX;
    stickY  = m_Entries[port].stickY;
    return true;
}

namespace
{
bool IsKnownCode(uint8_t code)
{
    return code >= static_cast<uint8_t>(CmdCode::SetPlayerState) && code <= static_cast<uint8_t>(CmdCode::LoadState);
}

Result ApplyLoadState(uint8_t slot, StateSlots& slots, bool& requested)
{
    switch (slots.Load(slot))
    {
    case StateSlots::LoadOutcome::Requested:
        requested = true;
        return Result::Pending;
    case StateSlots::LoadOutcome::SlotEmpty:
        return Result::SlotEmpty;
    case StateSlots::LoadOutcome::Failed:
        break;
    }
    return Result::Failed;
}

Result ApplySetPlayerState(const RawCommand& command, uint32_t matchInfoPtr)
{
    SetPlayerStateCmd cmd{};
    if (!DecodeCommand(command, cmd) || cmd.port > 3 || (cmd.mask & ~ReplayMemory::WRITE_ALL_BITS) != 0)
    {
        return Result::Malformed;
    }

    ReplayMemory::PlayerStateWrite write;
    write.mask              = cmd.mask;
    write.positionX         = cmd.positionX;
    write.positionY         = cmd.positionY;
    write.velocityX         = cmd.velocityX;
    write.velocityY         = cmd.velocityY;
    write.facingDirection   = cmd.facingDirection;
    write.damagePercent     = cmd.damagePercent;
    write.shieldHealth      = cmd.shieldHealth;
    write.stocksRemaining   = cmd.stocksRemaining;
    write.characterSpecific = cmd.characterSpecific;
    write.jumpsUsed         = cmd.jumpsUsed;
    write.hurtboxState      = cmd.hurtboxState;
    write.specialHitStatus  = cmd.specialHitStatus;
    return ReplayMemory::WritePortPlayerState(matchInfoPtr, cmd.port, write) ? Result::Ok : Result::Rejected;
}

Result ApplySetSetting(const RawCommand& command)
{
    SetSettingCmd cmd{};
    if (!DecodeCommand(command, cmd) || cmd.key >= ReplayMemory::kRemixSettingCount)
    {
        return Result::Malformed;
    }
    return ReplayMemory::WriteRemixSetting(cmd.key, cmd.value) ? Result::Ok : Result::Failed;
}

Result ApplySetRng(const RawCommand& command)
{
    SetRngCmd cmd{};
    if (!DecodeCommand(command, cmd))
    {
        return Result::Malformed;
    }
    ReplayMemory::WriteRngSeed(cmd.seed);
    return Result::Ok;
}

Result ApplySaveState(const RawCommand& command, StateSlots& slots)
{
    SlotCmd cmd{};
    if (!DecodeCommand(command, cmd) || cmd.slot >= kSlotCount)
    {
        return Result::Malformed;
    }
    return slots.Save(cmd.slot) ? Result::Ok : Result::Failed;
}

Result ApplySetPuppetInput(const RawCommand& command, PuppetTable& puppets)
{
    SetPuppetInputCmd cmd{};
    if (!DecodeCommand(command, cmd) || cmd.port > 3)
    {
        return Result::Malformed;
    }
    puppets.Set(cmd.port, cmd.buttons, cmd.stickX, cmd.stickY, cmd.durationFrames);
    return Result::Ok;
}
} // namespace

ApplyOutcome ApplyCommands(const CommandsMsg& commands, uint32_t matchInfoPtr, PuppetTable& puppets,
                           StateSlots& slots)
{
    ApplyOutcome outcome;
    outcome.results.assign(commands.commands.size(), static_cast<uint8_t>(Result::Ok));
    auto setResult = [&outcome](size_t index, Result result) { outcome.results[index] = static_cast<uint8_t>(result); };

    for (size_t i = 0; i < commands.commands.size(); i++)
    {
        if (!IsKnownCode(commands.commands[i].code))
        {
            setResult(i, Result::Unknown);
        }
    }

    // 1. LoadState. It is deferred (lands at the core's next safe boundary),
    // which would overwrite any memory write made in this same reply - so a
    // reply containing a well-formed one rejects every state write below. A
    // malformed LoadState has no effect on its neighbors.
    bool hasLoad = false;
    for (size_t i = 0; i < commands.commands.size(); i++)
    {
        const RawCommand& command = commands.commands[i];
        if (command.code != static_cast<uint8_t>(CmdCode::LoadState))
        {
            continue;
        }

        SlotCmd cmd{};
        if (!DecodeCommand(command, cmd) || cmd.slot >= kSlotCount)
        {
            setResult(i, Result::Malformed);
            continue;
        }
        if (hasLoad)
        {
            setResult(i, Result::Rejected); // only the first LoadState counts
            continue;
        }
        hasLoad = true;
        setResult(i, ApplyLoadState(cmd.slot, slots, outcome.loadRequested));
    }

    // 2. State writes, in sent order.
    for (size_t i = 0; i < commands.commands.size(); i++)
    {
        const RawCommand& command = commands.commands[i];
        switch (static_cast<CmdCode>(command.code))
        {
        case CmdCode::SetPlayerState:
            setResult(i, hasLoad ? Result::Rejected : ApplySetPlayerState(command, matchInfoPtr));
            break;
        case CmdCode::SetSetting:
            setResult(i, hasLoad ? Result::Rejected : ApplySetSetting(command));
            break;
        case CmdCode::SetRng:
            setResult(i, hasLoad ? Result::Rejected : ApplySetRng(command));
            break;
        default:
            break;
        }
    }

    // 3. SaveState (captures the state after the writes above).
    for (size_t i = 0; i < commands.commands.size(); i++)
    {
        const RawCommand& command = commands.commands[i];
        if (command.code == static_cast<uint8_t>(CmdCode::SaveState))
        {
            setResult(i, hasLoad ? Result::Rejected : ApplySaveState(command, slots));
        }
    }

    // 4. Puppet inputs (unaffected by a pending load).
    for (size_t i = 0; i < commands.commands.size(); i++)
    {
        const RawCommand& command = commands.commands[i];
        if (command.code == static_cast<uint8_t>(CmdCode::SetPuppetInput))
        {
            setResult(i, ApplySetPuppetInput(command, puppets));
        }
    }

    return outcome;
}
