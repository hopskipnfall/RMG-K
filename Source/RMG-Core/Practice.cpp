/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "Practice.hpp"
#include "Callback.hpp"
#include "Library.hpp"
#include "PracticeSession.hpp"
#include "ReplayEvents.hpp"
#include "RollbackNetcode.hpp"
#include "RomSettings.hpp"
#include "Settings.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>

namespace
{
// StateSlots backed by the core's rollback save/load: one in-memory buffer
// per slot, freed on emulation stop. Load is deferred (the core performs it
// at its next safe interrupt boundary and keeps the pointer until then), so a
// slot buffer is only freed/replaced by a later Save or at stop.
class CoreStateSlots : public StateSlots
{
  public:
    bool Save(uint8_t slot) override
    {
        if (slot >= m_Slots.size())
        {
            return false;
        }

        CoreRollbackState fresh;
        if (!CoreRollbackSaveGameState(fresh, 0))
        {
            return false;
        }
        CoreRollbackFreeGameState(m_Slots[slot]);
        m_Slots[slot] = fresh;
        return true;
    }

    LoadOutcome Load(uint8_t slot) override
    {
        if (slot >= m_Slots.size() || m_Slots[slot].buffer == nullptr)
        {
            return LoadOutcome::SlotEmpty;
        }
        return CoreRollbackLoadGameStateDeferred(m_Slots[slot]) ? LoadOutcome::Requested : LoadOutcome::Failed;
    }

    void Clear()
    {
        for (CoreRollbackState& state : m_Slots)
        {
            CoreRollbackFreeGameState(state);
        }
    }

  private:
    std::array<CoreRollbackState, PracticeProtocol::kSlotCount> m_Slots{};
};

std::mutex                       s_Mutex;
CoreStateSlots                   s_Slots;
std::unique_ptr<PracticeSession> s_Session;
} // namespace

namespace Practice
{
CORE_EXPORT bool OnEmulationStart(bool allowed)
{
    std::lock_guard<std::mutex> lock(s_Mutex);

    // Drop anything left from an abnormal prior session end.
    s_Session.reset();
    s_Slots.Clear();

    if (!allowed || !CoreSettingsGetBoolValue(SettingsID::Practice_Enabled))
    {
        return false;
    }

    CoreRomSettings romSettings;
    if (!CoreGetCurrentRomSettings(romSettings) || romSettings.GoodName != ReplayEvents::kSmashRemixGoodName)
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Info,
                               "Practice: enabled, but the loaded ROM is not Smash Remix 2.0.1; staying off");
        return false;
    }

    PracticeSession::Config config;
    config.port      = static_cast<uint16_t>(std::clamp(CoreSettingsGetIntValue(SettingsID::Practice_Port), 1, 65535));
    config.timeoutMs = std::clamp(CoreSettingsGetIntValue(SettingsID::Practice_TimeoutMs), 1, 100);
    config.goodName  = romSettings.GoodName;
    config.recorderSchemaVersion = ReplayEvents::kRecorderSchemaVersion;

    s_Session = std::make_unique<PracticeSession>(config, s_Slots);
    s_Session->Start();
    CoreAddCallbackMessage(CoreDebugMessageType::Info,
                           "Practice: waiting for a practice server on 127.0.0.1:" + std::to_string(config.port));
    return true;
}

CORE_EXPORT void OnEmulationStop(void)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Session.reset();
    s_Slots.Clear();
}

CORE_EXPORT void OnFrame(void)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    if (s_Session)
    {
        s_Session->OnFrame();
    }
}

CORE_EXPORT bool GetPuppetInput(int port, uint16_t& buttons, int8_t& stickX, int8_t& stickY)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    return s_Session && s_Session->GetPuppetInput(port, buttons, stickX, stickY);
}
} // namespace Practice
