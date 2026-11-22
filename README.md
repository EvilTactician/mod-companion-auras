# Companion Auras 0.1.0

<div align="center">

![Interface Version](https://img.shields.io/badge/Interface-30300-blue)
![WoW Version](https://img.shields.io/badge/WoW-3.3.5a-orange)
[![Version](https://img.shields.io/badge/Version-0.1.0-green)](https://github.com/EvilTactician/mod-companion-auras)
[![License](https://img.shields.io/badge/License-MIT-yellow)](LICENSE)

**An AzerothCore module which gives vanity companions specific auras (buffs) they provide to their owner, making each companion special and worth having in their own way.**

</div>

## What it does

- **Companion auras.** While you have a companion (vanity pet) out, you get its aura, shown on your buff bar with its own icon and tooltip. Dismiss the companion and the aura goes. Only one companion is out at a time, so only one aura is active at a time.
- **Common and rare.** Rare companions (raid and rare drops, Collector's Edition, trading card and promotional pets) give a slightly stronger version of their group's buff.
- **Item tooltips.** Every companion item says what it gives ("Companion: ... while summoned."), so you know what you are rolling on, buying or being rewarded with.
- **Companion names.** `.petname <name>` renames the companion you have out; `.petname` on its own gives it back its normal name. Names are saved per character and per companion, and everyone nearby sees them.
- **Real players only.** Playerbots never get companion auras.
- **Battlegrounds and arenas.** Companion auras switch off inside them by default (a `.conf` setting) and come back when you leave.

## The buffs

Each companion belongs to a group. Combat buffs are small but noticeable; comfort buffs help outside combat.

| Group | Buff | Common | Rare |
|---|---|---|---|
| Wise | Mana and energy regeneration, rage gained, rune regeneration | +5% | +6% |
| Feline | Critical strike chance | +2% | +3% |
| Venomous | Nature damage done | +3% | +4% |
| Fiery | Fire damage done | +3% | +4% |
| Frosty | Frost damage done | +3% | +4% |
| Shadowy | Shadow damage done | +3% | +4% |
| Predator | Physical damage done | +3% | +4% |
| Sturdy | Stamina | +3% | +4% |
| Dragonkin | Damage taken | -2% | -3% |
| Soothing | Healing done / threat generated | +3% / -5% | +4% / -6% |
| Aquatic | Swim speed | +25% | +30% |
| Feathered | Falling damage, as if you fell 20 yards less | same | same |
| Prospector | Extra main ore per mining vein | +1 | +1, 25% chance of +2 |
| Green thumb | Extra main herb per herb node | +1 | +1, 25% chance of +2 |
| Scavenger | Extra main leather per skinning | +1 | +1, 25% chance of +2 |
| Mechanical | Engineering skill | +10 | +12 |
| Champion | Reputation gained | +5% | +6% |
| Lucky | Your share of looted gold | +5% | +6% |

The full list of which companion is in which group, and which are rare, is in [`tools/companions.csv`](tools/companions.csv).

Notes:
- **Wise** helps every class: mana and energy users regenerate faster, rage users gain more rage from dealing damage, and death knights' runes come back faster.
- **Gathering buffs** only add the node's main item (the ore, herb or leather), never gems, stones or rare extras.
- **Lucky** adds to your own share of gold looted from corpses and chests, in a group too. Quest rewards and vendor sales are not affected.

## Installation

### Server

1. Clone the module into your AzerothCore `modules` folder (the folder must be called `mod-companion-auras`).
2. Re-run CMake and rebuild the server.
3. Copy `conf/mod_companion_auras.conf.dist` to your config folder as `mod_companion_auras.conf` and change it if you like.
4. Start the server. The module's SQL (the aura spells, the companion table, the item tooltips and the names table) is applied automatically by the database updater.

If you used the earlier companion names from mod-racial-tweaks, saved names are copied over automatically on the first start.

### Client patch

The buff bar takes each aura's name, icon and tooltip from the client's `Spell.dbc`, so every client needs a small patch:

1. Get the `Spell.dbc` your client uses now. **Only one `Spell.dbc` can win on a client:** if another custom patch already ships one (for example mod-guild-levels or mod-retail-professions), use that one, so its spells are kept. Otherwise use the server's `dbc/Spell.dbc`.
2. Run the generator with it and the server's `SpellIcon.dbc`:
   ```
   python3 tools/generate.py --dbc /path/to/Spell.dbc --icons /path/to/SpellIcon.dbc --out tools/out/Spell.dbc
   ```
   Add `--mpq tools/out/patch-enUS-5.MPQ` (with `STORMLIB=/path/to/libstorm.so`) to pack it straight into a patch.
3. Put the patch in each client's `Data/enUS` folder, named so it loads after your other custom patches.
4. Delete each client's `Cache` folder once, so the new companion item tooltips show.

## Tuning the buffs

All sizes and groups come from two files:

- [`tools/groups.csv`](tools/groups.csv): each group's sizes (common and rare) and icon.
- [`tools/companions.csv`](tools/companions.csv): each companion's group and rarity.

After changing them, run `python3 tools/generate.py` to rebuild `data/sql/db-world/mod_companion_auras_world.sql`, rebuild the client patch as above, and restart the server.

## Configuration

| Setting | Default | What it does |
|---|---|---|
| `CompanionAuras.Enable` | `1` | Companion auras on or off. Companion names work either way. |
| `CompanionAuras.DisableInBattlegrounds` | `1` | No companion auras in battlegrounds and arenas. |

## Credits

- [EvilTactician](https://github.com/EvilTactician): design and testing.
- Built for [AzerothCore](https://www.azerothcore.org/) with [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) and [mod-individual-progression](https://github.com/Grimfeather/mod-individual-progression).
