#include "FakeRdram.hpp"
#include "ReplayEventBuilder.hpp"
#include "ReplayEvents.hpp"
#include "TestMain.hpp"

#include <cstring>

using namespace MatchLayout;
using namespace ReplayEvents;

namespace
{
template <typename T>
T ReadAt(const std::vector<uint8_t>& bytes, size_t offset)
{
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}
} // namespace

TEST(AppendFrameEvents_emits_input_then_state_for_each_seated_port)
{
    FakeRdram ram;
    Setup(ram);
    ram.SetU32(kPlayerStruct0 + PS_ACTION_STATE, 0x0A);

    const ReplayMemory::MatchInfo info = ReplayMemory::ReadMatchInfo();
    std::vector<uint8_t> out;
    ReplayEventBuilder::AppendFrameEvents(out, 7, info, true);

    // InputFrame (code + 9) then StateFrame (code + 71); no items, no hazard.
    CHECK_EQ(out.size(), size_t(1 + sizeof(InputFrameEvent) + 1 + sizeof(StateFrameEvent)));
    CHECK_EQ(out[0], uint8_t(EventCode::InputFrame));
    CHECK_EQ(ReadAt<int32_t>(out, 1), 7);
    CHECK_EQ(out[1 + sizeof(InputFrameEvent)], uint8_t(EventCode::StateFrame));

    const size_t s = 1 + sizeof(InputFrameEvent) + 1;
    CHECK_EQ(ReadAt<int32_t>(out, s + 0), 7);       // frame
    CHECK_EQ(out[s + 4], 0);                         // port
    CHECK_EQ(ReadAt<uint16_t>(out, s + 6), 0x0A);    // actionStateId
    CHECK_EQ(ReadAt<float>(out, s + 8), 100.0f);     // positionX
    CHECK_EQ(ReadAt<float>(out, s + 12), 50.0f);     // positionY
    CHECK_EQ(ReadAt<uint32_t>(out, s + 0x1C), 37u);  // damagePercent
    CHECK_EQ(int8_t(out[s + 0x20]), 2);              // stocksRemaining
}

TEST(AppendFrameEvents_without_smash64_emits_only_input_frames)
{
    FakeRdram ram;
    Setup(ram);

    std::vector<uint8_t> out;
    ReplayEventBuilder::AppendFrameEvents(out, 0, ReplayMemory::ReadMatchInfo(), false);
    CHECK_EQ(out.size(), size_t(1 + sizeof(InputFrameEvent)));
}

TEST(AppendFrameEvents_skips_ports_that_are_not_seated)
{
    FakeRdram ram;
    Setup(ram);
    ram.SetU8(kPortBase0 + 0 * kPortStride + PORT_SLOT_TYPE, 2); // un-seat port 0

    std::vector<uint8_t> out;
    ReplayEventBuilder::AppendFrameEvents(out, 0, ReplayMemory::ReadMatchInfo(), true);
    CHECK(out.empty());
}

TEST(AppendEventPayloadsTable_declares_core_plus_smash64_sizes)
{
    std::vector<uint8_t> core, smash;
    ReplayEventBuilder::AppendEventPayloadsTable(core, false);
    ReplayEventBuilder::AppendEventPayloadsTable(smash, true);

    CHECK_EQ(core[0], 3);
    CHECK_EQ(core.size(), size_t(1 + 3 * 3));
    CHECK_EQ(smash[0], 8);
    CHECK_EQ(smash.size(), size_t(1 + 8 * 3));
    // First entry is MatchStart with its declared size.
    CHECK_EQ(smash[1], uint8_t(EventCode::MatchStart));
    CHECK_EQ(ReadAt<uint16_t>(smash, 2), uint16_t(sizeof(MatchStartEvent)));

    std::vector<uint8_t> event;
    ReplayEventBuilder::AppendEventPayloadsEvent(event, true);
    CHECK_EQ(event[0], uint8_t(EventCode::EventPayloads));
    CHECK_EQ(event.size(), smash.size() + 1);
}

TEST(AppendMatchStart_and_Settings_describe_the_match)
{
    FakeRdram ram;
    Setup(ram);
    ram.SetU32(0x80462428, 4); // ADDR_REMIX_HITLAG's live word (written directly: the write API arrives in a later task)

    const ReplayMemory::MatchInfo info = ReplayMemory::ReadMatchInfo();
    std::vector<uint8_t> out;
    ReplayEventBuilder::AppendMatchStart(out, info, {"Ness", "", "", ""});
    ReplayEventBuilder::AppendMatchSettings(out, info);

    CHECK_EQ(out[0], uint8_t(EventCode::MatchStart));
    CHECK(std::memcmp(out.data() + 1, "Ness", 4) == 0);
    CHECK_EQ(out[1 + 128 + 0], 0); // slotType port 0 human
    CHECK_EQ(out[1 + 128 + 1], 2); // slotType port 1 empty

    const size_t m = 1 + sizeof(MatchStartEvent);
    CHECK_EQ(out[m], uint8_t(EventCode::MatchSettings));
    CHECK_EQ(out[m + 1 + 0], 0x06);        // stageId
    CHECK_EQ(out[m + 1 + 0x25], 4);        // hitlag
    CHECK_EQ(out.size(), m + 1 + sizeof(MatchSettingsEvent));
}

TEST(AppendMatchEnd_reports_final_frame_reason_and_placements)
{
    FakeRdram ram;
    Setup(ram);

    std::vector<uint8_t> out;
    ReplayEventBuilder::AppendMatchEnd(out, 123, 1, ReplayMemory::ReadMatchInfo(), true);

    CHECK_EQ(out[0], uint8_t(EventCode::MatchEnd));
    CHECK_EQ(ReadAt<int32_t>(out, 1), 123);
    CHECK_EQ(out[5], 1);
    CHECK_EQ(out[1 + sizeof(MatchEndEvent)], uint8_t(EventCode::MatchResult));
    const size_t r = 1 + sizeof(MatchEndEvent) + 1;
    CHECK_EQ(int8_t(out[r + 0]), 2);   // port 0 stocks
    CHECK_EQ(int8_t(out[r + 1]), -1);  // port 1 never seated
}
