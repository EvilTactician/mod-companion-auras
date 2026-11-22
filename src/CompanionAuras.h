/*
 * mod-companion-auras: shared helpers for the companion aura and companion name scripts.
 */

#ifndef MOD_COMPANION_AURAS_H
#define MOD_COMPANION_AURAS_H

#include "Creature.h"
#include "ObjectAccessor.h"
#include "Player.h"

#ifdef MOD_PLAYERBOTS
#include "PlayerbotMgr.h"
#endif

namespace CompanionAuras
{
    // Extra effects that are done in code rather than by the aura itself
    enum BonusType : uint8
    {
        BONUS_NONE    = 0,
        BONUS_GOLD    = 1, // bonusValue = percent of the player's share of looted gold
        BONUS_ORE     = 2, // +1 main ore per node; bonusValue = percent chance of a second one
        BONUS_HERB    = 3, // +1 main herb per node; bonusValue = percent chance of a second one
        BONUS_LEATHER = 4  // +1 main leather per skinning; bonusValue = percent chance of a second one
    };

    inline bool IsPlayerbot(Player* player)
    {
#ifdef MOD_PLAYERBOTS
        return PlayerbotsMgr::instance().GetPlayerbotAI(player) != nullptr;
#else
        (void)player;
        return false;
#endif
    }

    // The player's companion (vanity pet), if one is out
    inline Creature* GetCompanion(Player* player)
    {
        ObjectGuid critterGuid = player->GetCritterGUID();
        return critterGuid ? ObjectAccessor::GetCreature(*player, critterGuid) : nullptr;
    }
}

#endif
