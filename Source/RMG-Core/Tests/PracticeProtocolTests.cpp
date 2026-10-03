#include "PracticeProtocol.hpp"
#include "TestMain.hpp"

using namespace PracticeProtocol;

namespace
{
std::vector<uint8_t> Bytes(std::initializer_list<int> values)
{
    std::vector<uint8_t> out;
    for (int v : values)
    {
        out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}
} // namespace

// The two example byte sequences in docs/PRACTICE_PROTOCOL.md section 7.
TEST(Protocol_matches_the_documented_example_bytes)
{
    CHECK(BuildHelloAck(1, true) == Bytes({0x04, 0, 0, 0, 0x81, 0x01, 0x00, 0x01}));

    SlotCmd save{2};
    CHECK(BuildCommands(5, {MakeCommand(CmdCode::SaveState, save)}) ==
          Bytes({0x0A, 0, 0, 0, 0x82, 0x05, 0, 0, 0, 0x01, 0x05, 0x01, 0x00, 0x02}));
}

TEST(Protocol_command_struct_sizes_match_the_spec)
{
    CHECK_EQ(sizeof(SetPlayerStateCmd), size_t(41));
    CHECK_EQ(sizeof(SetPuppetInputCmd), size_t(7));
    CHECK_EQ(sizeof(SetSettingCmd), size_t(3));
    CHECK_EQ(sizeof(SetRngCmd), size_t(4));
    CHECK_EQ(sizeof(SlotCmd), size_t(1));
}

TEST(ParseCommands_round_trips_through_BuildCommands)
{
    SetRngCmd rng{-7};
    SetSettingCmd setting{1, 4};
    const std::vector<uint8_t> wire =
        BuildCommands(42, {MakeCommand(CmdCode::SetRng, rng), MakeCommand(CmdCode::SetSetting, setting)});

    MessageReader reader;
    reader.Append(wire.data(), wire.size());
    Message message;
    CHECK(reader.Next(message) == MessageReader::Status::Message);
    CHECK_EQ(message.type, uint8_t(MsgType::Commands));

    const auto parsed = ParseCommands(message.payload);
    CHECK(parsed.has_value());
    CHECK_EQ(parsed->frame, 42);
    CHECK_EQ(parsed->commands.size(), size_t(2));
    CHECK_EQ(parsed->commands[0].code, uint8_t(CmdCode::SetRng));

    SetRngCmd decodedRng{};
    CHECK(DecodeCommand(parsed->commands[0], decodedRng));
    CHECK_EQ(decodedRng.seed, -7);

    SetRngCmd wrongSize{};
    CHECK(!DecodeCommand(parsed->commands[1], wrongSize)); // 3 bytes, not 4
}

TEST(ParseCommands_rejects_a_command_that_overruns_the_payload)
{
    // frame=1, count=1, code=0x05, declared size=9 but only 1 byte follows
    CHECK(!ParseCommands(Bytes({1, 0, 0, 0, 1, 0x05, 9, 0, 0x02})).has_value());
    CHECK(!ParseCommands(Bytes({1, 0, 0})).has_value()); // truncated header
}

TEST(ParseCommands_accepts_an_empty_reply)
{
    const auto parsed = ParseCommands(Bytes({9, 0, 0, 0, 0}));
    CHECK(parsed.has_value());
    CHECK_EQ(parsed->frame, 9);
    CHECK(parsed->commands.empty());
}

TEST(ParseHelloAck_reads_version_and_accepted)
{
    const auto ack = ParseHelloAck(Bytes({1, 0, 1}));
    CHECK(ack.has_value());
    CHECK_EQ(ack->protocolVersion, 1);
    CHECK(ack->accepted);
    CHECK(!ParseHelloAck(Bytes({1})).has_value());
}

TEST(MessageReader_reassembles_split_and_coalesced_messages)
{
    const std::vector<uint8_t> a = BuildHelloAck(1, true);
    const std::vector<uint8_t> b = BuildHelloAck(2, false);
    std::vector<uint8_t> stream = a;
    stream.insert(stream.end(), b.begin(), b.end());

    MessageReader reader;
    Message message;
    // Feed one byte at a time: never a message until the whole first one is in.
    for (size_t i = 0; i + 1 < a.size(); i++)
    {
        reader.Append(&stream[i], 1);
        CHECK(reader.Next(message) == MessageReader::Status::NeedMore);
    }
    reader.Append(&stream[a.size() - 1], 1);
    CHECK(reader.Next(message) == MessageReader::Status::Message);
    CHECK_EQ(ParseHelloAck(message.payload)->protocolVersion, 1);

    reader.Append(&stream[a.size()], b.size());
    CHECK(reader.Next(message) == MessageReader::Status::Message);
    CHECK_EQ(ParseHelloAck(message.payload)->protocolVersion, 2);
    CHECK(reader.Next(message) == MessageReader::Status::NeedMore);
}

TEST(MessageReader_flags_framing_violations)
{
    MessageReader zero;
    const auto zeroLen = Bytes({0, 0, 0, 0, 0x81});
    zero.Append(zeroLen.data(), zeroLen.size());
    Message message;
    CHECK(zero.Next(message) == MessageReader::Status::Error);

    MessageReader huge;
    const auto hugeLen = Bytes({0x01, 0x00, 0x10, 0x00, 0x81}); // 1 MiB + 1
    huge.Append(hugeLen.data(), hugeLen.size());
    CHECK(huge.Next(message) == MessageReader::Status::Error);
}

TEST(BuildFrame_lays_out_header_results_then_events)
{
    const std::vector<uint8_t> wire =
        BuildFrame(3, kFrameFlagStateLoaded, {uint8_t(Result::Ok), uint8_t(Result::Pending)}, {0xAA, 0xBB});
    // length = 1 type + 4 frame + 1 flags + 1 resultCount + 2 results + 2 events = 11
    CHECK(wire == Bytes({11, 0, 0, 0, 0x03, 3, 0, 0, 0, 0x01, 2, 0, 6, 0xAA, 0xBB}));
}

TEST(BuildHello_carries_version_goodname_schema_and_table)
{
    const std::vector<uint8_t> wire = BuildHello("SmashRemix2.0.1", 3, {1, 0x02, 0x84, 0x00});
    CHECK_EQ(wire.size(), size_t(4 + 1 + 2 + 64 + 4 + 4));
    CHECK_EQ(wire[4], uint8_t(MsgType::Hello));
    CHECK_EQ(wire[5], 1); // protocolVersion lo
    CHECK(std::string(reinterpret_cast<const char*>(&wire[7])) == "SmashRemix2.0.1");
    CHECK_EQ(wire[7 + 64], 3); // schema
    CHECK_EQ(wire[7 + 64 + 4], 1); // table count
}
