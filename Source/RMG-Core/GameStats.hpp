/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_GAMESTATS_HPP
#define CORE_GAMESTATS_HPP

// Publishes per-player game stats (read from N64 memory via the probe
// table in GameStatsProbes.hpp) into a shared-memory segment once per
// visible frame, for a separate reader application to consume. See
// GameStatsTypes.hpp for the published layout and reader protocol.

// creates and maps the shared memory segment; safe to call even when the
// core wasn't built with debugger support or mapping fails, in which case
// game stats are silently disabled for the rest of the session
void CoreInitGameStats(void);

// unmaps and removes the shared memory segment
void CoreStopGameStats(void);

// reads all probes for the current frame and publishes them; called once
// per visible frame from Emulation.cpp's FrameCallback
void CoreUpdateGameStats(unsigned int frameIndex);

#endif // CORE_GAMESTATS_HPP
