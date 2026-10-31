#include "FakeRdram.hpp"
#include "ReplayMemory.hpp"
#include "TestMain.hpp"

using namespace MatchLayout;

TEST(WritePortPlayerState_writes_only_masked_fields)
{
    FakeRdram ram;
    Setup(ram);

    ReplayMemory::PlayerStateWrite w;
    w.mask         = ReplayMemory::WRITE_POSITION_X | ReplayMemory::WRITE_DAMAGE_PERCENT;
    w.positionX    = -250.5f;
    w.damagePercent = 120;
    w.positionY    = 999.0f; // unmasked, must be ignored

    CHECK(ReplayMemory::WritePortPlayerState(kMatchInfo, 0, w));
    CHECK_EQ(ram.F32(kPosition0 + 0), -250.5f);
    CHECK_EQ(ram.F32(kPosition0 + 4), 50.0f);
    CHECK_EQ(ram.U32(kPlayerStruct0 + PS_DAMAGE), 120u);
}

TEST(WritePortPlayerState_writes_every_field)
{
    FakeRdram ram;
    Setup(ram);

    ReplayMemory::PlayerStateWrite w;
    w.mask              = ReplayMemory::WRITE_ALL_BITS;
    w.positionX         = 1.5f;
    w.positionY         = 2.5f;
    w.velocityX         = 3.5f;
    w.velocityY         = -4.5f;
    w.facingDirection   = -1;
    w.damagePercent     = 88;
    w.shieldHealth      = 40;
    w.stocksRemaining   = 1;
    w.characterSpecific = 5;
    w.jumpsUsed         = 1;
    w.hurtboxState      = 3;
    w.specialHitStatus  = 2;

    CHECK(ReplayMemory::WritePortPlayerState(kMatchInfo, 0, w));
    CHECK_EQ(ram.F32(kPosition0 + 0), 1.5f);
    CHECK_EQ(ram.F32(kPosition0 + 4), 2.5f);
    CHECK_EQ(ram.F32(kPlayerStruct0 + PS_VEL_X), 3.5f);
    CHECK_EQ(ram.F32(kPlayerStruct0 + PS_VEL_Y), -4.5f);
    CHECK_EQ(ram.U32(kPlayerStruct0 + PS_FACING), 0xFFFFFFFFu);
    CHECK_EQ(ram.U32(kPlayerStruct0 + PS_DAMAGE), 88u);
    CHECK_EQ(ram.U32(kPlayerStruct0 + PS_SHIELD), 40u);
    CHECK_EQ(ram.U8(kPortBase0 + PORT_STOCKS), 1);
    CHECK_EQ(ram.U32(kPlayerStruct0 + PS_PASSIVE), 5u);
    CHECK_EQ(ram.U8(kPlayerStruct0 + PS_JUMPS_USED), 1);
    CHECK_EQ(ram.U8(kPlayerStruct0 + PS_HURTBOX), 3);
    CHECK_EQ(ram.U8(kPlayerStruct0 + PS_SPECIAL_HIT), 2);
}

TEST(WritePortPlayerState_rejects_unseated_and_out_of_range_ports)
{
    FakeRdram ram;
    Setup(ram);

    ReplayMemory::PlayerStateWrite w;
    w.mask          = ReplayMemory::WRITE_DAMAGE_PERCENT;
    w.damagePercent = 5;

    CHECK(!ReplayMemory::WritePortPlayerState(kMatchInfo, 1, w)); // slotType 2 = empty
    CHECK(!ReplayMemory::WritePortPlayerState(kMatchInfo, 4, w));
    CHECK(!ReplayMemory::WritePortPlayerState(kMatchInfo, -1, w));
    CHECK_EQ(ram.U32(kPlayerStruct0 + PS_DAMAGE), 37u); // untouched
}

TEST(WritePortPlayerState_fails_cleanly_when_position_pointer_is_invalid)
{
    FakeRdram ram;
    Setup(ram);
    ram.SetU32(kPlayerStruct0 + PS_POSITION_PTR, 0); // not a RDRAM pointer

    ReplayMemory::PlayerStateWrite w;
    w.mask          = ReplayMemory::WRITE_POSITION_X | ReplayMemory::WRITE_DAMAGE_PERCENT;
    w.positionX     = 7.0f;
    w.damagePercent = 5;

    CHECK(!ReplayMemory::WritePortPlayerState(kMatchInfo, 0, w));
    CHECK_EQ(ram.U32(kPlayerStruct0 + PS_DAMAGE), 37u); // nothing partially applied
}

TEST(WriteRemixSetting_maps_key_to_the_same_address_ReadRemixSettings_reads)
{
    FakeRdram ram;
    Setup(ram);

    CHECK(ReplayMemory::WriteRemixSetting(0, 1));   // hitstun
    CHECK(ReplayMemory::WriteRemixSetting(1, 4));   // hitlag
    CHECK(ReplayMemory::WriteRemixSetting(38, 1));  // yoshiIslandCloudAnims
    CHECK(!ReplayMemory::WriteRemixSetting(39, 1)); // out of range

    const ReplayMemory::RemixSettings s = ReplayMemory::ReadRemixSettings();
    CHECK_EQ(s.hitstun, 1);
    CHECK_EQ(s.hitlag, 4);
    CHECK_EQ(s.yoshiIslandCloudAnims, 1);
    CHECK_EQ(s.di, 0);
    CHECK_EQ(ram.U32(0x804623F8), 1u); // ADDR_REMIX_HITSTUN, the live word
}

TEST(WriteRngSeed_writes_sSYUtilsRandomSeed)
{
    FakeRdram ram;
    Setup(ram);

    ReplayMemory::WriteRngSeed(-5);
    CHECK_EQ(ram.U32(kRngSeed), 0xFFFFFFFBu);
    CHECK_EQ(ReplayMemory::ReadMatchInfo().rngSeed, -5);
}
