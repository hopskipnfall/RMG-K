/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "PracticeSession.hpp"
#include "ReplayEventBuilder.hpp"

#include <algorithm>
#include <chrono>

using namespace PracticeProtocol;

namespace
{
// Increments per MatchBegin for the life of the process (protocol 3.2).
uint32_t s_NextMatchSerial = 1;
} // namespace

PracticeSession::PracticeSession(const Config& config, StateSlots& slots) : m_Config(config), m_Slots(slots)
{
}

PracticeSession::~PracticeSession()
{
    Stop();
}

void PracticeSession::Start()
{
    m_Handshaken              = false;
    m_MatchLive               = false;
    m_Frame                   = 0;
    m_LastResults.clear();
    m_LoadPending             = false;
    m_ReplyTimedOutPreviously = false;
    m_ConsecutiveTimeouts     = 0;
    m_Puppets.Clear();
    m_Transport.Start(m_Config.port, m_Config.retryIntervalMs);
}

void PracticeSession::Stop()
{
    m_Transport.Stop();
    m_Handshaken = false;
    m_MatchLive  = false;
    m_Puppets.Clear();
}

bool PracticeSession::GetPuppetInput(int port, uint16_t& buttons, int8_t& stickX, int8_t& stickY) const
{
    return m_Puppets.Peek(port, buttons, stickX, stickY);
}

bool PracticeSession::Handshake()
{
    std::vector<uint8_t> table;
    ReplayEventBuilder::AppendEventPayloadsTable(table, true);
    if (!m_Transport.Send(BuildHello(m_Config.goodName, m_Config.recorderSchemaVersion, table)))
    {
        return false;
    }

    Message message;
    if (m_Transport.Receive(message, m_Config.handshakeTimeoutMs) != PracticeTransport::RecvStatus::Message ||
        message.type != static_cast<uint8_t>(MsgType::HelloAck))
    {
        m_Transport.Disconnect(); // retry from scratch
        return false;
    }

    const std::optional<HelloAck> ack = ParseHelloAck(message.payload);
    if (!ack || !ack->accepted || ack->protocolVersion != kProtocolVersion)
    {
        // The server refused (or speaks another version): stay away until restart.
        m_Transport.SetRetryAllowed(false);
        m_Transport.Disconnect();
        return false;
    }

    m_Handshaken = true;
    return true;
}

void PracticeSession::HandleDisconnect()
{
    m_Handshaken              = false;
    m_MatchLive               = false;
    m_LastResults.clear();
    m_LoadPending             = false;
    m_ReplyTimedOutPreviously = false;
    m_ConsecutiveTimeouts     = 0;
    m_Puppets.Clear();
}

void PracticeSession::SendMatchBegin(const ReplayMemory::MatchInfo& info)
{
    std::vector<uint8_t> events;
    ReplayEventBuilder::AppendMatchStart(events, info, {}); // offline: no player names
    ReplayEventBuilder::AppendMatchSettings(events, info);

    m_MatchSerial = s_NextMatchSerial++;
    if (!m_Transport.Send(BuildMatchBegin(m_MatchSerial, events)))
    {
        return;
    }
    m_MatchLive              = true;
    m_Frame                  = 0;
    m_LastResults.clear();
    m_LoadPending            = false;
    m_ReplyTimedOutPreviously = false;
    m_PrevSaltyRunbackActive = ReplayMemory::IsSaltyRunbackActive();
}

void PracticeSession::SendMatchEnd(uint8_t endReason)
{
    ReplayMemory::MatchInfo info = ReplayMemory::ReadMatchInfo();
    if (!info.valid)
    {
        info = m_LastMatchInfo;
    }

    std::vector<uint8_t> events;
    ReplayEventBuilder::AppendMatchEnd(events, m_Frame > 0 ? m_Frame - 1 : 0, endReason, info, true);
    m_Transport.Send(BuildMatchEnd(m_MatchSerial, events));

    m_MatchLive = false;
    m_Puppets.Clear();
    m_LastResults.clear();
    m_LoadPending = false;
}

