/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef PRACTICE_APPLIER_HPP
#define PRACTICE_APPLIER_HPP

#include "PracticeProtocol.hpp"

#include <cstdint>
#include <vector>

// Applies a server's Commands to the running game (docs/PRACTICE_PROTOCOL.md
// section 4.4 / 6). Game memory goes through ReplayMemory; savestates go
// through the StateSlots interface so this stays testable without the core.

// Controller overrides requested by SetPuppetInput. Read from the PIF
// controller-read path via Peek(); advanced once per emulated frame by
// EndFrame().
class PuppetTable
{
  public:
    // frames == 0 releases the port.
    void Set(int port, uint16_t buttons, int8_t stickX, int8_t stickY, uint16_t frames);
    void Clear();

    // Call once per emulated frame, before applying that frame's reply: one
    // frame of every active override has been consumed.
    void EndFrame();

    // true (and fills the outputs) while `port` is overridden.
    bool Peek(int port, uint16_t& buttons, int8_t& stickX, int8_t& stickY) const;

  private:
    struct Entry
    {
        uint16_t buttons = 0;
        int8_t   stickX = 0;
        int8_t   stickY = 0;
        uint16_t framesLeft = 0;
    };
    Entry m_Entries[4];
};

// Emulator-side savestate slots (docs/PRACTICE_PROTOCOL.md SaveState/LoadState).
class StateSlots
{
  public:
    enum class LoadOutcome
    {
        Requested, // accepted; the core performs it at its next safe boundary
        SlotEmpty,
        Failed,
    };

    virtual ~StateSlots() = default;
    virtual bool        Save(uint8_t slot) = 0;
    virtual LoadOutcome Load(uint8_t slot) = 0;
};

struct ApplyOutcome
{
    std::vector<uint8_t> results;       // one PracticeProtocol::Result per command, in sent order
    bool                 loadRequested = false; // a LoadState was accepted (Result::Pending)
};

// Applies `commands` in the protocol's fixed order (LoadState; then
// SetPlayerState/SetSetting/SetRng; then SaveState; then SetPuppetInput),
// whatever order they were sent in. `matchInfoPtr` must come from a valid
// ReplayMemory::ReadMatchInfo().
ApplyOutcome ApplyCommands(const PracticeProtocol::CommandsMsg& commands, uint32_t matchInfoPtr, PuppetTable& puppets,
                           StateSlots& slots);

#endif // PRACTICE_APPLIER_HPP
