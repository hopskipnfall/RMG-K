/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef PRACTICE_SESSION_HPP
#define PRACTICE_SESSION_HPP

#include "PracticeApplier.hpp"
#include "PracticeProtocol.hpp"
#include "PracticeTransport.hpp"
#include "ReplayMemory.hpp"

#include <cstdint>
#include <string>
#include <vector>

// One practice-protocol session (docs/PRACTICE_PROTOCOL.md): owns the
// transport, drives the handshake and match lifecycle from game memory, and
// runs the per-frame lockstep exchange. Has no dependency on the emulator
// core beyond ReplayMemory (game memory) and the StateSlots interface, so it
// is fully testable against a fake RDRAM and a loopback fake server.
//
// Not thread-safe: Practice.cpp serializes every call behind one mutex.
class PracticeSession
{
  public:
    struct Config
    {
        uint16_t    port = 46464;
        int         timeoutMs = 8;            // per-frame reply budget
        int         handshakeTimeoutMs = 500; // HelloAck wait, once per connection
        int         retryIntervalMs = 1000;
        int         maxConsecutiveTimeouts = 8;
        std::string goodName;
        uint32_t    recorderSchemaVersion = 0;
    };

    PracticeSession(const Config& config, StateSlots& slots);
    ~PracticeSession();

    void Start();
    void Stop();

    // Once per real emulated frame, from the emulation thread.
    void OnFrame();

    // Controller override for `port` for the frame about to be simulated
    // (read from the PIF controller-read path).
    bool GetPuppetInput(int port, uint16_t& buttons, int8_t& stickX, int8_t& stickY) const;

    bool IsConnected() const { return m_Transport.IsConnected(); }
    bool IsHandshaken() const { return m_Handshaken; }

  private:
    bool Handshake();
    void HandleDisconnect();
    void SendMatchBegin(const ReplayMemory::MatchInfo& info);
    void SendMatchEnd(uint8_t endReason);
    void ExchangeFrame(const ReplayMemory::MatchInfo& info);

    Config               m_Config;
    StateSlots&          m_Slots;
    PracticeTransport    m_Transport;
    PuppetTable          m_Puppets;

    bool                 m_Handshaken = false;
    bool                 m_MatchLive = false;
    uint32_t             m_MatchSerial = 0;
    int32_t              m_Frame = 0;
    bool                 m_PrevSaltyRunbackActive = false;
    ReplayMemory::MatchInfo m_LastMatchInfo{};

    std::vector<uint8_t> m_LastResults;   // results of the last applied Commands, reported in the next Frame
    bool                 m_LoadPending = false;
    bool                 m_ReplyTimedOutPreviously = false;
    int                  m_ConsecutiveTimeouts = 0;
};

#endif // PRACTICE_SESSION_HPP