void PracticeSession::ExchangeFrame(const ReplayMemory::MatchInfo& info)
{
    uint8_t flags = 0;
    if (m_LoadPending)
    {
        flags |= kFrameFlagStateLoaded;
        m_LoadPending = false;
    }
    if (m_ReplyTimedOutPreviously)
    {
        flags |= kFrameFlagReplyTimedOutPreviously;
    }

    std::vector<uint8_t> events;
    ReplayEventBuilder::AppendFrameEvents(events, m_Frame, info, true);
    if (!m_Transport.Send(BuildFrame(m_Frame, flags, m_LastResults, events)))
    {
        return;
    }
    m_LastResults.clear(); // each result is reported exactly once

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_Config.timeoutMs);
    bool       replied = false;
    while (!replied)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        Message message;
        const PracticeTransport::RecvStatus status =
            m_Transport.Receive(message, static_cast<int>(std::max<long long>(0, left.count())));
        if (status == PracticeTransport::RecvStatus::Timeout)
        {
            break;
        }
        if (status != PracticeTransport::RecvStatus::Message)
        {
            return; // disconnected; handled on the next OnFrame
        }
        if (message.type != static_cast<uint8_t>(MsgType::Commands))
        {
            continue;
        }

        const std::optional<CommandsMsg> commands = ParseCommands(message.payload);
        if (!commands || commands->frame != m_Frame)
        {
            continue; // malformed, or a stale/future reply: ignore and keep waiting
        }

        const ApplyOutcome outcome = ApplyCommands(*commands, info.matchInfoPtr, m_Puppets, m_Slots);
        m_LastResults = outcome.results;
        if (outcome.loadRequested)
        {
            m_LoadPending = true;
        }
        replied = true;
    }

    m_ReplyTimedOutPreviously = !replied;
    m_ConsecutiveTimeouts     = replied ? 0 : m_ConsecutiveTimeouts + 1;
    if (m_ConsecutiveTimeouts >= m_Config.maxConsecutiveTimeouts)
    {
        m_Transport.Disconnect(); // reconnect from scratch
    }
    m_Frame++;
}

void PracticeSession::OnFrame()
{
    m_Puppets.EndFrame();

    if (!m_Transport.IsConnected())
    {
        if (m_Handshaken)
        {
            HandleDisconnect();
        }
        return;
    }
    if (m_Transport.TakeNewConnection())
    {
        HandleDisconnect();
        if (!Handshake())
        {
            return;
        }
    }
    if (!m_Handshaken)
    {
        return;
    }

    if (!ReplayMemory::IsInVsMatchScreen())
    {
        if (m_MatchLive)
        {
            SendMatchEnd(0 /* aborted: left the VS match screen */);
        }
        return;
    }

    const ReplayMemory::MatchInfo info = ReplayMemory::ReadMatchInfo();
    if (!info.valid)
    {
        return;
    }
    m_LastMatchInfo = info;

    if (!m_MatchLive)
    {
        if (info.gameStatus == 0 || info.gameStatus == 1)
        {
            SendMatchBegin(info);
        }
        return;
    }

    // Salty Runback restarts play without a normal end-of-match transition;
    // detected by a 0->1 edge, exactly as the replay recorder does (see
    // Replay.cpp OnFrame and ReplayMemory::IsSaltyRunbackActive).
    const bool saltyRunbackActive    = ReplayMemory::IsSaltyRunbackActive();
    const bool saltyRunbackTriggered = saltyRunbackActive && !m_PrevSaltyRunbackActive;
    m_PrevSaltyRunbackActive         = saltyRunbackActive;

    if (info.gameStatus == 5 || saltyRunbackTriggered)
    {
        SendMatchEnd(info.matchWasReset ? 0 : 1);
        return;
    }

    if (info.gameStatus == 1)
    {
        ExchangeFrame(info);
    }
}
