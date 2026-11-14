/*
 * mod-coordinates: positions inside dungeons and raids for addons such as AzerothQuest.
 *
 * The 3.3.5 client has no world map for most instances, so GetPlayerMapPosition returns 0,0 there and an
 * addon cannot point anywhere. The server always knows the real position, so the addon asks for it:
 *
 *   addon -> server  (SendAddonMessage("AQCOORD", body, "WHISPER", UnitName("player")))
 *     HELLO              -> HELLO <protocol> <capabilities>
 *     POS <ms> [ALL]     -> stream POS lines every <ms> while in a dungeon or raid (ALL: everywhere); 0 stops
 *     WHERE              -> one POS line now
 *     BOSSES             -> BOSS lines for the current instance, then BOSSEND
 *
 *   server -> addon  (CHAT_MSG_ADDON, prefix "AQCOORD", whispered from the player to themself)
 *     POS <map> <zone> <area> <x> <y> <z> <facing> <instance 0/1> <mapX|-> <mapY|->
 *     BOSS <encounterIndex> <done 0/1> <entry> <x|-> <y|-> <z|-> <name>
 *     BOSSEND <map> <difficulty> <count>
 *
 * x/y/z are world coordinates (yards). mapX/mapY are the 0-100 zone map coordinates when the zone has a
 * world map, the same numbers GetPlayerMapPosition gives * 100.
 */

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    bool sEnabled = true;
    bool sPlayerCommand = true;
    uint32 sMinInterval = 200;
    std::string sExportPath = "coordinates_export.lua";

    constexpr char const* ADDON_PREFIX = "AQCOORD";
    constexpr uint32 PROTOCOL_VERSION = 1;
    constexpr uint32 MAX_INTERVAL = 10000;
    constexpr char const* STREAM_KEY = "CoordinatesStream";
    // A client chat message holds 255 bytes; keep the prefix, tab and terminator inside that.
    constexpr size_t MAX_ADDON_MESSAGE = 250;

    // What the player's addon asked for. Lives in Player::CustomData, so it goes away with the player.
    struct Stream : public DataMap::Base
    {
        uint32 interval = 0;      // 0 = not streaming
        bool everywhere = false;  // also outside dungeons and raids
        uint32 timer = 0;
        uint32 mapId = UINT32_MAX;
        float x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;
    };

    struct BossSpot
    {
        uint32 encounterIndex = 0;
        std::string name;
        uint32 entry = 0;
        bool hasPosition = false;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint8 difficultyMask = 0;
    };

    struct Entrance
    {
        uint32 trigger = 0;
        uint32 mapId = 0;
        uint32 zoneId = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        bool hasMapPosition = false;
        float mapX = 0.0f, mapY = 0.0f;
    };

    LocaleConstant DbcLocale()
    {
        return sWorld->GetDefaultDbcLocale();
    }

    // DBC strings are only filled for the client locale the DBCs came from; fall back to enUS.
    std::string DbcString(char const* const* names)
    {
        char const* name = names[DbcLocale()];
        if (!name || !*name)
            name = names[LOCALE_enUS];
        return name ? name : "";
    }

    std::string AreaName(uint32 areaId)
    {
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId))
            return DbcString(area->area_name);
        return "?";
    }

    std::string MapName(uint32 mapId)
    {
        if (MapEntry const* map = sMapStore.LookupEntry(mapId))
            return DbcString(map->name);
        return "?";
    }

    // The continent index the addons use (GetCurrentMapContinent): 1 Kalimdor, 2 Eastern Kingdoms,
    // 3 Outland, 4 Northrend. The blood elf and draenei zones are map 530 but drawn on 1 and 2.
    int Continent(uint32 mapId, uint32 zoneId)
    {
        switch (GetVirtualMapForMapAndZone(mapId, zoneId))
        {
            case 1:   return 1;
            case 0:   return 2;
            case 530: return 3;
            case 571: return 4;
            default:  return 0;
        }
    }

    // World position to 0-100 zone map coordinates. False when the zone has no world map
    // (Map2ZoneCoordinates then leaves the numbers as they were).
    bool ZoneMapPosition(uint32 zoneId, float worldX, float worldY, float& mapX, float& mapY)
    {
        if (!zoneId)
            return false;
        float x = worldX;
        float y = worldY;
        Map2ZoneCoordinates(x, y, zoneId);
        if (x == worldX && y == worldY)
            return false;
        mapX = x;
        mapY = y;
        return true;
    }

    std::string LuaString(std::string const& text)
    {
        std::string out = "\"";
        for (char c : text)
        {
            if (c == '"' || c == '\\')
                out += '\\';
            if (c == '\n')
            {
                out += "\\n";
                continue;
            }
            out += c;
        }
        return out + "\"";
    }

    std::string Fixed(float value, int decimals)
    {
        std::ostringstream ss;
        ss.setf(std::ios::fixed);
        ss.precision(decimals);
        ss << value;
        return ss.str();
    }

    void SendAddon(Player* player, std::string const& body)
    {
        std::string message = std::string(ADDON_PREFIX) + "\t" + body;
        if (message.size() > MAX_ADDON_MESSAGE)
            message.resize(MAX_ADDON_MESSAGE);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
        player->SendDirectMessage(&data);
    }

    // ---- creature spawns per instance map, indexed once ----

    std::unordered_map<uint32, std::vector<ObjectGuid::LowType>> const& InstanceSpawns()
    {
        static std::unordered_map<uint32, std::vector<ObjectGuid::LowType>> spawns;
        static bool built = false;
        if (!built)
        {
            built = true;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                MapEntry const* map = sMapStore.LookupEntry(data.mapid);
                if (map && map->IsDungeon())
                    spawns[data.mapid].push_back(spawnId);
            }
        }
        return spawns;
    }

    // Does this spawn show the encounter's credit creature? The credit entry can be the heroic
    // version of a creature, which is never spawned itself (difficulty_entry_N of the normal one).
    bool SpawnMatches(CreatureData const* data, uint32 entry)
    {
        for (uint32 id : { data->id, data->id2, data->id3 })
        {
            if (!id)
                continue;
            if (id == entry)
                return true;
            if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(id))
                for (uint32 difficultyEntry : info->DifficultyEntry)
                    if (difficultyEntry == entry)
                        return true;
        }
        return false;
    }

    bool EqualsIgnoreCase(std::string const& a, std::string const& b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
        {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    }

    // Is this spawn a creature named like the encounter? For encounters credited by a spell
    // (Valithria Dreamwalker) the DBC names the boss but gives no creature.
    uint32 SpawnNamed(CreatureData const* data, std::string const& name)
    {
        for (uint32 id : { data->id, data->id2, data->id3 })
            if (id)
                if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(id))
                    if (EqualsIgnoreCase(info->Name, name))
                        return id;
        return 0;
    }

    // The spawn on this map that `matches` (returns the creature entry, 0 for no), preferring one
    // that exists in this difficulty.
    template<class Matches>
    bool FindBossSpawn(uint32 mapId, uint8 difficulty, BossSpot& info, Matches matches)
    {
        auto const& spawns = InstanceSpawns();
        auto itr = spawns.find(mapId);
        if (itr == spawns.end())
            return false;

        CreatureData const* found = nullptr;
        uint32 foundEntry = 0;
        for (ObjectGuid::LowType spawnId : itr->second)
        {
            CreatureData const* data = sObjectMgr->GetCreatureData(spawnId);
            uint32 entry = data ? matches(data) : 0;
            if (!entry)
                continue;
            bool inDifficulty = !data->spawnMask || (data->spawnMask & (1 << difficulty));
            if (!found || inDifficulty)
            {
                found = data;
                foundEntry = entry;
            }
            if (inDifficulty)
                break;
        }
        if (!found)
            return false;

        info.entry = foundEntry;
        info.hasPosition = true;
        info.x = found->posX;
        info.y = found->posY;
        info.z = found->posZ;
        return true;
    }

    // The bosses of an instance map, from DungeonEncounter.dbc and the creature spawns: the credit
    // creature, or else a spawn named like the encounter. Bosses summoned during the fight (Sindragosa)
    // and encounters that are no single creature (Icecrown Gunship Battle) have no position.
    // difficultyFilter < 0 collects every difficulty.
    std::vector<BossSpot> CollectBosses(uint32 mapId, int difficultyFilter)
    {
        std::map<uint32, BossSpot> bosses;
        for (uint8 difficulty = 0; difficulty < MAX_DIFFICULTY; ++difficulty)
        {
            if (difficultyFilter >= 0 && difficulty != difficultyFilter)
                continue;
            DungeonEncounterList const* encounters = sObjectMgr->GetDungeonEncounterList(mapId, Difficulty(difficulty));
            if (!encounters)
                continue;

            for (DungeonEncounter const* encounter : *encounters)
            {
                BossSpot& info = bosses[encounter->dbcEntry->encounterIndex];
                info.encounterIndex = encounter->dbcEntry->encounterIndex;
                info.difficultyMask |= 1 << difficulty;
                if (info.name.empty())
                    info.name = DbcString(encounter->dbcEntry->encounterName);
                if (info.hasPosition)
                    continue;

                if (encounter->creditType == ENCOUNTER_CREDIT_KILL_CREATURE)
                {
                    uint32 creditEntry = encounter->creditEntry;
                    info.entry = creditEntry;
                    if (FindBossSpawn(mapId, difficulty, info, [creditEntry](CreatureData const* data)
                        { return SpawnMatches(data, creditEntry) ? creditEntry : 0u; }))
                        continue;
                }

                // Creature names are English in the database, so match the enUS encounter name.
                char const* englishName = encounter->dbcEntry->encounterName[LOCALE_enUS];
                std::string name = (englishName && *englishName) ? englishName : info.name;
                FindBossSpawn(mapId, difficulty, info, [&name](CreatureData const* data) { return SpawnNamed(data, name); });
            }
        }

        std::vector<BossSpot> result;
        for (auto& [index, info] : bosses)
            result.push_back(std::move(info));
        return result;
    }

    // Where the instance is entered from the world: the area triggers that teleport into it.
    std::vector<Entrance> FindEntrances(uint32 instanceMapId)
    {
        std::vector<Entrance> entrances;
        for (auto const& [triggerId, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
        {
            if (teleport.target_mapId != instanceMapId)
                continue;
            AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(triggerId);
            if (!trigger)
                continue;
            MapEntry const* triggerMap = sMapStore.LookupEntry(trigger->map);
            if (!triggerMap || triggerMap->Instanceable())
                continue; // a trigger inside another instance (Scarlet Monastery wings and the like)

            Entrance entrance;
            entrance.trigger = triggerId;
            entrance.mapId = trigger->map;
            entrance.x = trigger->x;
            entrance.y = trigger->y;
            entrance.z = trigger->z;
            entrance.zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, trigger->map, trigger->x, trigger->y, trigger->z);
            if (!entrance.zoneId)
                if (MapEntry const* instanceMap = sMapStore.LookupEntry(instanceMapId))
                    entrance.zoneId = instanceMap->linked_zone;
            entrance.hasMapPosition = ZoneMapPosition(entrance.zoneId, entrance.x, entrance.y, entrance.mapX, entrance.mapY);
            entrances.push_back(entrance);
        }
        return entrances;
    }

    std::string WaypointLine(Entrance const& entrance, std::string const& text)
    {
        return "waypoint(" + std::to_string(Continent(entrance.mapId, entrance.zoneId)) + ", " +
            LuaString(AreaName(entrance.zoneId)) + ", " + LuaString(text) + ", " +
            Fixed(entrance.mapX, 2) + ", " + Fixed(entrance.mapY, 2) + ")";
    }

    // ---- addon protocol ----

    void SendPosition(Player* player)
    {
        uint32 zoneId = 0;
        uint32 areaId = 0;
        player->GetZoneAndAreaId(zoneId, areaId);

        float mapX = 0.0f;
        float mapY = 0.0f;
        bool hasMap = ZoneMapPosition(zoneId, player->GetPositionX(), player->GetPositionY(), mapX, mapY);

        std::ostringstream ss;
        ss << "POS " << player->GetMapId() << ' ' << zoneId << ' ' << areaId << ' '
           << Fixed(player->GetPositionX(), 1) << ' ' << Fixed(player->GetPositionY(), 1) << ' '
           << Fixed(player->GetPositionZ(), 1) << ' ' << Fixed(player->GetOrientation(), 3) << ' '
           << (player->GetMap()->IsDungeon() ? 1 : 0) << ' '
           << (hasMap ? Fixed(mapX, 2) : "-") << ' ' << (hasMap ? Fixed(mapY, 2) : "-");
        SendAddon(player, ss.str());
    }

    void SendBosses(Player* player)
    {
        Map* map = player->GetMap();
        if (!map->IsDungeon())
        {
            SendAddon(player, "BOSSEND " + std::to_string(map->GetId()) + " 0 0");
            return;
        }

        uint32 completed = 0;
        if (InstanceMap* instance = map->ToInstanceMap())
            if (InstanceScript* script = instance->GetInstanceScript())
                completed = script->GetCompletedEncounterMask();

        std::vector<BossSpot> bosses = CollectBosses(map->GetId(), map->GetDifficulty());
        for (BossSpot const& boss : bosses)
        {
            std::ostringstream ss;
            ss << "BOSS " << boss.encounterIndex << ' ' << ((completed & (1 << boss.encounterIndex)) ? 1 : 0) << ' '
               << boss.entry << ' '
               << (boss.hasPosition ? Fixed(boss.x, 1) : "-") << ' '
               << (boss.hasPosition ? Fixed(boss.y, 1) : "-") << ' '
               << (boss.hasPosition ? Fixed(boss.z, 1) : "-") << ' ' << boss.name;
            SendAddon(player, ss.str());
        }
        SendAddon(player, "BOSSEND " + std::to_string(map->GetId()) + ' ' + std::to_string(map->GetDifficulty()) + ' ' +
            std::to_string(bosses.size()));
    }

    void HandleAddonMessage(Player* player, std::string const& body)
    {
        std::istringstream in(body);
        std::string command;
        in >> command;

        if (command == "HELLO")
        {
            SendAddon(player, "HELLO " + std::to_string(PROTOCOL_VERSION) + " POS,WHERE,BOSSES");
        }
        else if (command == "POS")
        {
            uint32 interval = 0;
            std::string scope;
            in >> interval >> scope;

            Stream* stream = player->CustomData.GetDefault<Stream>(STREAM_KEY);
            stream->interval = interval ? std::clamp(interval, sMinInterval, MAX_INTERVAL) : 0;
            stream->everywhere = scope == "ALL";
            stream->timer = 0;          // send the first one on the next update
            stream->mapId = UINT32_MAX;
        }
        else if (command == "WHERE")
        {
            SendPosition(player);
        }
        else if (command == "BOSSES")
        {
            SendBosses(player);
        }
        else
        {
            SendAddon(player, "ERR unknown " + command);
        }
    }

    // ---- export ----

    bool WriteExport(std::string const& path, uint32& instanceCount)
    {
        std::ofstream out(path, std::ios::trunc);
        if (!out)
            return false;

        out << "-- Generated by mod-coordinates (.coords export) from the AzerothCore server data.\n"
               "-- entrances: zone map coordinates (0-100) of the area triggers that lead into the instance.\n"
               "-- bosses: world coordinates (yards) inside the instance; most 3.3.5 instances have no map,\n"
               "-- so compare them with the position mod-coordinates sends (AQCOORD POS).\n"
               "AzerothCoordinates = {\n";

        instanceCount = 0;
        for (uint32 mapId = 0; mapId < sMapStore.GetNumRows(); ++mapId)
        {
            MapEntry const* map = sMapStore.LookupEntry(mapId);
            if (!map || !map->IsDungeon())
                continue;

            std::vector<Entrance> entrances = FindEntrances(mapId);
            std::vector<BossSpot> bosses = CollectBosses(mapId, -1);
            if (entrances.empty() && bosses.empty())
                continue;
            ++instanceCount;

            out << "\t[" << mapId << "] = {name = " << LuaString(MapName(mapId)) << ", type = "
                << (map->IsRaid() ? "\"raid\"" : "\"dungeon\"") << ",\n\t\tentrances = {\n";
            for (Entrance const& entrance : entrances)
            {
                out << "\t\t\t{continent = " << Continent(entrance.mapId, entrance.zoneId) << ", mapZone = "
                    << LuaString(AreaName(entrance.zoneId)) << ", zoneId = " << entrance.zoneId;
                if (entrance.hasMapPosition)
                    out << ", x = " << Fixed(entrance.mapX, 2) << ", y = " << Fixed(entrance.mapY, 2);
                out << ", map = " << entrance.mapId << ", worldX = " << Fixed(entrance.x, 1) << ", worldY = "
                    << Fixed(entrance.y, 1) << ", worldZ = " << Fixed(entrance.z, 1) << ", trigger = "
                    << entrance.trigger << "},\n";
            }
            out << "\t\t},\n\t\tbosses = {\n";
            for (BossSpot const& boss : bosses)
            {
                out << "\t\t\t{index = " << boss.encounterIndex << ", name = " << LuaString(boss.name)
                    << ", entry = " << boss.entry << ", difficultyMask = " << uint32(boss.difficultyMask);
                if (boss.hasPosition)
                    out << ", x = " << Fixed(boss.x, 1) << ", y = " << Fixed(boss.y, 1) << ", z = " << Fixed(boss.z, 1);
                out << "},\n";
            }
            out << "\t\t},\n\t},\n";
        }
        out << "}\n";
        return bool(out);
    }
}

