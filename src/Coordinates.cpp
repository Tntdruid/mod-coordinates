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
 *     POS <map> <zone> <area> <x> <y> <z> <facing> <instance 0/1> <mapX|-> <mapY|-> <worldMapAreaId|-> <floor|->
 *     BOSS <encounterIndex> <state 0 open/1 done/2 friendly to you> <entry> <x|-> <y|-> <z|-> <name>
 *     BOSSMAP <encounterIndex> <worldMapAreaId> <floor> <mapX> <mapY> <exact 0/1>   (dungeon maps loaded)
 *     BOSSEND <map> <difficulty> <count>
 *
 * x/y/z are world coordinates (yards). mapX/mapY are 0-100 map coordinates, the same numbers
 * GetPlayerMapPosition gives * 100: the zone map, or inside an instance the floor of its dungeon map when
 * the dungeon maps of the WDM patch are loaded (Coordinates.DungeonMapDir). worldMapAreaId and floor are
 * what GetCurrentMapAreaID and GetCurrentMapDungeonLevel give on that map.
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
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace
{
    bool sEnabled = true;
    bool sPlayerCommand = true;
    uint32 sMinInterval = 200;
    std::string sExportPath = "coordinates_export.lua";
    std::string sDungeonMapDir;

    constexpr char const* ADDON_PREFIX = "AQCOORD";
    constexpr uint32 PROTOCOL_VERSION = 2;
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

    // ---- dungeon maps from the WDM patch (github.com/Trimitor/WDM-patch) ----
    //
    // The patch gives the 3.3.5 client maps for the classic and TBC instances. Its DBC files say where each
    // map, and each floor of a map, lies in the world, so the server can give addons the map coordinates the
    // client shows. The server's own DBCs are not touched: the files are read from Coordinates.DungeonMapDir.

    // A map or one floor of it. A WoW map's left edge is its largest world y and its top edge its largest
    // world x: DBC "X" runs along world y.
    struct MapBox
    {
        uint32 id = 0;              // DungeonMap row, or the WorldMapArea row of a single-level map
        uint32 mapId = 0;
        uint32 floor = 0;           // GetCurrentMapDungeonLevel; 0 on a single-level map
        uint32 worldMapAreaId = 0;  // GetCurrentMapAreaID
        float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;

        bool Contains(float x, float y) const { return y <= left && y >= right && x <= top && x >= bottom; }

        void ToMap(float x, float y, float& mapX, float& mapY) const
        {
            mapX = (left - y) / (left - right) * 100.0f;
            mapY = (top - x) / (top - bottom) * 100.0f;
        }
    };

    struct FloorChunk
    {
        uint32 floorId = 0;  // DungeonMap row
        float minZ = 0.0f;
    };

    struct DungeonMaps
    {
        std::unordered_map<uint32, MapBox> singleLevel;                // instance map -> its whole map
        std::unordered_map<uint32, std::vector<MapBox>> floors;       // instance map -> its floors
        std::unordered_map<uint32, MapBox> floorById;
        std::unordered_map<uint64, std::vector<FloorChunk>> chunks;   // map << 32 | WMO group -> floors
        std::unordered_map<uint32, uint32> worldMapAreaByMap;         // instance map -> WorldMapArea
        std::unordered_map<uint32, uint32> worldMapAreaByZone;        // continent zone -> WorldMapArea
    };
    DungeonMaps sDungeonMaps;

    struct MapSpot
    {
        uint32 worldMapAreaId = 0;
        uint32 floor = 0;
        float x = 0.0f, y = 0.0f;
        bool exact = false;  // the floor came from the building part at the position, not a floor's outline
    };

    // Boss positions do not move, so a floor found from the building is kept for the server's lifetime.
    std::map<std::tuple<uint32, int32, int32, int32>, MapSpot> sBossMapCache;

    bool DungeonMapsLoaded()
    {
        return !sDungeonMaps.floors.empty() || !sDungeonMaps.singleLevel.empty();
    }

    using DbcRows = std::vector<std::vector<uint32>>;

    // A WDBC file: a header, records of 4-byte fields, then the strings (not needed here).
    bool ReadDbc(std::string const& path, uint32 fieldCount, DbcRows& rows)
    {
        std::ifstream in(path, std::ios::binary);
        char magic[4] = {};
        uint32 header[4] = {};  // records, fields, record size, string block size
        if (!in.read(magic, sizeof(magic)) || !in.read(reinterpret_cast<char*>(header), sizeof(header)))
            return false;
        if (std::memcmp(magic, "WDBC", 4) != 0 || header[1] != fieldCount || header[2] != fieldCount * 4)
            return false;

        rows.assign(header[0], std::vector<uint32>(fieldCount));
        for (std::vector<uint32>& row : rows)
            if (!in.read(reinterpret_cast<char*>(row.data()), fieldCount * 4))
                return false;
        return true;
    }

    float DbcFloat(uint32 value)
    {
        float result;
        std::memcpy(&result, &value, sizeof(result));
        return result;
    }

    bool IsContinent(uint32 mapId)
    {
        return mapId == 0 || mapId == 1 || mapId == 530 || mapId == 571;
    }

    void LoadDungeonMaps()
    {
        sDungeonMaps = DungeonMaps();
        sBossMapCache.clear();
        if (sDungeonMapDir.empty())
            return;

        std::string dir = sDungeonMapDir;
        if (dir.back() != '/' && dir.back() != '\\')
            dir += '/';

        DbcRows areas;
        DbcRows floors;
        DbcRows chunks;
        if (!ReadDbc(dir + "WorldMapArea.dbc", 11, areas) || !ReadDbc(dir + "DungeonMap.dbc", 8, floors) ||
            !ReadDbc(dir + "DungeonMapChunk.dbc", 5, chunks))
        {
            LOG_ERROR("module", "mod-coordinates: {} needs WorldMapArea.dbc, DungeonMap.dbc and DungeonMapChunk.dbc "
                "from the WDM patch (3.3.5). Dungeon maps are off.", dir);
            return;
        }

        // ID, MapID, AreaID, AreaName, LocLeft, LocRight, LocTop, LocBottom, DisplayMapID, DefaultDungeonFloor, ParentWorldMapID
        for (std::vector<uint32> const& row : areas)
        {
            uint32 mapId = row[1];
            if (IsContinent(mapId))
            {
                if (row[2])
                    sDungeonMaps.worldMapAreaByZone[row[2]] = row[0];
                continue;
            }

            sDungeonMaps.worldMapAreaByMap[mapId] = row[0];
            MapBox box;
            box.id = row[0];
            box.mapId = mapId;
            box.worldMapAreaId = row[0];
            box.left = DbcFloat(row[4]);
            box.right = DbcFloat(row[5]);
            box.top = DbcFloat(row[6]);
            box.bottom = DbcFloat(row[7]);
            if (box.left > box.right && box.top > box.bottom)
                sDungeonMaps.singleLevel[mapId] = box;
        }

        // ID, MapID, FloorIndex, MinX, MaxX, MinY, MaxY, ParentWorldMapID
        for (std::vector<uint32> const& row : floors)
        {
            MapBox box;
            box.id = row[0];
            box.mapId = row[1];
            box.floor = row[2];
            auto area = sDungeonMaps.worldMapAreaByMap.find(box.mapId);
            box.worldMapAreaId = area != sDungeonMaps.worldMapAreaByMap.end() ? area->second : 0;
            box.left = DbcFloat(row[4]);
            box.right = DbcFloat(row[3]);
            box.top = DbcFloat(row[6]);
            box.bottom = DbcFloat(row[5]);
            if (box.left <= box.right || box.top <= box.bottom)
                continue;
            sDungeonMaps.floors[box.mapId].push_back(box);
            sDungeonMaps.floorById[box.id] = box;
        }

        // ID, MapID, WMOGroupID, DungeonMapID, MinZ
        for (std::vector<uint32> const& row : chunks)
            sDungeonMaps.chunks[(uint64(row[1]) << 32) | row[2]].push_back({ row[3], DbcFloat(row[4]) });

        LOG_INFO("module", "mod-coordinates: dungeon maps from {}: {} floors of {} instances, {} single-level maps.", dir,
            sDungeonMaps.floorById.size(), sDungeonMaps.floors.size(), sDungeonMaps.singleLevel.size());
    }

    // The floor whose building part (WMO group) holds the position, from DungeonMapChunk. The position's
    // grid must be loaded; `loadGrid` loads it, which only the world thread may do (chat and commands).
    MapBox const* FloorFromBuilding(Map* map, uint32 mapId, float x, float y, float z, bool loadGrid)
    {
        if (!map || map->GetId() != mapId)
            return nullptr;
        if (!map->IsGridLoaded(x, y))
        {
            if (!loadGrid)
                return nullptr;
            map->LoadGrid(x, y);
        }

        uint32 flags = 0;
        int32 adtId = 0;
        int32 rootId = 0;
        int32 groupId = 0;
        if (!map->GetAreaInfo(PHASEMASK_NORMAL, x, y, z + 1.0f, flags, adtId, rootId, groupId))
            return nullptr;

        auto chunkItr = sDungeonMaps.chunks.find((uint64(mapId) << 32) | uint32(groupId));
        if (chunkItr == sDungeonMaps.chunks.end())
            return nullptr;

        // A group can lie on several floors split by height: the highest floor starting below the position.
        FloorChunk const* best = nullptr;
        for (FloorChunk const& chunk : chunkItr->second)
            if (chunk.minZ <= z + 1.0f && (!best || chunk.minZ > best->minZ))
                best = &chunk;
        if (!best)
            best = &chunkItr->second.front();

        auto floor = sDungeonMaps.floorById.find(best->floorId);
        return floor != sDungeonMaps.floorById.end() && floor->second.mapId == mapId ? &floor->second : nullptr;
    }

    // Where a world position lies on the instance's map, as a client with the WDM patch shows it. The floor
    // comes from the building part at the position; when that is unknown (grid not loaded, no vmaps, a part
    // the patch does not list) it is the first floor whose outline holds the position.
    bool InstanceMapPosition(Map* map, uint32 mapId, float x, float y, float z, bool loadGrid, MapSpot& spot)
    {
        auto floors = sDungeonMaps.floors.find(mapId);
        if (floors == sDungeonMaps.floors.end())
        {
            auto single = sDungeonMaps.singleLevel.find(mapId);
            if (single == sDungeonMaps.singleLevel.end() || !single->second.Contains(x, y))
                return false;
            spot.worldMapAreaId = single->second.worldMapAreaId;
            spot.floor = 0;
            spot.exact = true;
            single->second.ToMap(x, y, spot.x, spot.y);
            return true;
        }

        MapBox const* chosen = FloorFromBuilding(map, mapId, x, y, z, loadGrid);
        spot.exact = chosen != nullptr;
        if (!chosen)
            for (MapBox const& box : floors->second)
                if (box.Contains(x, y) && (!chosen || box.floor < chosen->floor))
                    chosen = &box;
        if (!chosen)
            return false;

        spot.worldMapAreaId = chosen->worldMapAreaId;
        spot.floor = chosen->floor;
        chosen->ToMap(x, y, spot.x, spot.y);
        return true;
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

    // A boss whose faction is friendly to the player cannot be fought by them (the Lost Dwarves in Uldaman
    // for the Alliance), so its encounter never ends for them and an addon should pass it by.
    bool FriendlyToPlayer(Player* player, BossSpot const& boss)
    {
        CreatureTemplate const* info = boss.entry ? sObjectMgr->GetCreatureTemplate(boss.entry) : nullptr;
        FactionTemplateEntry const* bossFaction = info ? sFactionTemplateStore.LookupEntry(info->faction) : nullptr;
        FactionTemplateEntry const* playerFaction = player->GetFactionTemplateEntry();
        return bossFaction && playerFaction && bossFaction->IsFriendlyTo(*playerFaction);
    }

    // 0 open, 1 done, 2 friendly to this player (BOSS lines; protocol 1 addons read only "1" as done).
    int BossState(Player* player, uint32 completed, BossSpot const& boss)
    {
        if (completed & (1 << boss.encounterIndex))
            return 1;
        return FriendlyToPlayer(player, boss) ? 2 : 0;
    }

    // A boss on the instance's dungeon map. Without `map` (the export) only floors found earlier in an
    // instance are exact; the others come from the floors' outlines.
    bool BossMapPosition(Map* map, uint32 mapId, BossSpot const& boss, bool loadGrid, MapSpot& spot)
    {
        if (!boss.hasPosition || !DungeonMapsLoaded())
            return false;

        auto key = std::make_tuple(mapId, int32(std::lround(boss.x * 10.0f)), int32(std::lround(boss.y * 10.0f)),
            int32(std::lround(boss.z * 10.0f)));
        auto cached = sBossMapCache.find(key);
        if (cached != sBossMapCache.end())
        {
            spot = cached->second;
            return true;
        }

        if (!InstanceMapPosition(map, mapId, boss.x, boss.y, boss.z, loadGrid, spot))
            return false;
        if (spot.exact)
            sBossMapCache[key] = spot;
        return true;
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

        // Runs in the map's own update thread (the stream), so it never loads a grid: the player's is loaded.
        Map* map = player->GetMap();
        MapSpot spot;
        bool hasMap = false;
        std::string worldMapArea = "-";
        std::string floor = "-";
        if (map->IsDungeon() && InstanceMapPosition(map, map->GetId(), player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ(), false, spot))
        {
            hasMap = true;
            worldMapArea = std::to_string(spot.worldMapAreaId);
            floor = std::to_string(spot.floor);
        }
        else
        {
            hasMap = ZoneMapPosition(zoneId, player->GetPositionX(), player->GetPositionY(), spot.x, spot.y);
            auto area = sDungeonMaps.worldMapAreaByZone.find(zoneId);
            if (hasMap && area != sDungeonMaps.worldMapAreaByZone.end())
            {
                worldMapArea = std::to_string(area->second);
                floor = "0";
            }
        }

        std::ostringstream ss;
        ss << "POS " << player->GetMapId() << ' ' << zoneId << ' ' << areaId << ' '
           << Fixed(player->GetPositionX(), 1) << ' ' << Fixed(player->GetPositionY(), 1) << ' '
           << Fixed(player->GetPositionZ(), 1) << ' ' << Fixed(player->GetOrientation(), 3) << ' '
           << (map->IsDungeon() ? 1 : 0) << ' '
           << (hasMap ? Fixed(spot.x, 2) : "-") << ' ' << (hasMap ? Fixed(spot.y, 2) : "-") << ' '
           << worldMapArea << ' ' << floor;
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
            ss << "BOSS " << boss.encounterIndex << ' ' << BossState(player, completed, boss) << ' '
               << boss.entry << ' '
               << (boss.hasPosition ? Fixed(boss.x, 1) : "-") << ' '
               << (boss.hasPosition ? Fixed(boss.y, 1) : "-") << ' '
               << (boss.hasPosition ? Fixed(boss.z, 1) : "-") << ' ' << boss.name;
            SendAddon(player, ss.str());

            // A line of its own, so addons that read BOSS up to the name keep working.
            MapSpot spot;
            if (BossMapPosition(map, map->GetId(), boss, true, spot))
                SendAddon(player, "BOSSMAP " + std::to_string(boss.encounterIndex) + ' ' + std::to_string(spot.worldMapAreaId) +
                    ' ' + std::to_string(spot.floor) + ' ' + Fixed(spot.x, 2) + ' ' + Fixed(spot.y, 2) + ' ' +
                    (spot.exact ? "1" : "0"));
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
            SendAddon(player, "HELLO " + std::to_string(PROTOCOL_VERSION) + " POS,WHERE,BOSSES" +
                (DungeonMapsLoaded() ? ",MAPS" : ""));
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
               "-- bosses: world coordinates (yards) inside the instance. With the WDM patch's dungeon maps loaded\n"
               "-- (Coordinates.DungeonMapDir) also mapX/mapY on worldMapAreaId (GetCurrentMapAreaID) and floor\n"
               "-- (GetCurrentMapDungeonLevel). exactFloor = false: the floor is a guess from the floors' outlines,\n"
               "-- because no player has been near the boss since the server started; .coords bosses there makes it exact.\n"
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
                MapSpot spot;
                if (BossMapPosition(nullptr, mapId, boss, false, spot))
                    out << ", worldMapAreaId = " << spot.worldMapAreaId << ", floor = " << spot.floor << ", mapX = "
                        << Fixed(spot.x, 2) << ", mapY = " << Fixed(spot.y, 2) << ", exactFloor = "
                        << (spot.exact ? "true" : "false");
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
        sDungeonMapDir = sConfigMgr->GetOption<std::string>("Coordinates.DungeonMapDir", "");
        LoadDungeonMaps();
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

        MapSpot spot;
        if (map->IsDungeon() && InstanceMapPosition(map, map->GetId(), player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ(), true, spot))
        {
            handler->PSendSysMessage("Dungeon map {}{}: {}, {} ({})", spot.worldMapAreaId,
                spot.floor ? ", floor " + std::to_string(spot.floor) : "", Fixed(spot.x, 2), Fixed(spot.y, 2),
                !spot.floor ? "single-level map" : spot.exact ? "floor from the building" : "floor guessed from the outline");
            handler->PSendSysMessage("{{worldMapAreaId = {}, floor = {}, x = {}, y = {}}}", spot.worldMapAreaId, spot.floor,
                Fixed(spot.x, 2), Fixed(spot.y, 2));
        }
        else if (map->IsDungeon() && !DungeonMapsLoaded())
            handler->SendSysMessage("Dungeon map: not loaded - set Coordinates.DungeonMapDir to the WDM patch's DBC files.");

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
        else if (!map->IsDungeon() || !DungeonMapsLoaded())
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
            int bossState = BossState(player, completed, boss);
            std::string state = bossState == 1 ? "done" : bossState == 2 ? "friendly to you" : "open";
            MapSpot spot;
            std::string onMap;
            if (BossMapPosition(map, map->GetId(), boss, true, spot))
                onMap = " - map " + Fixed(spot.x, 1) + ", " + Fixed(spot.y, 1) +
                    (spot.floor ? " floor " + std::to_string(spot.floor) + (spot.exact ? "" : "?") : "");
            if (boss.hasPosition)
                handler->PSendSysMessage("  {}. {} ({}) - x {} y {} z {}, {:.0f} yards{}", boss.encounterIndex + 1, boss.name,
                    state, Fixed(boss.x, 1), Fixed(boss.y, 1), Fixed(boss.z, 1), player->GetDistance(boss.x, boss.y, boss.z), onMap);
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
