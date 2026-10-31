#include "FakeRdram.hpp"
#include "FakeServer.hpp"
#include "PracticeSession.hpp"
#include "ReplayEvents.hpp"
#include "TestMain.hpp"

#include <thread>

using namespace PracticeProtocol;
using namespace MatchLayout;

namespace
{
struct FakeSlots : StateSlots
{
    std::vector<int> saved;
    std::vector<int> loaded;
    bool Save(uint8_t slot) override { saved.push_back(slot); return true; }
    LoadOutcome Load(uint8_t slot) override { loaded.push_back(slot); return LoadOutcome::Requested; }
};

// A session wired to a loopback fake server that has already completed the
// handshake. Replies are queued one frame at a time: the session discards
// replies for any frame other than the one it is waiting on.
struct Harness
{
    FakeRdram    ram;
    FakeServer   server;
    FakeSlots    slots;
    PracticeSession session;

    static PracticeSession::Config MakeConfig(uint16_t port)
    {
        PracticeSession::Config config;
        config.port                  = port;
        config.timeoutMs             = 40;
        config.handshakeTimeoutMs    = 500;
        config.retryIntervalMs       = 20;
        config.goodName              = "SmashRemix2.0.1";
        config.recorderSchemaVersion = 3;
        return config;
    }

    explicit Harness(bool acceptHello = true, uint8_t gameStatus = 1)
        : session(MakeConfig(server.Port()), slots)
    {
        Setup(ram, gameStatus);
        session.Start();
        CHECK(server.AcceptClient());
        for (int i = 0; i < 300 && !session.IsConnected(); i++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        server.Send(BuildHelloAck(kProtocolVersion, acceptHello));
        session.OnFrame(); // sends Hello, consumes the (already queued) HelloAck
    }

    // Reads the next message the emulator sent.
    bool Next(Message& out) { return server.Receive(out, 1000); }

    // The constructor's OnFrame() performed the handshake and, because a match
    // was already live, also sent MatchBegin. Consume both; `matchBegin` is
    // left holding the MatchBegin message.
    void SkipToMatchLive(Message& matchBegin)
    {
        Message hello;
        CHECK(Next(hello));
        CHECK_EQ(hello.type, uint8_t(MsgType::Hello));
        CHECK(Next(matchBegin));
        CHECK_EQ(matchBegin.type, uint8_t(MsgType::MatchBegin));
    }

    void Reply(int32_t frame, std::vector<RawCommand> commands = {})
    {
        server.Send(BuildCommands(frame, commands));
    }
};

struct ParsedFrame
{
    int32_t              frame = 0;
    uint8_t              flags = 0;
    std::vector<uint8_t> results;
    size_t               eventBytes = 0;
};

ParsedFrame ParseFrame(const Message& message)
{
    ParsedFrame parsed;
    std::memcpy(&parsed.frame, message.payload.data(), 4);
    parsed.flags = message.payload[4];
    const uint8_t resultCount = message.payload[5];
    parsed.results.assign(message.payload.begin() + 6, message.payload.begin() + 6 + resultCount);
    parsed.eventBytes = message.payload.size() - 6 - resultCount;
    return parsed;
}
} // namespace

TEST(Session_says_hello_with_goodname_schema_and_event_table)
{
    Harness h;
    Message hello;
    CHECK(h.Next(hello));
    CHECK_EQ(hello.type, uint8_t(MsgType::Hello));
    CHECK_EQ(hello.payload[0], uint8_t(kProtocolVersion));
    CHECK(std::string(reinterpret_cast<const char*>(&hello.payload[2])) == "SmashRemix2.0.1");
    CHECK_EQ(hello.payload[2 + 64], 3);          // recorderSchemaVersion
    CHECK_EQ(hello.payload[2 + 64 + 4], 8);      // smash64 declares 8 event codes
    CHECK(h.session.IsHandshaken());
}

TEST(Session_refusing_hello_disconnects_and_never_retries)
{
    Harness h(/*acceptHello=*/false);
    CHECK(!h.session.IsHandshaken());
    CHECK(!h.server.AcceptClient(300)); // emulator does not come back
}

TEST(Session_sends_matchbegin_once_the_match_goes_live_then_a_frame_per_tick)
{
    Harness h;
    Message message;
    h.SkipToMatchLive(message);
    CHECK_EQ(message.payload[4], uint8_t(ReplayEvents::EventCode::MatchStart));

    h.Reply(0);
    h.session.OnFrame();
    CHECK(h.Next(message));
    CHECK_EQ(message.type, uint8_t(MsgType::Frame));
    const ParsedFrame first = ParseFrame(message);
    CHECK_EQ(first.frame, 0);
    CHECK_EQ(first.flags, 0);
    CHECK(first.eventBytes > 0);

    h.Reply(1);
    h.session.OnFrame();
    CHECK(h.Next(message));
    CHECK_EQ(ParseFrame(message).frame, 1);
}

TEST(Session_applies_the_reply_before_the_next_frame_and_reports_results)
{
    Harness h;
    Message message;
    h.SkipToMatchLive(message);

    SetPlayerStateCmd cmd{};
    cmd.port          = 0;
    cmd.mask          = ReplayMemory::WRITE_DAMAGE_PERCENT;
    cmd.damagePercent = 90;
    h.Reply(0, {MakeCommand(CmdCode::SetPlayerState, cmd)});
    h.session.OnFrame();
    CHECK(h.Next(message)); // Frame 0
    CHECK_EQ(h.ram.U32(kPlayerStruct0 + PS_DAMAGE), 90u);

    h.Reply(1);
    h.session.OnFrame();
    CHECK(h.Next(message)); // Frame 1 carries frame 0's command results
    const ParsedFrame second = ParseFrame(message);
    CHECK_EQ(second.results.size(), size_t(1));
    CHECK_EQ(second.results[0], uint8_t(Result::Ok));
}

TEST(Session_discards_stale_replies_and_applies_the_matching_one)
{
    Harness h;
    Message message;
    h.SkipToMatchLive(message);

    SetPlayerStateCmd stale{};
    stale.port          = 0;
    stale.mask          = ReplayMemory::WRITE_DAMAGE_PERCENT;
    stale.damagePercent = 11;
    SetPlayerStateCmd fresh = stale;
    fresh.damagePercent = 22;
    h.Reply(99, {MakeCommand(CmdCode::SetPlayerState, stale)}); // wrong frame
    h.Reply(0, {MakeCommand(CmdCode::SetPlayerState, fresh)});
    h.session.OnFrame();
    CHECK_EQ(h.ram.U32(kPlayerStruct0 + PS_DAMAGE), 22u);
}

TEST(Session_a_missing_reply_times_out_flags_the_next_frame_and_does_not_apply_anything)
{
    Harness h;
    Message message;
    h.SkipToMatchLive(message);

    h.session.OnFrame(); // no reply queued: waits ~timeoutMs, gives up
    CHECK(h.Next(message));
    CHECK_EQ(ParseFrame(message).frame, 0);
    CHECK(h.session.IsConnected());

    h.Reply(1);
    h.session.OnFrame();
    CHECK(h.Next(message));
    const ParsedFrame next = ParseFrame(message);
    CHECK_EQ(next.frame, 1);
    CHECK((next.flags & kFrameFlagReplyTimedOutPreviously) != 0);
}

TEST(Session_disconnects_after_too_many_consecutive_timeouts)
{
    Harness h;
    Message message;
    h.SkipToMatchLive(message);

    for (int i = 0; i < 8; i++)
    {
        h.session.OnFrame();
    }
    // The connection is dropped; the connector's retry makes a fresh one.
    CHECK(h.server.AcceptClient(1000));
}

TEST(Session_LoadState_is_pending_then_the_next_frame_is_flagged_stateLoaded)
{
    Harness h;
    Message message;
    h.SkipToMatchLive(message);

    SlotCmd load{3};
    h.Reply(0, {MakeCommand(CmdCode::LoadState, load)});
    h.session.OnFrame();
    CHECK(h.Next(message)); // Frame 0
    CHECK_EQ(h.slots.loaded.size(), size_t(1));
    CHECK_EQ(h.slots.loaded[0], 3);

    h.Reply(1);
    h.session.OnFrame();
    CHECK(h.Next(message));
    const ParsedFrame after = ParseFrame(message);
    CHECK_EQ(after.frame, 1); // frame numbers never rewind
    CHECK((after.flags & kFrameFlagStateLoaded) != 0);
    CHECK_EQ(after.results.size(), size_t(1));
    CHECK_EQ(after.results[0], uint8_t(Result::Pending));

    h.Reply(2);
    h.session.OnFrame();
    CHECK(h.Next(message));
    CHECK((ParseFrame(message).flags & kFrameFlagStateLoaded) == 0); // one-shot
}

TEST(Session_SaveState_calls_the_slot_store)
{
    Harness h;
    Message message;
    h.SkipToMatchLive(message);

    SlotCmd save{5};
    h.Reply(0, {MakeCommand(CmdCode::SaveState, save)});
    h.session.OnFrame();
    CHECK_EQ(h.slots.saved.size(), size_t(1));
    CHECK_EQ(h.slots.saved[0], 5);
}

TEST(Session_puppet_input_lasts_the_requested_number_of_frames)
{
    Harness h;
    Message message;
    h.SkipToMatchLive(message);

    uint16_t buttons = 0;
    int8_t x = 0, y = 0;
    SetPuppetInputCmd puppet{0, 0x8000, 30, -30, 2};
    h.Reply(0, {MakeCommand(CmdCode::SetPuppetInput, puppet)});
    h.session.OnFrame();
    CHECK(h.session.GetPuppetInput(0, buttons, x, y));
    CHECK_EQ(buttons, 0x8000);

    h.Reply(1);
    h.session.OnFrame();
    CHECK(h.session.GetPuppetInput(0, buttons, x, y)); // second frame

    h.Reply(2);
    h.session.OnFrame();
    CHECK(!h.session.GetPuppetInput(0, buttons, x, y)); // released
}

TEST(Session_sends_matchend_with_results_when_the_match_ends)
{
    Harness h;
    Message message;
    h.SkipToMatchLive(message);

    h.ram.SetU8(kMatchInfo + MI_GAME_STATUS, 5); // ended
    h.session.OnFrame();
    CHECK(h.Next(message));
    CHECK_EQ(message.type, uint8_t(MsgType::MatchEnd));
    CHECK_EQ(message.payload[4], uint8_t(ReplayEvents::EventCode::MatchEnd));
    CHECK_EQ(message.payload[4 + 1 + 4], 1); // endReason: normal
}

TEST(Session_leaving_the_vs_screen_aborts_the_match_and_a_new_match_gets_a_new_serial)
{
    Harness h;
    Message message;
    h.SkipToMatchLive(message);
    uint32_t firstSerial;
    std::memcpy(&firstSerial, message.payload.data(), 4);

    h.ram.SetU8(kCurrentScreen, 0x01); // back to a menu
    h.session.OnFrame();
    CHECK(h.Next(message));
    CHECK_EQ(message.type, uint8_t(MsgType::MatchEnd));
    CHECK_EQ(message.payload[4 + 1 + 4], 0); // endReason: aborted

    h.ram.SetU8(kCurrentScreen, 0x16);
    h.session.OnFrame();
    CHECK(h.Next(message));
    CHECK_EQ(message.type, uint8_t(MsgType::MatchBegin));
    uint32_t secondSerial;
    std::memcpy(&secondSerial, message.payload.data(), 4);
    CHECK_EQ(secondSerial, firstSerial + 1);
}

TEST(Session_does_nothing_outside_a_match)
{
    Harness h(true, /*gameStatus=*/2); // paused / not ongoing-or-countdown
    Message message;
    CHECK(h.Next(message)); // Hello
    h.session.OnFrame();
    CHECK(!h.server.Receive(message, 100)); // no MatchBegin, no Frame
}
