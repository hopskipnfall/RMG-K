/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "PracticeTransport.hpp"

#include <algorithm>
#include <chrono>

PracticeTransport::~PracticeTransport()
{
    Stop();
}

void PracticeTransport::Start(uint16_t port, int retryIntervalMs)
{
    Stop();

    PracticeSocket::Startup();
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_StopRequested = false;
        m_RetryAllowed  = true;
        m_NewConnection = false;
        m_Started       = true;
    }
    m_Reader = PracticeProtocol::MessageReader();
    m_Thread = std::thread(&PracticeTransport::ConnectorLoop, this, port, retryIntervalMs);
}

void PracticeTransport::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Started)
        {
            return;
        }
        m_StopRequested = true;
    }
    m_Wake.notify_all();
    if (m_Thread.joinable())
    {
        m_Thread.join();
    }
    Disconnect();
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Started = false;
    }
    PracticeSocket::Shutdown();
}

bool PracticeTransport::IsConnected() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Socket != PracticeSocket::kInvalid;
}

bool PracticeTransport::TakeNewConnection()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_NewConnection)
    {
        return false;
    }
    m_NewConnection = false;
    m_Reader        = PracticeProtocol::MessageReader(); // drop any bytes from a previous connection
    return true;
}

bool PracticeTransport::Send(const std::vector<uint8_t>& bytes)
{
    PracticeSocket::Handle socketHandle;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        socketHandle = m_Socket;
    }
    if (socketHandle == PracticeSocket::kInvalid)
    {
        return false;
    }
    if (!PracticeSocket::SendAll(socketHandle, bytes.data(), bytes.size()))
    {
        Disconnect();
        return false;
    }
    return true;
}

PracticeTransport::RecvStatus PracticeTransport::Receive(PracticeProtocol::Message& out, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    for (;;)
    {
        switch (m_Reader.Next(out))
        {
        case PracticeProtocol::MessageReader::Status::Message:
            return RecvStatus::Message;
        case PracticeProtocol::MessageReader::Status::Error:
            Disconnect();
            return RecvStatus::ProtocolError;
        case PracticeProtocol::MessageReader::Status::NeedMore:
            break;
        }

        PracticeSocket::Handle socketHandle;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            socketHandle = m_Socket;
        }
        if (socketHandle == PracticeSocket::kInvalid)
        {
            return RecvStatus::Disconnected;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int waitMs = static_cast<int>(std::max<long long>(0, remaining.count()));

        const int ready = PracticeSocket::WaitReadable(socketHandle, waitMs);
        if (ready == 0)
        {
            return RecvStatus::Timeout;
        }
        if (ready < 0)
        {
            Disconnect();
            return RecvStatus::Disconnected;
        }

        uint8_t buffer[4096];
        const long received = PracticeSocket::Recv(socketHandle, buffer, sizeof(buffer));
        if (received <= 0)
        {
            Disconnect();
            return RecvStatus::Disconnected;
        }
        m_Reader.Append(buffer, static_cast<size_t>(received));
    }
}

void PracticeTransport::Disconnect()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    PracticeSocket::Close(m_Socket);
    m_Socket = PracticeSocket::kInvalid;
}

void PracticeTransport::SetRetryAllowed(bool allowed)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_RetryAllowed = allowed;
}

void PracticeTransport::ConnectorLoop(uint16_t port, int retryIntervalMs)
{
    std::unique_lock<std::mutex> lock(m_Mutex);
    while (!m_StopRequested)
    {
        if (m_Socket == PracticeSocket::kInvalid && m_RetryAllowed)
        {
            lock.unlock();
            const PracticeSocket::Handle connected = PracticeSocket::ConnectLoopback(port);
            lock.lock();
            if (connected != PracticeSocket::kInvalid)
            {
                if (m_Socket == PracticeSocket::kInvalid && !m_StopRequested)
                {
                    m_Socket        = connected;
                    m_NewConnection = true;
                }
                else
                {
                    PracticeSocket::Close(connected);
                }
            }
        }
        m_Wake.wait_for(lock, std::chrono::milliseconds(retryIntervalMs), [this] { return m_StopRequested; });
    }
}
