/*
 * mod-companion-auras: while a real player has a companion (vanity pet) out, they get
 * that companion's aura. The auras, which companion gives which, and the size of each
 * buff are generated from the buff table (see tools/), so they can be tuned there.
 *
 * Most buffs are plain aura effects. A few are done here in code:
 *   - Lucky:      extra gold on the player's own share of looted gold
 *   - Prospector, Green thumb, Scavenger: one extra main ore / herb / leather per gather
 *
 * Also here:
 *   - World drops: companions nothing else gives can drop, very rarely, from any creature
 *   - Addon messages (prefix "CompAuras") so client addons such as Journal can show the auras
 */

#include "CompanionAuras.h"

#include "AllCreatureScript.h"
#include "Chat.h"
#include "Config.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "DataMap.h"
#include "GameObject.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "LootScript.h"
#include "Map.h"
#include "MiscScript.h"
#include "ObjectMgr.h"
#include "PlayerScript.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "WorldPacket.h"
#include "WorldScript.h"

#include <unordered_map>
#include <unordered_set>

using namespace CompanionAuras;

namespace
{
    struct CompanionEntry
    {
        uint32 auraSpell   = 0;
        uint32 helperSpell = 0; // hidden extra aura for effects that don't fit in one spell
        uint8  bonusType   = BONUS_NONE;
        uint32 bonusValue  = 0;
        uint32 summonSpell = 0; // the companion's own summon spell
        bool   rare        = false;
        bool   worldDrop   = false; // nothing else gives it, so it is in the world drop pool
    };

    // The companion whose aura a player has right now
    struct ActiveCompanion : DataMap::Base
    {
        CompanionEntry const* entry = nullptr;
        ObjectGuid companionGuid;
    };

    bool sEnabled = true;
    bool sDisableInBattlegrounds = true;
    uint32 sWorldDropChance = 20000; // 1 in this many creature kills, 0 = off

    // Filled once at startup, read-only afterwards
    std::unordered_map<uint32, CompanionEntry> sByCreature;   // companion creature entry -> its aura
    std::unordered_set<uint32> sModuleSpells;                  // every aura this module can apply
    std::unordered_map<uint32, uint32> sMainItemByGoLoot;      // gameobject loot id -> main item
    std::unordered_map<uint32, uint32> sMainItemBySkinLoot;    // skinning loot id -> main item
    std::vector<uint32> sWorldDropItems;                       // companion items nothing else gives

    ActiveCompanion* GetActive(Player* player)
    {
        return player->CustomData.GetDefault<ActiveCompanion>("CompanionAurasActive");
    }

    // The companion an item teaches: the creature its summon spell summons, and that spell
    uint32 GetCompanionCreature(ItemTemplate const* proto, uint32& summonSpell)
    {
        for (auto const& itemSpell : proto->Spells)
        {
            if (itemSpell.SpellId <= 0)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(itemSpell.SpellId);
            if (!spellInfo)
                continue;

            for (SpellEffectInfo const& effect : spellInfo->GetEffects())
            {
                if (effect.Effect == SPELL_EFFECT_SUMMON && effect.MiscValue > 0)
                {
                    summonSpell = spellInfo->Id;
                    return uint32(effect.MiscValue);
                }
            }
        }

        return 0;
    }

    // Main item per loot id: the item with the highest chance, not from a reference
    void LoadMainItems(char const* table, std::unordered_map<uint32, uint32>& out)
    {
        out.clear();
        if (QueryResult result = WorldDatabase.Query("SELECT `Entry`, `Item` FROM `{}` WHERE `Reference` = 0 ORDER BY `Entry`, `Chance` DESC", table))
        {
            do
            {
                Field* fields = result->Fetch();
                out.emplace(fields[0].Get<uint32>(), fields[1].Get<uint32>()); // first per entry wins
            } while (result->NextRow());
        }
    }

