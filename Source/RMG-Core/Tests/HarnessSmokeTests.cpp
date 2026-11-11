#include "FakeRdram.hpp"
#include "ReplayMemory.hpp"
#include "TestMain.hpp"

TEST(FakeRdram_round_trips_through_the_DebugMem_function_pointers)
{
    FakeRdram ram;
    m64p::Core.DebugMemWrite32(0x80001000, 0xDEADBEEF);
    CHECK_EQ(m64p::Core.DebugMemRead32(0x80001000), 0xDEADBEEFu);
    CHECK_EQ(m64p::Core.DebugMemRead8(0x80001000), 0xDE); // big-endian: first byte is the most significant

    m64p::Core.DebugMemWrite8(0x80001003, 0x11);
    CHECK_EQ(m64p::Core.DebugMemRead16(0x80001002), 0xBE11);
    m64p::Core.DebugMemWrite16(0x80001000, 0x1234);
    CHECK_EQ(m64p::Core.DebugMemRead32(0x80001000), 0x1234BE11u);
}

TEST(ReplayMemory_reads_the_current_screen_from_the_fake_ram)
{
    FakeRdram ram;
    CHECK(!ReplayMemory::IsInVsMatchScreen());
    MatchLayout::Setup(ram);
    CHECK(ReplayMemory::IsInVsMatchScreen());
}
