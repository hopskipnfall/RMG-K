/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef REPLAY_EVENT_BUILDER_HPP
#define REPLAY_EVENT_BUILDER_HPP

#include "ReplayMemory.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Fills .rmgr events (ReplayEvents.hpp) from live game memory and appends
// them to a byte vector. Shared by the file recorder and the practice client.
// `smash64` selects whether the smash64 extension events are included
// (RMGR_SPEC.md section 2.1).
namespace ReplayEventBuilder
{
// Appends only the declared-size table: u8 count, then count * (u8 code, u16 size).
void AppendEventPayloadsTable(std::vector<uint8_t>& out, bool smash64);

// Appends the full EventPayloads event: code 0x01 followed by the table.
void AppendEventPayloadsEvent(std::vector<uint8_t>& out, bool smash64);

void AppendMatchStart(std::vector<uint8_t>& out, const ReplayMemory::MatchInfo& matchInfo,
                      const std::array<std::string, 4>& playerNames);

// smash64 only: appends MatchSettings.
void AppendMatchSettings(std::vector<uint8_t>& out, const ReplayMemory::MatchInfo& matchInfo);

// One InputFrame (+ StateFrame when smash64) per seated, live port in port
// order; then, when smash64, one ItemUpdate per live item and at most one
// StageHazardUpdate.
void AppendFrameEvents(std::vector<uint8_t>& out, int32_t frame, const ReplayMemory::MatchInfo& matchInfo,
                       bool smash64);

// MatchEnd, then (when smash64) MatchResult.
void AppendMatchEnd(std::vector<uint8_t>& out, int32_t finalFrame, uint8_t endReason,
                    const ReplayMemory::MatchInfo& matchInfo, bool smash64);
} // namespace ReplayEventBuilder

#endif // REPLAY_EVENT_BUILDER_HPP
