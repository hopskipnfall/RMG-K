// Fake 8 MB big-endian RDRAM installed behind m64p::Core's DebugMem* function
// pointers, so the real ReplayMemory/ReplayEventBuilder code can run on the
// host with no emulator. Addresses are logical (KSEG0 0x80000000-based) and
// byte-addressed big-endian, matching how the real DebugMemRead*/Write*
// behave for the offsets the existing recorder already uses.
#ifndef FAKE_RDRAM_HPP
#define FAKE_RDRAM_HPP

#include "m64p/Api.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

class FakeRdram
{
  public:
    FakeRdram()
    {
        s_Instance = this;
        m_Bytes.assign(0x800000, 0);
        m64p::Core.DebugMemRead8   = &FakeRdram::Read8;
        m64p::Core.DebugMemRead16  = &FakeRdram::Read16;
        m64p::Core.DebugMemRead32  = &FakeRdram::Read32;
        m64p::Core.DebugMemWrite8  = &FakeRdram::WriteB;
        m64p::Core.DebugMemWrite16 = &FakeRdram::WriteH;
        m64p::Core.DebugMemWrite32 = &FakeRdram::WriteW;
    }
    ~FakeRdram() { s_Instance = nullptr; }

    uint8_t  U8(uint32_t a) const  { return m_Bytes[a & 0x7FFFFF]; }
    uint16_t U16(uint32_t a) const { return static_cast<uint16_t>((U8(a) << 8) | U8(a + 1)); }
    uint32_t U32(uint32_t a) const { return (static_cast<uint32_t>(U16(a)) << 16) | U16(a + 2); }
    float    F32(uint32_t a) const { uint32_t b = U32(a); float f; std::memcpy(&f, &b, 4); return f; }

    void SetU8(uint32_t a, uint8_t v)   { m_Bytes[a & 0x7FFFFF] = v; }
    void SetU16(uint32_t a, uint16_t v) { SetU8(a, v >> 8); SetU8(a + 1, v & 0xFF); }
    void SetU32(uint32_t a, uint32_t v) { SetU16(a, v >> 16); SetU16(a + 2, v & 0xFFFF); }
    void SetF32(uint32_t a, float f)    { uint32_t b; std::memcpy(&b, &f, 4); SetU32(a, b); }

  private:
    static unsigned char  Read8(unsigned a)  { return s_Instance->U8(a); }
    static unsigned short Read16(unsigned a) { return s_Instance->U16(a); }
    static unsigned int   Read32(unsigned a) { return s_Instance->U32(a); }
    static void WriteB(unsigned a, unsigned char v)  { s_Instance->SetU8(a, v); }
    static void WriteH(unsigned a, unsigned short v) { s_Instance->SetU16(a, v); }
    static void WriteW(unsigned a, unsigned int v)   { s_Instance->SetU32(a, v); }

    inline static FakeRdram* s_Instance = nullptr;
    std::vector<uint8_t>     m_Bytes;
};

// Lays out one live VS match with port 0 seated (Mario-ish, `characterId`)
// using the same addresses/offsets ReplayMemory.cpp reads. Constants are
// duplicated here deliberately: if ReplayMemory's layout ever changes, these
// tests fail loudly instead of silently following it.
namespace MatchLayout
{
constexpr uint32_t kCurrentScreen   = 0x800A4AD0;
constexpr uint32_t kMatchInfoPtr    = 0x800A50E8;
constexpr uint32_t kRngSeed         = 0x8003B940;
constexpr uint32_t kMatchInfo       = 0x80200000;
constexpr uint32_t kPlayerObject0   = 0x80210000;
constexpr uint32_t kPlayerStruct0   = 0x80220000;
constexpr uint32_t kPosition0       = 0x80230000;
constexpr uint32_t kPortBase0       = kMatchInfo + 0x20; // port 0
constexpr uint32_t kPortStride      = 0x74;

// MatchInfo fields
constexpr uint32_t MI_STAGE_ID = 0x01, MI_GAME_STATUS = 0x11;
// Port fields
constexpr uint32_t PORT_SLOT_TYPE = 0x02, PORT_CHARACTER_ID = 0x03, PORT_STOCKS = 0x0B, PORT_PLAYER_OBJECT = 0x58;
// Player struct fields
constexpr uint32_t PS_ACTION_STATE = 0x24, PS_DAMAGE = 0x2C, PS_FACING = 0x44, PS_VEL_X = 0x48, PS_VEL_Y = 0x4C,
                   PS_POSITION_PTR = 0x78, PS_JUMPS_USED = 0x148, PS_SHIELD = 0x34, PS_PASSIVE = 0xADC,
                   PS_HURTBOX = 0x5BB, PS_SPECIAL_HIT = 0x5AF;

inline void Setup(FakeRdram& ram, uint8_t gameStatus = 1)
{
    ram.SetU8(kCurrentScreen, 0x16);
    ram.SetU32(kMatchInfoPtr, kMatchInfo);
    ram.SetU8(kMatchInfo + MI_STAGE_ID, 0x06);
    ram.SetU8(kMatchInfo + MI_GAME_STATUS, gameStatus);
    for (int port = 0; port < 4; port++)
    {
        ram.SetU8(kPortBase0 + port * kPortStride + PORT_SLOT_TYPE, 2); // empty
    }
    ram.SetU8(kPortBase0 + PORT_SLOT_TYPE, 0); // port 0 human
    ram.SetU8(kPortBase0 + PORT_CHARACTER_ID, 0x00);
    ram.SetU8(kPortBase0 + PORT_STOCKS, 2);
    ram.SetU32(kPortBase0 + PORT_PLAYER_OBJECT, kPlayerObject0);
    ram.SetU32(kPlayerObject0 + 0x84, kPlayerStruct0);
    ram.SetU32(kPlayerStruct0 + PS_POSITION_PTR, kPosition0);
    ram.SetF32(kPosition0 + 0, 100.0f);
    ram.SetF32(kPosition0 + 4, 50.0f);
    ram.SetU32(kPlayerStruct0 + PS_DAMAGE, 37);
    ram.SetU32(kPlayerStruct0 + PS_FACING, 1);
}
} // namespace MatchLayout

#endif // FAKE_RDRAM_HPP
