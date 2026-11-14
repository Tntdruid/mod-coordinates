# mod-coordinates

Author: Tntdruid

Coordinates for addons such as AzerothQuest, also inside dungeons and raids.

The 3.3.5 client has no world map for most instances, so `GetPlayerMapPosition("player")` returns 0,0
there and a guide arrow has nothing to work with. The server always knows where the player is. This
module hands that position to the addon, along with where the bosses of the instance stand.

[AzerothQuest](https://github.com/Tntdruid/AzerothQuest) uses it for its dungeon arrow (`/aq dungeon`):
inside a dungeon or raid an arrow points at the next boss still alive, in encounter order.

## Install

1. Clone the module into the `modules` folder of AzerothCore:
   ```bash
   cd azerothcore-wotlk/modules
   git clone https://github.com/Tntdruid/mod-coordinates.git
   ```
2. Re-run CMake and build the worldserver.
3. Copy `conf/mod_coordinates.conf.dist` to the server's `configs/modules/mod_coordinates.conf`.

No SQL is needed.

## Commands

| Command | Who | What |
| --- | --- | --- |
| `.coords` | players (or GMs, see config) | Map, zone and area, world x/y/z, zone map coordinates and a ready `waypoint(...)` line in AzerothQuest's guide format. Inside an instance it prints the entrance instead. |
| `.coords bosses` | players (or GMs) | The bosses of the current instance: done/open, position and distance. |
| `.coords export` | administrators, also from the console | Writes `coordinates_export.lua` with every dungeon and raid: entrances in zone map coordinates and bosses in world coordinates. |

## Addon protocol

The addon whispers to the player themself with the prefix `AQCOORD`; the module takes the message and
does not deliver it. Without the module the whisper just comes back, so the addon can check for `HELLO`.

```lua
SendAddonMessage("AQCOORD", "POS 250", "WHISPER", UnitName("player"))
```

| Addon sends | Server answers |
| --- | --- |
| `HELLO` | `HELLO <protocol> POS,WHERE,BOSSES` |
| `POS <ms>` | `POS` lines every `<ms>` (at least `Coordinates.MinInterval`) while in a dungeon or raid, only when the player moved or turned. `POS 0` stops. |
| `POS <ms> ALL` | The same, everywhere. |
| `WHERE` | One `POS` line now. |
| `BOSSES` | One `BOSS` line per encounter, then `BOSSEND`. |

```
POS <map> <zone> <area> <x> <y> <z> <facing> <instance 0/1> <mapX|-> <mapY|->
BOSS <encounterIndex> <done 0/1> <entry> <x|-> <y|-> <z|-> <name>
BOSSEND <map> <difficulty> <count>
```

- `x y z` are world coordinates in yards, `facing` is in radians (the same as `GetPlayerFacing()`).
- `mapX mapY` are 0-100 zone map coordinates when the zone has a world map, otherwise `-`.
- A boss is found by its encounter's credit creature, or else by a spawn with the encounter's name (Valithria
  Dreamwalker, credited by a spell). It has no position (`-`) when it is summoned during the fight (Sindragosa)
  or is no single creature (Icecrown Gunship Battle).
- The stream is forgotten at logout; send `POS` again after login or `/reload`.

To point an arrow at a boss, use the world coordinates directly: the angle to the target is
`atan2(bossY - y, bossX - x)`, compared with `facing`. In WoW world coordinates +x is north and +y is west.

## License

GNU General Public License v2.0, the same as AzerothCore. See [LICENSE](LICENSE).
