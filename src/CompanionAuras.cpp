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
 *   - Bringing your companion back after a loading screen, hearthstone, flight, death or
 *     login, unless you dismissed it yourself
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

    // A player's companion state: the aura they have now, and the companion to bring back
    struct ActiveCompanion : DataMap::Base
    {
        CompanionEntry const* entry = nullptr;
        ObjectGuid companionGuid;

        uint32 wantedSpell     = 0; // summon spell of the companion they had out, 0 = none
        uint32 resummonTimer   = 0; // ms until the next check
        uint8  resummonTries   = 0; // summon attempts since the companion went away
        uint32 followTimer     = 0; // ms until the next "is it still with me?" check
    };

    bool sEnabled = true;
    bool sDisableInBattlegrounds = true;
    bool sResummon = true;
    uint32 sWorldDropChance = 20000; // 1 in this many creature kills, 0 = off

    constexpr uint32 RESUMMON_CHECK_MS  = 2000; // how often to look again while waiting
    constexpr uint32 RESUMMON_LOGIN_MS  = 5000; // a little longer after login, for the client
    constexpr uint8  RESUMMON_MAX_TRIES = 3;    // then give up until the player summons one
    constexpr uint32 FOLLOW_CHECK_MS    = 1000; // how often to check the companion is still nearby
    constexpr float  FOLLOW_MAX_DIST    = 60.0f; // further than this: bring it to the player

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

    // The creature a spell summons, following the spells it triggers or teaches (a few levels
    // deep, as some companion items use a spell that casts or teaches the real summon spell)
    uint32 GetSummonedCreature(uint32 spellId, uint32& summonSpell, uint8 depth = 0)
    {
        SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!spellInfo || depth > 3)
            return 0;

        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        {
            if (effect.Effect == SPELL_EFFECT_SUMMON && effect.MiscValue > 0)
            {
                summonSpell = spellInfo->Id;
                return uint32(effect.MiscValue);
            }
        }

        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
            if (effect.TriggerSpell && effect.TriggerSpell != spellId)
                if (uint32 creature = GetSummonedCreature(effect.TriggerSpell, summonSpell, depth + 1))
                    return creature;

        return 0;
    }

    // The companion an item teaches: its summon spell from the table first, then the item's spells
    uint32 GetCompanionCreature(ItemTemplate const* proto, uint32 tableSpell, uint32& summonSpell)
    {
        if (uint32 creature = GetSummonedCreature(tableSpell, summonSpell))
            return creature;

        for (auto const& itemSpell : proto->Spells)
            if (itemSpell.SpellId > 0)
                if (uint32 creature = GetSummonedCreature(uint32(itemSpell.SpellId), summonSpell))
                    return creature;

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

        QueryResult result = WorldDatabase.Query("SELECT `item_entry`, `aura_spell`, `helper_spell`, `bonus_type`, `bonus_value`, `rare`, `obtainable`, `summon_spell` FROM `mod_companion_auras`");
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

            uint32 creatureEntry = GetCompanionCreature(proto, fields[7].Get<uint32>(), entry.summonSpell);
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

    // Saved so the companion can come back after logging in
    void SaveWantedCompanion(Player* player, uint32 spellId)
    {
        if (spellId)
            CharacterDatabase.Execute("REPLACE INTO `mod_companion_auras_resummon` (`guid`, `spell`) VALUES ({}, {})", player->GetGUID().GetCounter(), spellId);
        else
            CharacterDatabase.Execute("DELETE FROM `mod_companion_auras_resummon` WHERE `guid` = {}", player->GetGUID().GetCounter());
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

        if (IsPlayerbot(player))
            return;

        auto itr = sByCreature.find(companion->GetEntry());
        if (itr == sByCreature.end())
            return;

        // Keep track of the companion even where it gives no aura (a battleground, or auras
        // switched off), so it still comes back after the next loading screen
        ActiveCompanion* active = GetActive(player);
        active->companionGuid = companion->GetGUID();

        if (!sEnabled || !AurasAllowedHere(player))
            return;

        CompanionEntry const& entry = itr->second;
        player->AddAura(entry.auraSpell, player);
        if (entry.helperSpell)
            player->AddAura(entry.helperSpell, player);
        active->entry = &entry;
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

    // Remember a companion the player summoned, so it can come back later
    void RememberCompanion(Player* player, Creature* companion)
    {
        // Only companions on the module's list: not quest critters or a Children's Week orphan
        if (!sResummon || IsPlayerbot(player) || !sByCreature.count(companion->GetEntry()))
            return;

        uint32 spellId = companion->GetUInt32Value(UNIT_CREATED_BY_SPELL);
        ActiveCompanion* active = GetActive(player);
        active->resummonTries = 0;
        if (spellId && spellId != active->wantedSpell)
        {
            active->wantedSpell = spellId;
            SaveWantedCompanion(player, spellId);
        }
    }

    // Whether the companion went away because of a loading screen, flight, death or logout
    // (bring it back) rather than the player dismissing it (leave it).
    // IsDuringRemoveFromWorld: a map change that isn't a spell (a dungeon or raid portal, joining
    // a battleground or a dungeon finder group, a boat or zeppelin) takes the player off the old map
    // before it marks them as teleporting, and the companion is removed during that.
    bool WentAwayOnItsOwn(Player* owner)
    {
        return owner->IsBeingTeleported() || owner->IsDuringRemoveFromWorld() || owner->IsInFlight() || !owner->IsAlive() || !owner->IsInWorld()
            || (owner->GetSession() && owner->GetSession()->PlayerLogout());
    }

    // Safe moment to summon: no companion out, nothing that a summon would break or that blocks it
    bool CanResummonNow(Player* player)
    {
        return player->IsInWorld() && !player->IsBeingTeleported() && player->IsAlive() && !player->IsInFlight()
            && !player->IsInCombat() && !player->GetCritterGUID() && !player->GetVehicle()
            && !player->IsFalling() && !player->GetTransport()                 // landed, not on a boat or zeppelin
            && !player->GetCharmGUID() && !player->GetCharmerGUID()            // not controlling or controlled
            && !player->HasStealthAura() && !player->HasInvisibilityAura()
            && !player->HasUnitState(UNIT_STATE_CASTING) && !player->IsNonMeleeSpellCast(false);
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
        sResummon = sConfigMgr->GetOption<bool>("CompanionAuras.Resummon", true);
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
        {
            ApplyCompanionAura(owner, creature);
            RememberCompanion(owner, creature);
        }
    }

    void OnCreatureRemoveWorld(Creature* creature) override
    {
        if (!creature->IsSummon())
            return;

        ObjectGuid summonerGuid = creature->ToTempSummon()->GetSummonerGUID();
        if (!summonerGuid.IsPlayer())
            return;

        Player* owner = ObjectAccessor::GetPlayer(*creature, summonerGuid);
        if (!owner)
            return;

        ActiveCompanion* active = GetActive(owner);
        if (active->companionGuid != creature->GetGUID())
            return; // an older companion, already replaced by a new one

        RemoveCompanionAuras(owner);

        // Dismissed by the player: don't bring it back. Gone on its own: check again soon.
        if (WentAwayOnItsOwn(owner))
            active->resummonTimer = RESUMMON_CHECK_MS;
        else if (active->wantedSpell)
        {
            active->wantedSpell = 0;
            SaveWantedCompanion(owner, 0);
        }
    }
};

class CompanionAurasPlayerScript : public PlayerScript
{
public:
    CompanionAurasPlayerScript() : PlayerScript("CompanionAurasPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_MAP_CHANGED,
        PLAYERHOOK_ON_LOOT_ITEM,
        PLAYERHOOK_ON_UPDATE,
        PLAYERHOOK_ON_DELETE_FROM_DB
    }) { }

    // Auras are saved with the character, but the companion is not: clear any left over, and
    // bring back the companion the player had out when they logged out
    void OnPlayerLogin(Player* player) override
    {
        RemoveCompanionAuras(player);

        if (!sResummon || IsPlayerbot(player))
            return;

        ActiveCompanion* active = GetActive(player);
        if (QueryResult result = CharacterDatabase.Query("SELECT `spell` FROM `mod_companion_auras_resummon` WHERE `guid` = {}", player->GetGUID().GetCounter()))
        {
            active->wantedSpell = result->Fetch()[0].Get<uint32>();
            active->resummonTries = 0;
            active->resummonTimer = RESUMMON_LOGIN_MS;
        }
    }

    // Entering or leaving a battleground or arena switches the aura off or back on
    void OnPlayerMapChanged(Player* player) override
    {
        if (Creature* companion = GetCompanion(player))
        {
            ApplyCompanionAura(player, companion);
            return;
        }

        // Still marked as out: the companion was only removed after the player had left the old
        // map (a boat or zeppelin despawns it as it changes map), so the module never saw it go.
        // It went with the map change, so bring it back.
        ActiveCompanion* active = player->CustomData.Get<ActiveCompanion>("CompanionAurasActive");
        bool lostOnTheWay = active && !active->companionGuid.IsEmpty();

        RemoveCompanionAuras(player);

        if (lostOnTheWay && sResummon && active->wantedSpell && !IsPlayerbot(player))
            active->resummonTimer = RESUMMON_CHECK_MS;
    }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM `mod_companion_auras_resummon` WHERE `guid` = {}", guid);
    }

    // Only does anything for a player waiting to get their companion back
    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        ActiveCompanion* active = player->CustomData.Get<ActiveCompanion>("CompanionAurasActive");
        if (!active)
            return;

        // A teleport within the same map (a hearthstone or portal on the same continent, Moonglade,
        // Dalaran's portals) doesn't take the companion along: it would stay behind, out of sight.
        // Bring it to the player, the way the core does for hunter pets. Not on boats, zeppelins or
        // vehicles, where the companion can't stand with the player anyway.
        if (active->companionGuid)
        {
            if (active->followTimer > diff)
                active->followTimer -= diff;
            else
            {
                active->followTimer = FOLLOW_CHECK_MS;
                if (player->IsInWorld() && !player->IsBeingTeleported() && !player->IsInFlight()
                    && !player->GetTransport() && !player->GetVehicle())
                    if (Creature* companion = GetCompanion(player))
                        if (companion->IsInWorld() && companion->GetMap() == player->GetMap() &&
                            !companion->IsWithinDistInMap(player, FOLLOW_MAX_DIST))
                            companion->NearTeleportTo(player->GetPositionX(), player->GetPositionY(),
                                player->GetPositionZ(), companion->GetOrientation());
            }
        }

        if (!active->resummonTimer)
            return;

        if (active->resummonTimer > diff)
        {
            active->resummonTimer -= diff;
            return;
        }
        active->resummonTimer = 0;

        // Nothing to do: switched off, nothing wanted, or a companion is out already
        if (!sResummon || !active->wantedSpell || player->GetCritterGUID())
        {
            // Safety: an aura without its companion should never stay
            if (active->entry && !GetCompanion(player))
                RemoveCompanionAuras(player);
            return;
        }

        if (!player->HasSpell(active->wantedSpell))
        {
            active->wantedSpell = 0;
            SaveWantedCompanion(player, 0);
            return;
        }

        // Not a good moment (combat, stealth, a flight, dead, casting...): wait and look again.
        // This doesn't count as a try, so a long flight or fight is fine.
        if (!CanResummonNow(player))
        {
            active->resummonTimer = RESUMMON_CHECK_MS;
            return;
        }

        // A few tries, then give up until the player summons a companion again
        if (active->resummonTries >= RESUMMON_MAX_TRIES)
            return;

        ++active->resummonTries;
        player->CastSpell(player, active->wantedSpell, true);
        active->resummonTimer = RESUMMON_CHECK_MS; // look again: if it didn't come out, try again
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