    void LoadCompanions()
    {
        sByCreature.clear();
        sModuleSpells.clear();
        sWorldDropItems.clear();

        QueryResult result = WorldDatabase.Query("SELECT `item_entry`, `aura_spell`, `helper_spell`, `bonus_type`, `bonus_value`, `rare`, `obtainable` FROM `mod_companion_auras`");
        if (!result)
        {
            LOG_ERROR("module", "mod-companion-auras: table `mod_companion_auras` is empty or missing.");
            return;
        }

        do
        {
            Field* fields = result->Fetch();
            uint32 itemEntry = fields[0].Get<uint32>();

            CompanionEntry entry;
            entry.auraSpell   = fields[1].Get<uint32>();
            entry.helperSpell = fields[2].Get<uint32>();
            entry.bonusType   = fields[3].Get<uint8>();
            entry.bonusValue  = fields[4].Get<uint32>();
            entry.rare        = fields[5].Get<uint8>() != 0;
            entry.worldDrop   = fields[6].Get<uint8>() == 0;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
            if (!proto)
            {
                LOG_ERROR("module", "mod-companion-auras: item {} does not exist, skipped.", itemEntry);
                continue;
            }

            if (!sSpellMgr->GetSpellInfo(entry.auraSpell) || (entry.helperSpell && !sSpellMgr->GetSpellInfo(entry.helperSpell)))
            {
                LOG_ERROR("module", "mod-companion-auras: aura spell {} for item {} is missing from spell_dbc, skipped.", entry.auraSpell, itemEntry);
                continue;
            }

            uint32 creatureEntry = GetCompanionCreature(proto, entry.summonSpell);
            if (!creatureEntry)
            {
                LOG_ERROR("module", "mod-companion-auras: item {} ({}) summons no companion, skipped.", itemEntry, proto->Name1);
                continue;
            }

            sByCreature[creatureEntry] = entry;
            sModuleSpells.insert(entry.auraSpell);
            if (entry.helperSpell)
                sModuleSpells.insert(entry.helperSpell);
            if (entry.worldDrop)
                sWorldDropItems.push_back(itemEntry);
        } while (result->NextRow());

        LoadMainItems("gameobject_loot_template", sMainItemByGoLoot);
        LoadMainItems("skinning_loot_template", sMainItemBySkinLoot);

        LOG_INFO("module", "mod-companion-auras: loaded {} companions, {} of them world drops.", sByCreature.size(), sWorldDropItems.size());
    }

    bool AurasAllowedHere(Player* player)
    {
        return !(sDisableInBattlegrounds && player->GetMap() && player->GetMap()->IsBattlegroundOrArena());
    }

    void RemoveCompanionAuras(Player* player)
    {
        for (uint32 spellId : sModuleSpells)
            player->RemoveAurasDueToSpell(spellId);

        ActiveCompanion* active = GetActive(player);
        active->entry = nullptr;
        active->companionGuid.Clear();
    }

    void ApplyCompanionAura(Player* player, Creature* companion)
    {
        RemoveCompanionAuras(player);

        if (!sEnabled || IsPlayerbot(player) || !AurasAllowedHere(player))
            return;

        auto itr = sByCreature.find(companion->GetEntry());
        if (itr == sByCreature.end())
            return;

        CompanionEntry const& entry = itr->second;
        player->AddAura(entry.auraSpell, player);
        if (entry.helperSpell)
            player->AddAura(entry.helperSpell, player);

        ActiveCompanion* active = GetActive(player);
        active->entry = &entry;
        active->companionGuid = companion->GetGUID();
    }

    // The owner of a companion, if the creature is a player's current companion
    Player* GetCompanionOwner(Creature* creature)
    {
        if (!creature->IsSummon())
            return nullptr;

        Unit* summoner = creature->ToTempSummon()->GetSummonerUnit();
        Player* owner = summoner ? summoner->ToPlayer() : nullptr;
        if (!owner || owner->GetCritterGUID() != creature->GetGUID())
            return nullptr;

        return owner;
    }

