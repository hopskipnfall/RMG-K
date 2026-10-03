/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef PRACTICE_TRANSPORT_HPP
#define PRACTICE_TRANSPORT_HPP

#include "PracticeProtocol.hpp"
#include "PracticeSocket.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

// Client end of the practice protocol's TCP link (docs/PRACTICE_PROTOCOL.md
// section 1): a background thread keeps (re)connecting to the server on
// 127.0.0.1 while disconnected; the thread that owns the session (the
// emulation thread) does all sends and receives.
//
// Threading: Start/Stop/Send/Receive/Disconnect/TakeNewConnection must all be
// called from one thread at a time (the practice session serializes them
// behind its own mutex). Only the internal connector thread runs on its own.
class PracticeTransport
{
  public:
    enum class RecvStatus
    {
        Message,
        Timeout,
        Disconnected,
        ProtocolError, // framing violation; the connection has been dropped
    };

    PracticeTransport() = default;
    ~PracticeTransport();

    PracticeTransport(const PracticeTransport&) = delete;
    PracticeTransport& operator=(const PracticeTransport&) = delete;

    // Starts the connector thread. Retries every `retryIntervalMs` while
    // disconnected and retries are allowed.
    void Start(uint16_t port, int retryIntervalMs = 1000);
    void Stop();

    bool IsConnected() const;

    // True exactly once per fresh connection, so the caller knows to send Hello.
    bool TakeNewConnection();

    // Blocking full send. On failure drops the connection and returns false.
    bool Send(const std::vector<uint8_t>& bytes);

    // Waits up to timeoutMs for the next complete message (0 = poll once).
    RecvStatus Receive(PracticeProtocol::Message& out, int timeoutMs);

    // Drops the current connection. The connector reconnects unless retries
    // were disabled.
    void Disconnect();

    // false = stay disconnected after the next drop (e.g. the server refused
    // our Hello). Retries are re-enabled by Start().
    void SetRetryAllowed(bool allowed);

  private:
    void ConnectorLoop(uint16_t port, int retryIntervalMs);

    mutable std::mutex      m_Mutex;
    std::condition_variable m_Wake;
    PracticeSocket::Handle  m_Socket = PracticeSocket::kInvalid;
    bool                    m_NewConnection = false;
    bool                    m_StopRequested = false;
    bool                    m_RetryAllowed = true;
    bool                    m_Started = false;
    std::thread             m_Thread;
    PracticeProtocol::MessageReader m_Reader; // emulation thread only
};

#endif // PRACTICE_TRANSPORT_HPP