class CoordinatesWorld : public WorldScript
{
public:
    CoordinatesWorld() : WorldScript("CoordinatesWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sEnabled = sConfigMgr->GetOption<bool>("Coordinates.Enable", true);
        sPlayerCommand = sConfigMgr->GetOption<bool>("Coordinates.PlayerCommand", true);
        sMinInterval = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Coordinates.MinInterval", 200), 100, MAX_INTERVAL);
        sExportPath = sConfigMgr->GetOption<std::string>("Coordinates.ExportPath", "coordinates_export.lua");
    }
};

class CoordinatesPlayer : public PlayerScript
{
public:
    // Only register the hooks we use; an empty list enables every PlayerScript hook.
    CoordinatesPlayer() : PlayerScript("CoordinatesPlayer", { PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_ON_UPDATE }) {}

    // The addon whispers its requests to the player themself; take those and do not deliver them.
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 language, std::string& msg, Player* receiver) override
    {
        static std::string const prefix = std::string(ADDON_PREFIX) + "\t";
        if (language != LANG_ADDON || player != receiver || msg.rfind(prefix, 0) != 0)
            return true;

        if (sEnabled)
            HandleAddonMessage(player, msg.substr(prefix.size()));

        return false;
    }

    // Only players whose addon asked for a stream have one, so bots and everyone else cost nothing.
    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        Stream* stream = player->CustomData.Get<Stream>(STREAM_KEY);
        if (!stream || !stream->interval || !sEnabled || !player->IsInWorld())
            return;

        if (stream->timer > diff)
        {
            stream->timer -= diff;
            return;
        }
        stream->timer = stream->interval;

        if (!stream->everywhere && !player->GetMap()->IsDungeon())
        {
            stream->mapId = UINT32_MAX;
            return;
        }

        // Standing still sends nothing; the addon keeps the last position.
        bool moved = stream->mapId != player->GetMapId() ||
            std::fabs(stream->x - player->GetPositionX()) > 0.3f ||
            std::fabs(stream->y - player->GetPositionY()) > 0.3f ||
            std::fabs(stream->z - player->GetPositionZ()) > 0.3f ||
            std::fabs(stream->o - player->GetOrientation()) > 0.05f;
        if (!moved)
            return;

        stream->mapId = player->GetMapId();
        stream->x = player->GetPositionX();
        stream->y = player->GetPositionY();
        stream->z = player->GetPositionZ();
        stream->o = player->GetOrientation();
        SendPosition(player);
    }
};

