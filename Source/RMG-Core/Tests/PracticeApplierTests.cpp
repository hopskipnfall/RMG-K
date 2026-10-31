#include "FakeRdram.hpp"
#include "PracticeApplier.hpp"
#include "ReplayMemory.hpp"
#include "TestMain.hpp"

using namespace PracticeProtocol;
using namespace MatchLayout;

namespace
{
struct FakeSlots : StateSlots
{
    std::vector<int>    saved;
    std::vector<int>    loaded;
    bool                saveOk = true;
    LoadOutcome         loadOutcome = LoadOutcome::Requested;

    bool Save(uint8_t slot) override { saved.push_back(slot); return saveOk; }
    LoadOutcome Load(uint8_t slot) override { loaded.push_back(slot); return loadOutcome; }
};

CommandsMsg Msg(std::vector<RawCommand> commands)
{
    CommandsMsg msg;
    msg.frame    = 0;
    msg.commands = std::move(commands);
    return msg;
}

RawCommand Damage(uint8_t port, uint32_t percent)
{
    SetPlayerStateCmd cmd{};
    cmd.port          = port;
    cmd.mask          = ReplayMemory::WRITE_DAMAGE_PERCENT;
    cmd.damagePercent = percent;
    return MakeCommand(CmdCode::SetPlayerState, cmd);
}

uint8_t R(Result result) { return static_cast<uint8_t>(result); }
} // namespace

TEST(Apply_SetPlayerState_writes_memory_and_reports_ok)
{
    FakeRdram ram;
    Setup(ram);
    PuppetTable puppets;
    FakeSlots slots;

    const ApplyOutcome out = ApplyCommands(Msg({Damage(0, 140)}), kMatchInfo, puppets, slots);
    CHECK_EQ(out.results.size(), size_t(1));
    CHECK_EQ(out.results[0], R(Result::Ok));
    CHECK_EQ(ram.U32(kPlayerStruct0 + PS_DAMAGE), 140u);
}

TEST(Apply_SetPlayerState_to_an_unseated_port_is_rejected)
{
    FakeRdram ram;
    Setup(ram);
    PuppetTable puppets;
    FakeSlots slots;

    const ApplyOutcome out = ApplyCommands(Msg({Damage(1, 140)}), kMatchInfo, puppets, slots);
    CHECK_EQ(out.results[0], R(Result::Rejected));
}

TEST(Apply_rejects_malformed_commands_without_touching_state)
{
    FakeRdram ram;
    Setup(ram);
    PuppetTable puppets;
    FakeSlots slots;

    SetPlayerStateCmd badPort{};
    badPort.port = 9;
    SetPlayerStateCmd badMask{};
    badMask.port = 0;
    badMask.mask = 1u << 20;
    SetSettingCmd badKey{39, 1};
    SlotCmd badSlot{16};
    RawCommand truncated;
    truncated.code    = uint8_t(CmdCode::SetRng);
    truncated.payload = {1, 2}; // needs 4 bytes

    const ApplyOutcome out = ApplyCommands(
        Msg({MakeCommand(CmdCode::SetPlayerState, badPort), MakeCommand(CmdCode::SetPlayerState, badMask),
             MakeCommand(CmdCode::SetSetting, badKey), MakeCommand(CmdCode::SaveState, badSlot),
             MakeCommand(CmdCode::LoadState, badSlot), truncated}),
        kMatchInfo, puppets, slots);

    for (uint8_t result : out.results)
    {
        CHECK_EQ(result, R(Result::Malformed));
    }
    CHECK(slots.saved.empty());
    CHECK(slots.loaded.empty());
    CHECK(!out.loadRequested);
}

TEST(Apply_unknown_command_codes_are_skipped_and_reported)
{
    FakeRdram ram;
    Setup(ram);
    PuppetTable puppets;
    FakeSlots slots;

    RawCommand unknown;
    unknown.code    = 0x7F;
    unknown.payload = {1, 2, 3};
    const ApplyOutcome out = ApplyCommands(Msg({unknown, Damage(0, 5)}), kMatchInfo, puppets, slots);
    CHECK_EQ(out.results[0], R(Result::Unknown));
    CHECK_EQ(out.results[1], R(Result::Ok)); // the rest of the reply still applies
    CHECK_EQ(ram.U32(kPlayerStruct0 + PS_DAMAGE), 5u);
}

TEST(Apply_LoadState_is_pending_and_rejects_state_writes_in_the_same_reply)
{
    FakeRdram ram;
    Setup(ram);
    PuppetTable puppets;
    FakeSlots slots;

    SlotCmd load{3};
    SlotCmd save{4};
    SetRngCmd rng{99};
    SetPuppetInputCmd puppet{0, 0x8000, 10, -10, 5};

    // Deliberately sent in the "wrong" order: the load still goes first.
    const ApplyOutcome out = ApplyCommands(
        Msg({Damage(0, 200), MakeCommand(CmdCode::SetRng, rng), MakeCommand(CmdCode::SaveState, save),
             MakeCommand(CmdCode::SetPuppetInput, puppet), MakeCommand(CmdCode::LoadState, load)}),
        kMatchInfo, puppets, slots);

    CHECK(out.loadRequested);
    CHECK_EQ(out.results[0], R(Result::Rejected));
    CHECK_EQ(out.results[1], R(Result::Rejected));
    CHECK_EQ(out.results[2], R(Result::Rejected));
    CHECK_EQ(out.results[3], R(Result::Ok)); // puppet input is still honored
    CHECK_EQ(out.results[4], R(Result::Pending));
    CHECK_EQ(slots.loaded.size(), size_t(1));
    CHECK_EQ(slots.loaded[0], 3);
    CHECK(slots.saved.empty());
    CHECK_EQ(ram.U32(kPlayerStruct0 + PS_DAMAGE), 37u); // unchanged
    CHECK_EQ(ram.U32(kRngSeed), 0u);

    uint16_t buttons;
    int8_t x, y;
    CHECK(puppets.Peek(0, buttons, x, y));
}

