/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef REPLAY_HPP
#define REPLAY_HPP

// New, independent per-match ".rmgr" replay recorder. Entirely separate
// from Source/n02's .krec format - does not read, write, or otherwise
// touch anything under Source/n02/.
namespace Replay
{
// Call once, right after CoreStartEmulation registers the frame callback.
// Reads SettingsID::GameStats_ReplayEnabled and arms (or doesn't) the
// per-match state machine driven by OnFrame().
void OnEmulationStart(void);

// Call from CoreStopEmulation. Finalizes (patches the length field and
// closes) any file still open, e.g. if the user quit mid-match.
void OnEmulationStop(void);

// Call once per real emulated frame (i.e. from the same place krec's
// frame-counter hook lives) - never during GekkoNet rollback resimulation.
// Owns the whole "waiting for a match to start" / "recording" state
// machine; safe to call every frame regardless of current state.
void OnFrame(void);
} // namespace Replay

#endif // REPLAY_HPP
