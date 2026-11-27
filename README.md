# Companion Auras 1.0.1

<div align="center">

![Interface Version](https://img.shields.io/badge/Interface-30300-blue)
![WoW Version](https://img.shields.io/badge/WoW-3.3.5a-orange)
[![Version](https://img.shields.io/badge/Version-1.0.1-green)](https://github.com/EvilTactician/mod-companion-auras)
[![License](https://img.shields.io/badge/License-MIT-yellow)](LICENSE)

**An AzerothCore module which gives vanity companions specific auras (buffs) they provide to their owner, making each companion special and worth having in their own way.**

</div>

## What it does

- **Companion auras.** While you have a companion (vanity pet) out, you get its aura, shown on your buff bar with its own icon and tooltip. Dismiss the companion and the aura goes. Only one companion is out at a time, so only one aura is active at a time.
- **Common and rare.** Rare companions (raid and rare drops, Collector's Edition, trading card and promotional pets) give a slightly stronger version of their group's buff.
- **Tooltips.** Every companion item says what it gives ("Companion: ... while summoned."), so you know what you are rolling on, buying or being rewarded with. With the client patch, each companion's own spell (action bar, pet list) says it too: "Summons and dismisses your Black Tabby Cat. Critical strike chance increased by 2% while summoned."
- **Your companion comes back.** In 3.3.5 a companion goes away on every loading screen. The module brings back the companion you had out after a teleport, hearthstone, flight, death or logging in, when it is safe (out of combat, not stealthed). If you dismissed it yourself, it stays away. Dungeon and raid portals and battlegrounds count as loading screens too. A teleport on the same continent (a hearthstone, a portal) leaves your companion behind in 3.3.5, so the module brings it to you. After a boat or zeppelin to another continent you summon it again yourself, this is unfortunately a limitation I couldn't find a clean solution for and this is how it works normally in the game. Nearly every other situation is handled, so you shouldn't have to re-summon companions nearly as often as you would without this module.
- **Companion names.** `.petname <name>` renames the companion you have out; `.petname` on its own gives it back its normal name. Names are saved per character and per companion, and everyone nearby sees them.
- **Real players only.** Playerbots never get companion auras.
- **Battlegrounds and arenas.** Companion auras switch off inside them by default (a `.conf` setting) and come back when you leave.
- **World drops.** Companions nothing else on the realm gives (promotional, event and unreleased pets) can drop from any creature, very rarely. Existing loot tables are left alone: it is an extra roll of its own, 1 in 20,000 kills by default.
- **Journal (optional).** With [My Journal fork](https://github.com/EvilTactician/wow-mod-forever-journal), the Pets tab shows the buff each companion gives, and you can rename your companions there instead of typing `.petname`.

## Screenshots
Screenshot includes my optional DragonUI fork.
<img width="1428" height="978" alt="image" src="https://github.com/user-attachments/assets/931bfa0b-d444-42c0-bfcc-359165840cf3" />
<details>
<summary><strong>See more screenshots (click to expand)</strong></summary>
<img width="1422" height="978" alt="image" src="https://github.com/user-attachments/assets/33c525c6-bf68-40f9-bbc4-b9fdbec9c92b" />
<img width="485" height="194" alt="image" src="https://github.com/user-attachments/assets/3af105fa-08d3-4e06-b147-1205199e14af" />
</details>


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

### Journal (optional)

[My Journal fork](https://github.com/EvilTactician/wow-mod-forever-journal) (a client addon with its own server module) works with this module: its Pets tab shows each companion's buff and lets you rename companions. Install it as its README describes. Companion Auras works without it.

### Client patch

The buff bar takes each aura's name, icon and tooltip from the client's `Spell.dbc`, so every client needs a small patch:

1. Get the `Spell.dbc` and `SpellIcon.dbc` your client uses now. **Only one `Spell.dbc` can win on a client:** if another custom patch already ships one (for example mod-guild-levels or mod-retail-professions), use that one, so its spells are kept. For an English client they are most likely in `Data/enUS/patch-enUS-3.MPQ` (the last Blizzard patch), or in `Data/enUS/patch-enUS-4.MPQ` if you already built patches for other modules. Other languages and clients may differ: the highest-numbered patch that contains a `Spell.dbc` is the one the client uses. Extract them with an MPQ tool or StormLib.
2. Run the generator with both:
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

## Uninstalling

The module's database changes stay when the module is removed, so it comes with uninstall SQL in [`tools/uninstall`](tools/uninstall), where the server never runs it on its own:

1. Run `mod_companion_auras_uninstall_world.sql` on the world database, then `mod_companion_auras_uninstall_characters.sql` on the characters database. This restores the companion item tooltips, removes the aura spells and any companion auras saved on characters, and drops the module's tables (saved companion names included).
2. Remove the module from `modules` and rebuild the server.
3. Delete the companion patch (e.g. `patch-enUS-5.MPQ`) from each client and delete the client's `Cache` folder.

## Configuration

| Setting | Default | What it does |
|---|---|---|
| `CompanionAuras.Enable` | `1` | Companion auras on or off. Companion names and companions coming back work either way. |
| `CompanionAuras.DisableInBattlegrounds` | `1` | No companion auras in battlegrounds and arenas. |
| `CompanionAuras.Resummon` | `1` | Bring your companion back after teleports, hearthstone, flights, death and login. |
| `CompanionAuras.WorldDrop.Chance` | `20000` | World drops: 1 in this many creature kills. `0` turns them off. |

Which companions are world drops is the `obtainable` column of [`tools/companions.csv`](tools/companions.csv): `No` means nothing else gives it.

## For addon authors

Client addons (such as Journal) can show each companion's aura and custom name. They ask with an addon message whispered to the player themselves, prefix `CompAuras`, for example `SendAddonMessage("CompAuras", "SYNC", "WHISPER", UnitName("player"))`. Answers come back as addon messages with the same prefix. On a realm without this module the request simply comes back unanswered, so it is safe to send.

| Request | Answer |
|---|---|
| `SYNC` | `A\|creature,spell,flags;...` (in several messages), then `N\|creature\|name` for each custom name, then `E\|` |
| `NAME\|creature\|name` | `N\|creature\|name`, or `F\|creature` if the name isn't allowed or the character doesn't know that companion |
| `NAME\|creature` | `R\|creature` (back to its normal name) |

`creature` is the companion's creature entry (as `GetCompanionInfo` returns it), `spell` its aura spell (its name, icon and tooltip are in the client patch), and `flags` is 1 for rare and 2 for a world drop. Renaming with `.petname` also sends `N|` or `R|`. The chat commands `.companions sync` and `.companions name <creature> [name]` do the same by hand.

## Credits

- [EvilTactician](https://github.com/EvilTactician): Original design and testing.
- Built for [AzerothCore](https://www.azerothcore.org/) with [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) and [mod-individual-progression](https://github.com/Grimfeather/mod-individual-progression).