    // Extra items for the gathering buffs
    void GiveGatheringBonus(Player* player, CompanionEntry const& entry, uint32 itemEntry)
    {
        uint32 count = 1;
        if (entry.bonusValue && roll_chance_i(int32(entry.bonusValue)))
            ++count;

        player->AddItem(itemEntry, count);
    }
}

void CompanionAuras::SendAddonMessage(Player* player, std::string const& message)
{
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, "CompAuras\t" + message);
    player->SendDirectMessage(&data);
}

// "A|creature,spell,flags;..." in chunks (flags: 1 rare, 2 world drop), then "E|" when done
void CompanionAuras::SendCompanionList(Player* player)
{
    std::string chunk;
    for (auto const& [creature, entry] : sByCreature)
    {
        std::string item = Acore::StringFormat("{},{},{}", creature, entry.auraSpell, (entry.rare ? 1 : 0) | (entry.worldDrop && sWorldDropChance ? 2 : 0));
        if (chunk.size() + item.size() > 200)
        {
            SendAddonMessage(player, "A|" + chunk);
            chunk.clear();
        }
        chunk += (chunk.empty() ? "" : ";") + item;
    }

    if (!chunk.empty())
        SendAddonMessage(player, "A|" + chunk);
}

uint32 CompanionAuras::GetSummonSpell(uint32 creatureEntry)
{
    auto itr = sByCreature.find(creatureEntry);
    return itr != sByCreature.end() ? itr->second.summonSpell : 0;
}

class CompanionAurasWorldScript : public WorldScript
{
public:
    CompanionAurasWorldScript() : WorldScript("CompanionAurasWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sEnabled = sConfigMgr->GetOption<bool>("CompanionAuras.Enable", true);
        sDisableInBattlegrounds = sConfigMgr->GetOption<bool>("CompanionAuras.DisableInBattlegrounds", true);
        sWorldDropChance = sConfigMgr->GetOption<uint32>("CompanionAuras.WorldDrop.Chance", 20000);
    }

    void OnStartup() override
    {
        LoadCompanions();
    }
};

class CompanionAurasCreatureScript : public AllCreatureScript
{
public:
    CompanionAurasCreatureScript() : AllCreatureScript("CompanionAurasCreatureScript") { }

    void OnCreatureAddWorld(Creature* creature) override
    {
        if (Player* owner = GetCompanionOwner(creature))
            ApplyCompanionAura(owner, creature);
    }

    void OnCreatureRemoveWorld(Creature* creature) override
    {
        if (!creature->IsSummon())
            return;

        ObjectGuid summonerGuid = creature->ToTempSummon()->GetSummonerGUID();
        if (!summonerGuid.IsPlayer())
            return;

        Player* owner = ObjectAccessor::GetPlayer(*creature, summonerGuid);
        if (owner && GetActive(owner)->companionGuid == creature->GetGUID())
            RemoveCompanionAuras(owner);
    }
};