TEST(Apply_only_the_first_LoadState_counts)
{
    FakeRdram ram;
    Setup(ram);
    PuppetTable puppets;
    FakeSlots slots;

    SlotCmd a{1};
    SlotCmd b{2};
    const ApplyOutcome out = ApplyCommands(
        Msg({MakeCommand(CmdCode::LoadState, a), MakeCommand(CmdCode::LoadState, b)}), kMatchInfo, puppets, slots);
    CHECK_EQ(out.results[0], R(Result::Pending));
    CHECK_EQ(out.results[1], R(Result::Rejected));
    CHECK_EQ(slots.loaded.size(), size_t(1));
}

TEST(Apply_LoadState_outcomes_map_to_result_codes)
{
    FakeRdram ram;
    Setup(ram);
    PuppetTable puppets;
    FakeSlots slots;
    SlotCmd load{0};

    slots.loadOutcome = StateSlots::LoadOutcome::SlotEmpty;
    CHECK_EQ(ApplyCommands(Msg({MakeCommand(CmdCode::LoadState, load)}), kMatchInfo, puppets, slots).results[0],
             R(Result::SlotEmpty));

    slots.loadOutcome = StateSlots::LoadOutcome::Failed;
    const ApplyOutcome failed = ApplyCommands(Msg({MakeCommand(CmdCode::LoadState, load)}), kMatchInfo, puppets, slots);
    CHECK_EQ(failed.results[0], R(Result::Failed));
    CHECK(!failed.loadRequested);
}

TEST(Apply_SaveState_captures_after_the_writes_in_the_same_reply)
{
    FakeRdram ram;
    Setup(ram);
    PuppetTable puppets;

    struct OrderSlots : StateSlots
    {
        FakeRdram* ram;
        uint32_t   damageAtSave = 0;
        bool Save(uint8_t) override { damageAtSave = ram->U32(kPlayerStruct0 + PS_DAMAGE); return true; }
        LoadOutcome Load(uint8_t) override { return LoadOutcome::Requested; }
    } slots;
    slots.ram = &ram;

    SlotCmd save{0};
    ApplyCommands(Msg({MakeCommand(CmdCode::SaveState, save), Damage(0, 77)}), kMatchInfo, puppets, slots);
    CHECK_EQ(slots.damageAtSave, 77u);
}

TEST(Apply_SetSetting_and_SetRng_write_game_memory)
{
    FakeRdram ram;
    Setup(ram);
    PuppetTable puppets;
    FakeSlots slots;

    SetSettingCmd setting{1, 3}; // hitlag = Ultimate
    SetRngCmd rng{12345};
    const ApplyOutcome out = ApplyCommands(
        Msg({MakeCommand(CmdCode::SetSetting, setting), MakeCommand(CmdCode::SetRng, rng)}), kMatchInfo, puppets, slots);
    CHECK_EQ(out.results[0], R(Result::Ok));
    CHECK_EQ(out.results[1], R(Result::Ok));
    CHECK_EQ(ReplayMemory::ReadRemixSettings().hitlag, 3);
    CHECK_EQ(ram.U32(kRngSeed), 12345u);
}

TEST(PuppetTable_overrides_for_exactly_the_requested_number_of_frames)
{
    PuppetTable puppets;
    uint16_t buttons = 0;
    int8_t x = 0, y = 0;

    puppets.Set(2, 0x4000, 80, -80, 2);
    CHECK(puppets.Peek(2, buttons, x, y));
    CHECK_EQ(buttons, 0x4000);
    CHECK_EQ(x, 80);
    CHECK_EQ(y, -80);
    CHECK(!puppets.Peek(0, buttons, x, y));

    puppets.EndFrame();
    CHECK(puppets.Peek(2, buttons, x, y)); // 1 frame left
    puppets.EndFrame();
    CHECK(!puppets.Peek(2, buttons, x, y)); // released

    puppets.Set(2, 1, 1, 1, 5);
    puppets.Set(2, 1, 1, 1, 0); // duration 0 releases immediately
    CHECK(!puppets.Peek(2, buttons, x, y));

    puppets.Set(1, 1, 1, 1, 9);
    puppets.Clear();
    CHECK(!puppets.Peek(1, buttons, x, y));
}

TEST(Apply_a_LoadState_that_finds_an_empty_slot_still_rejects_state_writes)
{
    FakeRdram ram;
    Setup(ram);
    PuppetTable puppets;
    FakeSlots slots;
    slots.loadOutcome = StateSlots::LoadOutcome::SlotEmpty;

    SlotCmd load{7};
    const ApplyOutcome out =
        ApplyCommands(Msg({MakeCommand(CmdCode::LoadState, load), Damage(0, 9)}), kMatchInfo, puppets, slots);
    CHECK_EQ(out.results[0], R(Result::SlotEmpty));
    CHECK_EQ(out.results[1], R(Result::Rejected));
    CHECK(!out.loadRequested);
    CHECK_EQ(ram.U32(kPlayerStruct0 + PS_DAMAGE), 37u);
}
