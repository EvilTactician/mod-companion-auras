/*
 * mod-companion-auras: companion names. Real players can give their companions
 * (vanity pets) their own names, saved per character and per companion type.
 *
 *   .petname <name>   Renames the companion you have out. Same rules as hunter pet names.
 *   .petname          Without a name, puts the companion back to its normal name.
 *
 * For client addons such as Journal, as an addon message whispered to yourself with the prefix
 * "CompAuras" (answers come back the same way). A realm without this module just echoes these
 * back, so an addon can send them safely:
 *   SYNC                       the companion list ("A|"), your names ("N|") and "E|" at the end
 *   NAME|<creature>|<name>     renames any companion you know, out or not ("N|", or "F|" if refused)
 *   NAME|<creature>            puts it back to its normal name ("R|")
 * The same as chat commands: .companions sync and .companions name <creature> [name].
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
#include "ObjectMgr.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TemporarySummon.h"

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

    // Saves (or with an empty name, clears) a companion name, updates the companion if it is
    // out, and tells the player and their client addons
    void SetCompanionName(ChatHandler* handler, Player* player, uint32 entry, std::string const& name)
    {
        uint32 guid = player->GetGUID().GetCounter();
        CompanionNamesData* data = GetNames(player);
        Creature* companion = GetCompanion(player);
        if (companion && companion->GetEntry() != entry)
            companion = nullptr;

        if (name.empty())
        {
            data->names.erase(entry);
            CharacterDatabase.Execute("DELETE FROM `mod_companion_auras_names` WHERE `guid` = {} AND `entry` = {}", guid, entry);

            if (companion)
            {
                companion->SetName(companion->GetCreatureTemplate()->Name);
                companion->SetUInt32Value(UNIT_FIELD_PETNUMBER, 0);
                companion->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, 0);
            }

            SendAddonMessage(player, Acore::StringFormat("R|{}", entry));
            handler->SendSysMessage("Your companion has its normal name again.");
            return;
        }

        // Same rules as renaming a hunter pet
        if (ObjectMgr::CheckPetName(name) != PET_NAME_SUCCESS)
        {
            SendAddonMessage(player, Acore::StringFormat("F|{}", entry));
            handler->SendSysMessage("That name isn't allowed. Use one word of up to 12 letters, no spaces or numbers.");
            return;
        }

        data->names[entry] = name;
        std::string escapedName = name;
        CharacterDatabase.EscapeString(escapedName);
        CharacterDatabase.Execute("REPLACE INTO `mod_companion_auras_names` (`guid`, `entry`, `name`) VALUES ({}, {}, '{}')",
            guid, entry, escapedName);

        if (companion)
            ApplyName(companion, name);

        SendAddonMessage(player, Acore::StringFormat("N|{}|{}", entry, name));
        handler->PSendSysMessage("Your companion is now called {}.", name);
    }

    // The same for any companion the player knows, out or not (client addons, .companions name)
    void SetKnownCompanionName(ChatHandler* handler, Player* player, uint32 entry, std::string const& name)
    {
        uint32 summonSpell = GetSummonSpell(entry);
        if (!summonSpell || !player->HasSpell(summonSpell))
        {
            SendAddonMessage(player, Acore::StringFormat("F|{}", entry));
            handler->SendSysMessage("You don't have that companion.");
            return;
        }

        SetCompanionName(handler, player, entry, name);
    }

    // Everything a client addon needs: the companion list, the player's names, then "E|"
    void SendSync(Player* player)
    {
        SendCompanionList(player);
        for (auto const& [entry, name] : GetNames(player)->names)
            SendAddonMessage(player, Acore::StringFormat("N|{}|{}", entry, name));
        SendAddonMessage(player, "E|");
    }
}

class CompanionNamesPlayerScript : public PlayerScript
{
public:
    CompanionNamesPlayerScript() : PlayerScript("CompanionNamesPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_DELETE_FROM_DB,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT
    }) { }

    // Requests from client addons: an addon message the player whispers to themselves.
    // Swallowed, so it never shows up as chat.
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (lang != LANG_ADDON || receiver != player)
            return true;

        static std::string const prefix = "CompAuras\t";
        if (msg.compare(0, prefix.size(), prefix) != 0)
            return true;

        std::string request = msg.substr(prefix.size());
        if (request == "SYNC")
            SendSync(player);
        else if (request.compare(0, 5, "NAME|") == 0)
        {
            std::string rest = request.substr(5);
            size_t bar = rest.find('|');
            std::string number = rest.substr(0, bar);
            std::string name = bar == std::string::npos ? std::string() : rest.substr(bar + 1);
            if (!number.empty() && number.size() < 10 && number.find_first_not_of("0123456789") == std::string::npos)
            {
                ChatHandler handler(player->GetSession());
                SetKnownCompanionName(&handler, player, uint32(std::stoul(number)), name);
            }
        }
        return false;
    }

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
        static ChatCommandTable companionsTable =
        {
            { "sync", HandleCompanionsSyncCommand, SEC_PLAYER, Console::No },
            { "name", HandleCompanionsNameCommand, SEC_PLAYER, Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "petname",    HandlePetNameCommand, SEC_PLAYER, Console::No },
            { "companions", companionsTable }
        };

        return commandTable;
    }

    // .petname [name]: the companion you have out
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

        SetCompanionName(handler, player, companion->GetEntry(), std::string(newName));
        return true;
    }

    // .companions sync: everything a client addon needs, as addon messages
    static bool HandleCompanionsSyncCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        SendSync(player);
        return true;
    }

    // .companions name <creature> [name]: any companion the player knows, out or not
    static bool HandleCompanionsNameCommand(ChatHandler* handler, uint32 entry, Tail newName)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        SetKnownCompanionName(handler, player, entry, std::string(newName));
        return true;
    }
};

void AddCompanionNamesScripts()
{
    new CompanionNamesPlayerScript();
    new CompanionNamesCreatureScript();
    new CompanionNamesCommandScript();
}
