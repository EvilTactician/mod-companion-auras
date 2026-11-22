/*
 * mod-companion-auras: companion names. Real players can give their companions
 * (vanity pets) their own names, saved per character and per companion type.
 *
 *   .petname <name>   Renames the companion you have out. Same rules as hunter pet names.
 *   .petname          Without a name, puts the companion back to its normal name.
 *
 * A renamed companion gets a pet number, so clients ask the server for its name
 * (exactly as they do for hunter pets) and everyone nearby sees the new name.
 * Companions that were never renamed are left completely alone.
 */

#include "CompanionAuras.h"

#include "AllCreatureScript.h"
#include "Chat.h"
#include "CommandScript.h"
#include "DatabaseEnv.h"
#include "DataMap.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "TemporarySummon.h"
#include "WorldScript.h"

#include <unordered_map>

using namespace Acore::ChatCommands;
using namespace CompanionAuras;

namespace
{
    // Saved companion names of one character: creature entry -> name
    struct CompanionNamesData : DataMap::Base
    {
        std::unordered_map<uint32, std::string> names;
    };

    CompanionNamesData* GetNames(Player* player)
    {
        return player->CustomData.GetDefault<CompanionNamesData>("CompanionAurasNames");
    }

    // Gives the companion a custom name; the new timestamp makes clients ask for the name again
    void ApplyName(Creature* companion, std::string const& name)
    {
        companion->SetName(name);
        if (!companion->GetUInt32Value(UNIT_FIELD_PETNUMBER))
            companion->SetUInt32Value(UNIT_FIELD_PETNUMBER, sObjectMgr->GeneratePetNumber());
        companion->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, uint32(GameTime::GetGameTime().count()));
    }
}

// Names saved by the earlier version in mod-racial-tweaks move over once
class CompanionNamesWorldScript : public WorldScript
{
public:
    CompanionNamesWorldScript() : WorldScript("CompanionNamesWorldScript", { WORLDHOOK_ON_STARTUP }) { }

    void OnStartup() override
    {
        if (!CharacterDatabase.Query("SHOW TABLES LIKE 'mod_racial_tweaks_companion_names'"))
            return;

        CharacterDatabase.DirectExecute("INSERT IGNORE INTO `mod_companion_auras_names` (`guid`, `entry`, `name`) "
            "SELECT `guid`, `entry`, `name` FROM `mod_racial_tweaks_companion_names`");
        LOG_INFO("module", "mod-companion-auras: copied companion names from mod_racial_tweaks_companion_names.");
    }
};

class CompanionNamesPlayerScript : public PlayerScript
{
public:
    CompanionNamesPlayerScript() : PlayerScript("CompanionNamesPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_DELETE_FROM_DB
    }) { }

    // Loads the character's saved names once per login (real players only)
    void OnPlayerLogin(Player* player) override
    {
        if (IsPlayerbot(player))
            return;

        CompanionNamesData* data = GetNames(player);
        data->names.clear();

        if (QueryResult result = CharacterDatabase.Query("SELECT `entry`, `name` FROM `mod_companion_auras_names` WHERE `guid` = {}", player->GetGUID().GetCounter()))
        {
            do
            {
                Field* fields = result->Fetch();
                data->names[fields[0].Get<uint32>()] = fields[1].Get<std::string>();
            } while (result->NextRow());
        }
    }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM `mod_companion_auras_names` WHERE `guid` = {}", guid);
    }
};

class CompanionNamesCreatureScript : public AllCreatureScript
{
public:
    CompanionNamesCreatureScript() : AllCreatureScript("CompanionNamesCreatureScript") { }

    // Names a player's companion as it is summoned, if that companion type has a saved name
    void OnCreatureAddWorld(Creature* creature) override
    {
        if (!creature->IsSummon())
            return;

        Unit* summoner = creature->ToTempSummon()->GetSummonerUnit();
        Player* owner = summoner ? summoner->ToPlayer() : nullptr;
        if (!owner || owner->GetCritterGUID() != creature->GetGUID())
            return;

        CompanionNamesData* data = GetNames(owner);
        auto itr = data->names.find(creature->GetEntry());
        if (itr != data->names.end())
            ApplyName(creature, itr->second);
    }
};

class CompanionNamesCommandScript : public CommandScript
{
public:
    CompanionNamesCommandScript() : CommandScript("CompanionNamesCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "petname", HandlePetNameCommand, SEC_PLAYER, Console::No }
        };

        return commandTable;
    }

    static bool HandlePetNameCommand(ChatHandler* handler, Tail newName)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        Creature* companion = GetCompanion(player);
        if (!companion)
        {
            handler->SendSysMessage("Summon a companion first.");
            return true;
        }

        uint32 entry = companion->GetEntry();
        uint32 guid = player->GetGUID().GetCounter();
        CompanionNamesData* data = GetNames(player);
        std::string name(newName);

        // No name: back to the normal name
        if (name.empty())
        {
            data->names.erase(entry);
            CharacterDatabase.Execute("DELETE FROM `mod_companion_auras_names` WHERE `guid` = {} AND `entry` = {}", guid, entry);

            companion->SetName(companion->GetCreatureTemplate()->Name);
            companion->SetUInt32Value(UNIT_FIELD_PETNUMBER, 0);
            companion->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, 0);
            handler->SendSysMessage("Your companion has its normal name again.");
            return true;
        }

        // Same rules as renaming a hunter pet
        if (ObjectMgr::CheckPetName(name) != PET_NAME_SUCCESS)
        {
            handler->SendSysMessage("That name isn't allowed. Use one word of up to 12 letters, no spaces or numbers.");
            return true;
        }

        data->names[entry] = name;
        CharacterDatabase.Execute("REPLACE INTO `mod_companion_auras_names` (`guid`, `entry`, `name`) VALUES ({}, {}, '{}')",
            guid, entry, CharacterDatabase.EscapeString(name));

        ApplyName(companion, name);
        handler->PSendSysMessage("Your companion is now called {}.", name);
        return true;
    }
};

void AddCompanionNamesScripts()
{
    new CompanionNamesWorldScript();
    new CompanionNamesPlayerScript();
    new CompanionNamesCreatureScript();
    new CompanionNamesCommandScript();
}