using namespace Acore::ChatCommands;

class CoordinatesCommand : public CommandScript
{
public:
    CoordinatesCommand() : CommandScript("CoordinatesCommand") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable coordsTable =
        {
            { "bosses", HandleBossesCommand, SEC_PLAYER,        Console::No  },
            { "export", HandleExportCommand, SEC_ADMINISTRATOR, Console::Yes },
            { "",       HandleCoordsCommand, SEC_PLAYER,        Console::No  },
        };

        static ChatCommandTable commandTable =
        {
            { "coords", coordsTable },
        };
        return commandTable;
    }

    static bool Allowed(ChatHandler* handler)
    {
        if (sPlayerCommand || handler->GetSession()->GetSecurity() >= SEC_GAMEMASTER)
            return true;
        handler->SendSysMessage("The .coords command is for game masters on this server.");
        return false;
    }

    // `.coords`: where you stand, in world and zone map coordinates, and as an AzerothQuest waypoint line.
    static bool HandleCoordsCommand(ChatHandler* handler)
    {
        if (!Allowed(handler))
            return true;

        Player* player = handler->GetSession()->GetPlayer();
        Map* map = player->GetMap();
        uint32 zoneId = 0;
        uint32 areaId = 0;
        player->GetZoneAndAreaId(zoneId, areaId);

        handler->PSendSysMessage("Map {} {}{}, zone {} {}, area {} {}", map->GetId(), MapName(map->GetId()),
            map->IsRaid() ? " (raid, difficulty " + std::to_string(map->GetDifficulty()) + ")" :
            map->IsDungeon() ? " (dungeon, difficulty " + std::to_string(map->GetDifficulty()) + ")" : "",
            zoneId, AreaName(zoneId), areaId, AreaName(areaId));
        handler->PSendSysMessage("World: x {} y {} z {} facing {}", Fixed(player->GetPositionX(), 1),
            Fixed(player->GetPositionY(), 1), Fixed(player->GetPositionZ(), 1), Fixed(player->GetOrientation(), 2));

        float mapX = 0.0f;
        float mapY = 0.0f;
        if (ZoneMapPosition(zoneId, player->GetPositionX(), player->GetPositionY(), mapX, mapY))
        {
            handler->PSendSysMessage("Zone map: {}, {}", Fixed(mapX, 2), Fixed(mapY, 2));
            if (!map->IsDungeon())
            {
                Entrance here;
                here.mapId = map->GetId();
                here.zoneId = zoneId;
                here.mapX = mapX;
                here.mapY = mapY;
                handler->PSendSysMessage("{}", WaypointLine(here, AreaName(areaId)));
            }
        }
        else
            handler->SendSysMessage("Zone map: none - the 3.3.5 client has no map here; addons need the world position.");

        if (map->IsDungeon())
            for (Entrance const& entrance : FindEntrances(map->GetId()))
                if (entrance.hasMapPosition)
                    handler->PSendSysMessage("Entrance: {}", WaypointLine(entrance, MapName(map->GetId()) + " entrance"));

        return true;
    }

    // `.coords bosses`: the bosses of this instance, where they stand and how far away.
    static bool HandleBossesCommand(ChatHandler* handler)
    {
        if (!Allowed(handler))
            return true;

        Player* player = handler->GetSession()->GetPlayer();
        Map* map = player->GetMap();
        if (!map->IsDungeon())
        {
            handler->SendSysMessage("You are not in a dungeon or raid.");
            return true;
        }

        uint32 completed = 0;
        if (InstanceMap* instance = map->ToInstanceMap())
            if (InstanceScript* script = instance->GetInstanceScript())
                completed = script->GetCompletedEncounterMask();

        std::vector<BossSpot> bosses = CollectBosses(map->GetId(), map->GetDifficulty());
        handler->PSendSysMessage("{}: {} bosses.", MapName(map->GetId()), bosses.size());
        for (BossSpot const& boss : bosses)
        {
            std::string state = (completed & (1 << boss.encounterIndex)) ? "done" : "open";
            if (boss.hasPosition)
                handler->PSendSysMessage("  {}. {} ({}) - x {} y {} z {}, {:.0f} yards", boss.encounterIndex + 1, boss.name,
                    state, Fixed(boss.x, 1), Fixed(boss.y, 1), Fixed(boss.z, 1), player->GetDistance(boss.x, boss.y, boss.z));
            else
                handler->PSendSysMessage("  {}. {} ({}) - no spawn (summoned or scripted)", boss.encounterIndex + 1,
                    boss.name, state);
        }
        return true;
    }

    // `.coords export`: every dungeon and raid entrance and boss as a Lua table for addon build tools.
    static bool HandleExportCommand(ChatHandler* handler)
    {
        uint32 count = 0;
        if (!WriteExport(sExportPath, count))
        {
            handler->PSendSysMessage("Could not write {}.", sExportPath);
            return true;
        }
        handler->PSendSysMessage("Wrote {} dungeons and raids to {}.", count, sExportPath);
        return true;
    }
};

void AddCoordinatesScripts()
{
    new CoordinatesWorld();
    new CoordinatesPlayer();
    new CoordinatesCommand();
}
