// Loopback fake practice server for transport/session tests.
#ifndef FAKE_SERVER_HPP
#define FAKE_SERVER_HPP

#include "PracticeProtocol.hpp"
#include "PracticeSocket.hpp"

#include <chrono>

class FakeServer
{
  public:
    FakeServer()
    {
        PracticeSocket::Startup();
        m_Listener = PracticeSocket::ListenLoopback(0, &m_Port);
    }
    ~FakeServer()
    {
        DropClient();
        PracticeSocket::Close(m_Listener);
        PracticeSocket::Shutdown();
    }

    uint16_t Port() const { return m_Port; }

    bool AcceptClient(int timeoutMs = 3000)
    {
        DropClient();
        m_Client = PracticeSocket::Accept(m_Listener, timeoutMs);
        m_Reader = PracticeProtocol::MessageReader();
        return m_Client != PracticeSocket::kInvalid;
    }

    void DropClient()
    {
        PracticeSocket::Close(m_Client);
        m_Client = PracticeSocket::kInvalid;
    }

    bool Send(const std::vector<uint8_t>& bytes)
    {
        return PracticeSocket::SendAll(m_Client, bytes.data(), bytes.size());
    }

    // Next complete message from the client, or false on timeout/close.
    bool Receive(PracticeProtocol::Message& out, int timeoutMs = 3000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;)
        {
            if (m_Reader.Next(out) == PracticeProtocol::MessageReader::Status::Message)
            {
                return true;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0 || PracticeSocket::WaitReadable(m_Client, static_cast<int>(left.count())) != 1)
            {
                return false;
            }
            uint8_t buffer[4096];
            const long received = PracticeSocket::Recv(m_Client, buffer, sizeof(buffer));
            if (received <= 0)
            {
                return false;
            }
            m_Reader.Append(buffer, static_cast<size_t>(received));
        }
    }

  private:
    PracticeSocket::Handle          m_Listener = PracticeSocket::kInvalid;
    PracticeSocket::Handle          m_Client = PracticeSocket::kInvalid;
    uint16_t                        m_Port = 0;
    PracticeProtocol::MessageReader m_Reader;
};

#endif // FAKE_SERVER_HPP
