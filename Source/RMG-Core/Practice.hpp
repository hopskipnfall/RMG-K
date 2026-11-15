/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef PRACTICE_HPP
#define PRACTICE_HPP

#include <cstdint>

// Practice client: streams live Smash Remix 2.0.1 match state to an external
// practice server over localhost and applies the commands it sends back. See
// docs/PRACTICE_PROTOCOL.md for the wire protocol; the logic lives in
// PracticeSession.cpp, this namespace only gates it and adapts the core.
//
// Thread safety: all four functions are internally synchronized (one mutex),
// so they may be called from the emulation thread (OnFrame,
// GetPuppetInput) and the UI thread (OnEmulationStop) without extra locking.
namespace Practice
{
// Call once per emulation start. `allowed` must be false for any netplay or
// local-rollback session (the practice client rewrites game state). Arms the
// client only when allowed, SettingsID::Practice_Enabled is set, and the
// loaded ROM is Smash Remix 2.0.1. Returns whether it armed.
bool OnEmulationStart(bool allowed);

// Disconnects and frees all savestate slots. Safe to call when not armed.
void OnEmulationStop(void);

// Once per real emulated frame (next to Replay::OnFrame).
void OnFrame(void);

// From the PIF controller-read path: the controller override for `port` for
// the frame about to be simulated, if the server requested one.
bool GetPuppetInput(int port, uint16_t& buttons, int8_t& stickX, int8_t& stickY);
} // namespace Practice

#endif // PRACTICE_HPP