class CompanionAurasPlayerScript : public PlayerScript
{
public:
    CompanionAurasPlayerScript() : PlayerScript("CompanionAurasPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_MAP_CHANGED,
        PLAYERHOOK_ON_LOOT_ITEM
    }) { }

    // Auras are saved with the character, but the companion is not: clear any left over
    void OnPlayerLogin(Player* player) override
    {
        RemoveCompanionAuras(player);
    }

    // Entering or leaving a battleground or arena switches the aura off or back on
    void OnPlayerMapChanged(Player* player) override
    {
        if (Creature* companion = GetCompanion(player))
            ApplyCompanionAura(player, companion);
        else
            RemoveCompanionAuras(player);
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid lootguid) override
    {
        CompanionEntry const* entry = GetActive(player)->entry;
        if (!entry || !item)
            return;

        uint8 type = entry->bonusType;
        if (type != BONUS_ORE && type != BONUS_HERB && type != BONUS_LEATHER)
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->Class != ITEM_CLASS_TRADE_GOODS)
            return;

        uint32 itemEntry = item->GetEntry();

        if (lootguid.IsGameObject() && (type == BONUS_ORE || type == BONUS_HERB))
        {
            GameObject* go = ObjectAccessor::GetGameObject(*player, lootguid);
            if (!go)
                return;

            auto itr = sMainItemByGoLoot.find(go->GetGOInfo()->GetLootId());
            if (itr == sMainItemByGoLoot.end() || itr->second != itemEntry)
                return;

            bool isOre  = proto->SubClass == ITEM_SUBCLASS_METAL_STONE;
            bool isHerb = proto->SubClass == ITEM_SUBCLASS_HERB;
            if ((type == BONUS_ORE && isOre) || (type == BONUS_HERB && isHerb))
                GiveGatheringBonus(player, *entry, itemEntry);
        }
        else if (lootguid.IsCreatureOrVehicle() && type == BONUS_LEATHER)
        {
            Creature* creature = ObjectAccessor::GetCreature(*player, lootguid);
            if (!creature || creature->loot.loot_type != LOOT_SKINNING)
                return;

            auto itr = sMainItemBySkinLoot.find(creature->GetCreatureTemplate()->SkinLootId);
            if (itr != sMainItemBySkinLoot.end() && itr->second == itemEntry && proto->SubClass == ITEM_SUBCLASS_LEATHER)
                GiveGatheringBonus(player, *entry, itemEntry);
        }
    }
};

// Lucky: extra gold on each nearby group member's own share, for those with a Lucky companion out
class CompanionAurasLootScript : public LootScript
{
public:
    CompanionAurasLootScript() : LootScript("CompanionAurasLootScript", { LOOTHOOK_ON_LOOT_MONEY }) { }

    void OnLootMoney(Player* player, uint32 gold) override
    {
        if (!gold)
            return;

        std::vector<Player*> receivers;
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member->IsInMap(player) && member->IsAtGroupRewardDistance(player))
                    receivers.push_back(member);
            }
        }

        if (receivers.empty())
            receivers.push_back(player);

        uint32 share = gold / receivers.size();
        for (Player* receiver : receivers)
        {
            CompanionEntry const* entry = GetActive(receiver)->entry;
            if (!entry || entry->bonusType != BONUS_GOLD)
                continue;

            uint32 bonus = share * entry->bonusValue / 100;
            if (bonus)
                receiver->ModifyMoney(int32(bonus));
        }
    }
};

// World drops: a very rare extra roll on creature loot for a companion nothing else gives.
// Existing loot tables are not touched; only loot from creatures that have a loot table rolls.
class CompanionAurasWorldDropScript : public MiscScript
{
public:
    CompanionAurasWorldDropScript() : MiscScript("CompanionAurasWorldDropScript", { MISCHOOK_ON_AFTER_LOOT_TEMPLATE_PROCESS }) { }

    void OnAfterLootTemplateProcess(Loot* loot, LootTemplate const* /*tab*/, LootStore const& store, Player* lootOwner,
        bool /*personal*/, bool /*noEmptyError*/, uint16 /*lootMode*/) override
    {
        if (!sWorldDropChance || sWorldDropItems.empty() || !loot || !lootOwner || &store != &LootTemplates_Creature)
            return;

        if (urand(1, sWorldDropChance) != 1 || !HasRealPlayer(lootOwner))
            return;

        uint32 itemEntry = Acore::Containers::SelectRandomContainerElement(sWorldDropItems);
        loot->AddItem(LootStoreItem(itemEntry, 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, 1, 1));
    }

private:
    // Only kills by real players (or a group with one in it) roll
    static bool HasRealPlayer(Player* lootOwner)
    {
        if (!IsPlayerbot(lootOwner))
            return true;

        if (Group* group = lootOwner->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    if (!IsPlayerbot(member))
                        return true;

        return false;
    }
};

void AddCompanionAurasScripts()
{
    new CompanionAurasWorldScript();
    new CompanionAurasCreatureScript();
    new CompanionAurasPlayerScript();
    new CompanionAurasLootScript();
    new CompanionAurasWorldDropScript();
}
