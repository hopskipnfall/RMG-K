/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "ReplayEventBuilder.hpp"
#include "ReplayEvents.hpp"

#include <algorithm>

using namespace ReplayEvents;

namespace ReplayEventBuilder
{
void AppendEventPayloadsTable(std::vector<uint8_t>& out, bool smash64)
{
    struct EventSize
    {
        EventCode code;
        uint16_t  size;
    };
    static constexpr EventSize kCoreSizes[] = {
        {EventCode::MatchStart, static_cast<uint16_t>(sizeof(MatchStartEvent))},
        {EventCode::InputFrame, static_cast<uint16_t>(sizeof(InputFrameEvent))},
        {EventCode::MatchEnd,   static_cast<uint16_t>(sizeof(MatchEndEvent))},
    };
    static constexpr EventSize kSmash64Sizes[] = {
        {EventCode::StateFrame,        static_cast<uint16_t>(sizeof(StateFrameEvent))},
        {EventCode::ItemUpdate,        static_cast<uint16_t>(sizeof(ItemUpdateEvent))},
        {EventCode::StageHazardUpdate, static_cast<uint16_t>(sizeof(StageHazardUpdateEvent))},
        {EventCode::MatchSettings,     static_cast<uint16_t>(sizeof(MatchSettingsEvent))},
        {EventCode::MatchResult,       static_cast<uint16_t>(sizeof(MatchResultEvent))},
    };
    constexpr size_t kCoreCount    = sizeof(kCoreSizes) / sizeof(kCoreSizes[0]);
    constexpr size_t kSmash64Count = sizeof(kSmash64Sizes) / sizeof(kSmash64Sizes[0]);

    AppendValue(out, static_cast<uint8_t>(kCoreCount + (smash64 ? kSmash64Count : 0)));

    auto appendEntries = [&out](const EventSize* entries, size_t entryCount)
    {
        for (size_t i = 0; i < entryCount; i++)
        {
            AppendValue(out, static_cast<uint8_t>(entries[i].code));
            AppendValue(out, entries[i].size);
        }
    };

    appendEntries(kCoreSizes, kCoreCount);
    if (smash64)
    {
        appendEntries(kSmash64Sizes, kSmash64Count);
    }
}

void AppendEventPayloadsEvent(std::vector<uint8_t>& out, bool smash64)
{
    AppendValue(out, static_cast<uint8_t>(EventCode::EventPayloads));
    AppendEventPayloadsTable(out, smash64);
}

void AppendMatchStart(std::vector<uint8_t>& out, const ReplayMemory::MatchInfo& matchInfo,
                      const std::array<std::string, 4>& playerNames)
{
    MatchStartEvent startEvent{};
    for (int port = 0; port < 4; port++)
    {
        ReplayMemory::PortMatchInfo portInfo = ReplayMemory::ReadPortMatchInfo(matchInfo.matchInfoPtr, port);
        startEvent.slotType[port] = portInfo.slotType;
    }

    for (int port = 0; port < 4; port++)
    {
        WriteFixedString(startEvent.playerNames[port], sizeof(startEvent.playerNames[port]), playerNames[port]);
    }

    AppendEvent(out, EventCode::MatchStart, startEvent);
}

void AppendMatchSettings(std::vector<uint8_t>& out, const ReplayMemory::MatchInfo& matchInfo)
{
    MatchSettingsEvent settingsEvent{};
    settingsEvent.stageId           = matchInfo.stageId;
    settingsEvent.gameType          = matchInfo.gameType;
    settingsEvent.stockCountSetting = matchInfo.stockCountSetting;
    settingsEvent.timeLimitMinutes  = matchInfo.timeLimitMinutes;
    settingsEvent.damageRatio       = matchInfo.damageRatio;
    settingsEvent.itemFrequency     = matchInfo.itemFrequency;
    settingsEvent.teamsEnabled      = matchInfo.teamsEnabled ? 1 : 0;
    settingsEvent.handicapMode      = matchInfo.handicapMode;
    settingsEvent.rngSeed           = matchInfo.rngSeed;

    ReplayMemory::RemixSettings remixSettings = ReplayMemory::ReadRemixSettings();
    settingsEvent.hitstun               = remixSettings.hitstun;
    settingsEvent.hitlag                = remixSettings.hitlag;
    settingsEvent.di                    = remixSettings.di;
    settingsEvent.japaneseSounds        = remixSettings.japaneseSounds;
    settingsEvent.japaneseStunSleep     = remixSettings.japaneseStunSleep;
    settingsEvent.momentumSlide         = remixSettings.momentumSlide;
    settingsEvent.shieldStun            = remixSettings.shieldStun;
    settingsEvent.zCancel               = remixSettings.zCancel;
    settingsEvent.punishFailedZCancel   = remixSettings.punishFailedZCancel;
    settingsEvent.improvedAI            = remixSettings.improvedAI;
    settingsEvent.tripping              = remixSettings.tripping;
    settingsEvent.rage                  = remixSettings.rage;
    settingsEvent.footstoolJumping      = remixSettings.footstoolJumping;
    settingsEvent.airDodging            = remixSettings.airDodging;
    settingsEvent.jabLocking            = remixSettings.jabLocking;
    settingsEvent.edgeCJumping          = remixSettings.edgeCJumping;
    settingsEvent.perfectShielding      = remixSettings.perfectShielding;
    settingsEvent.parrying              = remixSettings.parrying;
    settingsEvent.spotDodging           = remixSettings.spotDodging;
    settingsEvent.fastFallAerials       = remixSettings.fastFallAerials;
    settingsEvent.ledgeTrumping         = remixSettings.ledgeTrumping;
    settingsEvent.wallTeching           = remixSettings.wallTeching;
    settingsEvent.chargeSmashes         = remixSettings.chargeSmashes;
    settingsEvent.itemContainers        = remixSettings.itemContainers;
    settingsEvent.gameSpeed             = remixSettings.gameSpeed;
    settingsEvent.specialZoom           = remixSettings.specialZoom;
    settingsEvent.blastzoneWarp         = remixSettings.blastzoneWarp;
    settingsEvent.singleButtonMode      = remixSettings.singleButtonMode;
    settingsEvent.allItemsRDropAerial   = remixSettings.allItemsRDropAerial;
    settingsEvent.moveStaling           = remixSettings.moveStaling;
    settingsEvent.stopwatchItem         = remixSettings.stopwatchItem;
    settingsEvent.stageSelectLayout     = remixSettings.stageSelectLayout;
    settingsEvent.hazardMode            = remixSettings.hazardMode;
    settingsEvent.whispyMode            = remixSettings.whispyMode;
    settingsEvent.saffronPokemonRate    = remixSettings.saffronPokemonRate;
    settingsEvent.pokemonAnnouncer      = remixSettings.pokemonAnnouncer;
    settingsEvent.dragonKingHUD         = remixSettings.dragonKingHUD;
    settingsEvent.cameraMode            = remixSettings.cameraMode;
    settingsEvent.yoshiIslandCloudAnims = remixSettings.yoshiIslandCloudAnims;

    for (int port = 0; port < 4; port++)
    {
        ReplayMemory::PortMatchInfo portInfo = ReplayMemory::ReadPortMatchInfo(matchInfo.matchInfoPtr, port);
        settingsEvent.characterId[port] = portInfo.characterId;
        settingsEvent.costumeId[port]   = portInfo.costumeId;
        settingsEvent.teamColor[port]   = portInfo.teamColor;

        // team/handicap/cpuLevel need the player-object/player-struct
        // chase, which can be unpopulated if the file opened during the
        // pre-match countdown (game_status == 0) before characters have
        // spawned. Left at their zero-initialized default in that case.
        ReplayMemory::PortPlayerState playerState = ReplayMemory::ReadPortPlayerState(matchInfo.matchInfoPtr, port);
        if (playerState.valid)
        {
            settingsEvent.portTeam[port]     = playerState.team;
            settingsEvent.portHandicap[port] = playerState.handicap;
            settingsEvent.portCpuLevel[port] = playerState.cpuLevel;
        }
    }

    AppendEvent(out, EventCode::MatchSettings, settingsEvent);
}

void AppendFrameEvents(std::vector<uint8_t>& out, int32_t frame, const ReplayMemory::MatchInfo& matchInfo,
                       bool smash64)
{
    for (int port = 0; port < 4; port++)
    {
        ReplayMemory::PortMatchInfo portInfo = ReplayMemory::ReadPortMatchInfo(matchInfo.matchInfoPtr, port);
        if (!portInfo.seated)
        {
            continue;
        }

        ReplayMemory::PortPlayerState state = ReplayMemory::ReadPortPlayerState(matchInfo.matchInfoPtr, port);
        if (!state.valid)
        {
            continue;
        }

        InputFrameEvent input{};
        input.frame   = frame;
        input.port    = static_cast<uint8_t>(port);
        input.buttons = state.processedButtons;
        input.stickX  = state.stickX;
        input.stickY  = state.stickY;
        AppendEvent(out, EventCode::InputFrame, input);

        if (smash64)
        {
            StateFrameEvent stateFrame{};
            stateFrame.frame             = frame;
            stateFrame.port              = static_cast<uint8_t>(port);
            stateFrame.characterId       = portInfo.characterId;
            stateFrame.actionStateId     = state.actionStateId;
            stateFrame.positionX         = state.positionX;
            stateFrame.positionY         = state.positionY;
            stateFrame.facingDirection   = state.facingDirection;
            stateFrame.velocityX         = state.velocityX;
            stateFrame.velocityY         = state.velocityY;
            stateFrame.damagePercent     = state.damagePercent;
            stateFrame.stocksRemaining   = portInfo.stocksRemaining;
            // Clamped rather than a raw cast: state.jumpsRemaining is signed
            // and defaults to 0 if the FTAttributes pointer chase ever
            // fails, but could in principle read momentarily negative
            // mid-transition - wrapping that to a large uint8_t via a raw
            // cast would be actively misleading, not just imprecise.
            stateFrame.jumpsRemaining    = static_cast<uint8_t>(std::max(0, state.jumpsRemaining));
            stateFrame.groundedState     = state.groundedState;
            stateFrame.hurtboxState      = state.hurtboxState;
            stateFrame.hitstunCounter    = state.hitstunCounter;
            stateFrame.actionFrameCounter = state.actionFrameCounter;
            stateFrame.comboHitCount     = portInfo.comboHitCount;
            stateFrame.comboDamage       = portInfo.comboDamage;
            stateFrame.scaleX            = state.scaleX;
            stateFrame.scaleY            = state.scaleY;
            stateFrame.characterSpecific = state.characterSpecific;
            stateFrame.shieldHealth      = state.shieldHealth;
            stateFrame.specialHitStatus  = state.specialHitStatus;
            stateFrame.knockbackResist   = state.knockbackResist;
            AppendEvent(out, EventCode::StateFrame, stateFrame);
        }
    }

    if (smash64)
    {
        // One ItemUpdate per currently-live Item/Weapon GObj - after every
        // seated port's InputFrame/StateFrame pair, same as the per-port
        // events above. Zero events written when the list is empty this
        // frame - never a zeroed/placeholder event, same convention as an
        // unseated port.
        for (const ReplayMemory::ItemObject& item : ReplayMemory::ReadItemObjects())
        {
            ItemUpdateEvent itemEvent{};
            itemEvent.frame         = frame;
            itemEvent.objectAddress = item.objectAddress;
            itemEvent.linkId        = item.linkId;
            itemEvent.kind          = item.kind;
            itemEvent.positionX     = item.positionX;
            itemEvent.positionY     = item.positionY;
            itemEvent.positionZ     = item.positionZ;
            itemEvent.scaleX        = item.scaleX;
            itemEvent.scaleY        = item.scaleY;
            AppendEvent(out, EventCode::ItemUpdate, itemEvent);
        }

        // StageHazardUpdate - only written when at least one tracked hazard
        // is active, same sparse convention as ItemUpdate above.
        const ReplayMemory::StageHazards hazards = ReplayMemory::ReadStageHazards(matchInfo.stageId);
        uint8_t hazardFlags = 0;
        if (hazards.whispyBlowing)
        {
            hazardFlags |= kHazardFlagWhispyBlowing;
            if (hazards.whispyBlowingRight)
            {
                hazardFlags |= kHazardFlagWhispyBlowingRight;
            }
        }
        if (hazardFlags != 0)
        {
            StageHazardUpdateEvent hazardEvent{};
            hazardEvent.frame       = frame;
            hazardEvent.hazardFlags = hazardFlags;
            AppendEvent(out, EventCode::StageHazardUpdate, hazardEvent);
        }
    }
}

void AppendMatchEnd(std::vector<uint8_t>& out, int32_t finalFrame, uint8_t endReason,
                    const ReplayMemory::MatchInfo& matchInfo, bool smash64)
{
    MatchEndEvent endEvent{};
    endEvent.finalFrame = finalFrame;
    endEvent.endReason  = endReason;
    AppendEvent(out, EventCode::MatchEnd, endEvent);

    if (smash64)
    {
        MatchResultEvent resultEvent{};
        for (int port = 0; port < 4; port++)
        {
            ReplayMemory::PortMatchInfo portInfo = ReplayMemory::ReadPortMatchInfo(matchInfo.matchInfoPtr, port);
            resultEvent.placements[port] = portInfo.seated ? portInfo.stocksRemaining : -1;
        }
        AppendEvent(out, EventCode::MatchResult, resultEvent);
    }
}
} // namespace ReplayEventBuilder
