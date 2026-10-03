#include "FakeServer.hpp"
#include "PracticeTransport.hpp"
#include "TestMain.hpp"

#include <thread>

using namespace PracticeProtocol;

namespace
{
bool WaitConnected(PracticeTransport& transport, int timeoutMs = 3000)
{
    for (int waited = 0; waited < timeoutMs; waited += 10)
    {
        if (transport.IsConnected())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
} // namespace

TEST(Transport_connects_to_a_listening_server_and_reports_one_new_connection)
{
    FakeServer server;
    PracticeTransport transport;
    transport.Start(server.Port(), 20);

    CHECK(server.AcceptClient());
    CHECK(WaitConnected(transport));
    CHECK(transport.TakeNewConnection());
    CHECK(!transport.TakeNewConnection()); // only once per connection
    transport.Stop();
}

TEST(Transport_sends_and_receives_framed_messages)
{
    FakeServer server;
    PracticeTransport transport;
    transport.Start(server.Port(), 20);
    CHECK(server.AcceptClient());
    CHECK(WaitConnected(transport));

    CHECK(transport.Send(BuildMatchBegin(9, {0x02})));
    Message fromEmulator;
    CHECK(server.Receive(fromEmulator));
    CHECK_EQ(fromEmulator.type, uint8_t(MsgType::MatchBegin));

    CHECK(server.Send(BuildHelloAck(1, true)));
    Message fromServer;
    CHECK(transport.Receive(fromServer, 2000) == PracticeTransport::RecvStatus::Message);
    CHECK_EQ(fromServer.type, uint8_t(MsgType::HelloAck));
    transport.Stop();
}

TEST(Transport_receive_times_out_when_the_server_is_silent)
{
    FakeServer server;
    PracticeTransport transport;
    transport.Start(server.Port(), 20);
    CHECK(server.AcceptClient());
    CHECK(WaitConnected(transport));

    Message message;
    CHECK(transport.Receive(message, 30) == PracticeTransport::RecvStatus::Timeout);
    CHECK(transport.IsConnected()); // a timeout is not a disconnect
    transport.Stop();
}

TEST(Transport_reports_disconnect_then_reconnects)
{
    FakeServer server;
    PracticeTransport transport;
    transport.Start(server.Port(), 20);
    CHECK(server.AcceptClient());
    CHECK(WaitConnected(transport));
    CHECK(transport.TakeNewConnection());

    server.DropClient();
    Message message;
    CHECK(transport.Receive(message, 2000) == PracticeTransport::RecvStatus::Disconnected);
    CHECK(!transport.IsConnected());

    CHECK(server.AcceptClient()); // connector retries on its own
    CHECK(WaitConnected(transport));
    CHECK(transport.TakeNewConnection());
    transport.Stop();
}

TEST(Transport_does_not_reconnect_when_retries_are_disabled)
{
    FakeServer server;
    PracticeTransport transport;
    transport.Start(server.Port(), 20);
    CHECK(server.AcceptClient());
    CHECK(WaitConnected(transport));

    transport.SetRetryAllowed(false);
    transport.Disconnect();
    CHECK(!server.AcceptClient(300)); // nobody connects
    CHECK(!transport.IsConnected());
    transport.Stop();
}

TEST(Transport_drops_the_connection_on_a_framing_violation)
{
    FakeServer server;
    PracticeTransport transport;
    transport.Start(server.Port(), 20);
    CHECK(server.AcceptClient());
    CHECK(WaitConnected(transport));

    const std::vector<uint8_t> zeroLength = {0, 0, 0, 0, 0x81};
    CHECK(server.Send(zeroLength));
    Message message;
    CHECK(transport.Receive(message, 2000) == PracticeTransport::RecvStatus::ProtocolError);
    CHECK(!transport.IsConnected());
    transport.Stop();
}

TEST(Transport_stays_disconnected_with_no_server_listening)
{
    uint16_t freePort;
    {
        FakeServer probe; // grab an ephemeral port, then close it
        freePort = probe.Port();
    }
    PracticeTransport transport;
    transport.Start(freePort, 20);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!transport.IsConnected());
    CHECK(!transport.TakeNewConnection());
    transport.Stop();
}
