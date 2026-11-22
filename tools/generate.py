#!/usr/bin/env python3
"""
mod-companion-auras generator.

Builds everything that comes from the buff table (groups.csv and companions.csv):

  1. data/sql/db-world/mod_companion_auras_world.sql  (always)
     - the companion -> aura table the server reads
     - the aura spells for the server (spell_dbc)
     - the "Companion: ..." line on each companion item's tooltip
  2. A client Spell.dbc with the aura spells added      (with --dbc)
     - optionally packed into a patch MPQ               (with --mpq, needs StormLib)

Usage:
  python3 generate.py
  python3 generate.py --dbc /path/to/Spell.dbc --icons /path/to/SpellIcon.dbc --out out/Spell.dbc
  STORMLIB=~/StormLib/build/libstorm.so python3 generate.py --dbc ... --icons ... --out out/Spell.dbc --mpq out/patch-enUS-5.MPQ

Give --dbc the Spell.dbc your client uses now: if another custom patch already ships a
Spell.dbc (e.g. mod-guild-levels or mod-retail-professions), use that one, so its spells
are kept. Only one Spell.dbc wins on the client.
"""

import argparse
import csv
import ctypes
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SQL_OUT = os.path.join(ROOT, "data", "sql", "db-world", "mod_companion_auras_world.sql")

# Spell IDs: group i gets SPELL_BASE + 2i (common) and SPELL_BASE + 2i + 1 (rare)
SPELL_BASE = 951000
HELPER_BASE = 951100           # hidden rune aura for Wise: +0 common, +1 rare
LANG_MASK = 16712190           # standard "enUS string present" flags value

# Spell constants (3.3.5)
SPELL_EFFECT_APPLY_AURA = 6
TARGET_UNIT_CASTER = 1
ATTR0_DO_NOT_DISPLAY = 0x00000080
ATTR0_NO_AURA_CANCEL = 0x80000000
ATTR3_ALLOW_AURA_WHILE_DEAD = 0x00100000
DURATION_INFINITE = 21
RANGE_SELF = 1
CAST_INSTANT = 1

AURA_DUMMY = 4
AURA_MOD_THREAT = 10
AURA_MOD_SKILL = 30
AURA_MOD_SWIM_SPEED = 58
AURA_MOD_DAMAGE_PERCENT_DONE = 79
AURA_MOD_DAMAGE_PERCENT_TAKEN = 87
AURA_MOD_POWER_REGEN_PERCENT = 110
AURA_MOD_HEALING_DONE_PERCENT = 136
AURA_MOD_TOTAL_STAT_PERCENTAGE = 137
AURA_SAFE_FALL = 144
AURA_MOD_REPUTATION_GAIN = 156
AURA_MOD_RAGE_FROM_DAMAGE_DEALT = 213
AURA_MOD_CRIT_PCT = 290

POWER_MANA, POWER_ENERGY, POWER_RUNE = 0, 3, 5
SCHOOL_PHYSICAL, SCHOOL_FIRE, SCHOOL_NATURE, SCHOOL_FROST, SCHOOL_SHADOW, SCHOOL_ALL = 1, 4, 8, 16, 32, 127
STAT_STAMINA = 2
SKILL_ENGINEERING = 202

# Bonus types done in server code (match src/CompanionAuras.h)
BONUS_NONE, BONUS_GOLD, BONUS_ORE, BONUS_HERB, BONUS_LEATHER = 0, 1, 2, 3, 4

# spell_dbc / Spell.dbc columns in order: i = int, f = float, s = string
COLUMNS = [
    ("ID", "i"),
    ("Category", "i"),
    ("DispelType", "i"),
    ("Mechanic", "i"),
    ("Attributes", "i"),
    ("AttributesEx", "i"),
    ("AttributesEx2", "i"),
    ("AttributesEx3", "i"),
    ("AttributesEx4", "i"),
    ("AttributesEx5", "i"),
    ("AttributesEx6", "i"),
    ("AttributesEx7", "i"),
    ("ShapeshiftMask", "i"),
    ("unk_320_2", "i"),
    ("ShapeshiftExclude", "i"),
    ("unk_320_3", "i"),
    ("Targets", "i"),
    ("TargetCreatureType", "i"),
    ("RequiresSpellFocus", "i"),
    ("FacingCasterFlags", "i"),
    ("CasterAuraState", "i"),
    ("TargetAuraState", "i"),
    ("ExcludeCasterAuraState", "i"),
    ("ExcludeTargetAuraState", "i"),
    ("CasterAuraSpell", "i"),
    ("TargetAuraSpell", "i"),
    ("ExcludeCasterAuraSpell", "i"),
    ("ExcludeTargetAuraSpell", "i"),
    ("CastingTimeIndex", "i"),
    ("RecoveryTime", "i"),
    ("CategoryRecoveryTime", "i"),
    ("InterruptFlags", "i"),
    ("AuraInterruptFlags", "i"),
    ("ChannelInterruptFlags", "i"),
    ("ProcTypeMask", "i"),
    ("ProcChance", "i"),
    ("ProcCharges", "i"),
    ("MaxLevel", "i"),
    ("BaseLevel", "i"),
    ("SpellLevel", "i"),
    ("DurationIndex", "i"),
    ("PowerType", "i"),
    ("ManaCost", "i"),
    ("ManaCostPerLevel", "i"),
    ("ManaPerSecond", "i"),
    ("ManaPerSecondPerLevel", "i"),
    ("RangeIndex", "i"),
    ("Speed", "f"),
    ("ModalNextSpell", "i"),
    ("CumulativeAura", "i"),
    ("Totem_1", "i"),
    ("Totem_2", "i"),
    ("Reagent_1", "i"),
    ("Reagent_2", "i"),
    ("Reagent_3", "i"),
    ("Reagent_4", "i"),
    ("Reagent_5", "i"),
    ("Reagent_6", "i"),
    ("Reagent_7", "i"),
    ("Reagent_8", "i"),
    ("ReagentCount_1", "i"),
    ("ReagentCount_2", "i"),
    ("ReagentCount_3", "i"),
    ("ReagentCount_4", "i"),
    ("ReagentCount_5", "i"),
    ("ReagentCount_6", "i"),
    ("ReagentCount_7", "i"),
    ("ReagentCount_8", "i"),
    ("EquippedItemClass", "i"),
    ("EquippedItemSubclass", "i"),
    ("EquippedItemInvTypes", "i"),
    ("Effect_1", "i"),
    ("Effect_2", "i"),
    ("Effect_3", "i"),
    ("EffectDieSides_1", "i"),
    ("EffectDieSides_2", "i"),
    ("EffectDieSides_3", "i"),
    ("EffectRealPointsPerLevel_1", "f"),
    ("EffectRealPointsPerLevel_2", "f"),
    ("EffectRealPointsPerLevel_3", "f"),
    ("EffectBasePoints_1", "i"),
    ("EffectBasePoints_2", "i"),
    ("EffectBasePoints_3", "i"),
    ("EffectMechanic_1", "i"),
    ("EffectMechanic_2", "i"),
    ("EffectMechanic_3", "i"),
    ("ImplicitTargetA_1", "i"),
    ("ImplicitTargetA_2", "i"),
    ("ImplicitTargetA_3", "i"),
    ("ImplicitTargetB_1", "i"),
    ("ImplicitTargetB_2", "i"),
    ("ImplicitTargetB_3", "i"),
    ("EffectRadiusIndex_1", "i"),
    ("EffectRadiusIndex_2", "i"),
    ("EffectRadiusIndex_3", "i"),
    ("EffectAura_1", "i"),
    ("EffectAura_2", "i"),
    ("EffectAura_3", "i"),
    ("EffectAuraPeriod_1", "i"),
    ("EffectAuraPeriod_2", "i"),
    ("EffectAuraPeriod_3", "i"),
    ("EffectMultipleValue_1", "f"),
    ("EffectMultipleValue_2", "f"),
    ("EffectMultipleValue_3", "f"),
    ("EffectChainTargets_1", "i"),
    ("EffectChainTargets_2", "i"),
    ("EffectChainTargets_3", "i"),
    ("EffectItemType_1", "i"),
    ("EffectItemType_2", "i"),
    ("EffectItemType_3", "i"),
    ("EffectMiscValue_1", "i"),
    ("EffectMiscValue_2", "i"),
    ("EffectMiscValue_3", "i"),
    ("EffectMiscValueB_1", "i"),
    ("EffectMiscValueB_2", "i"),
    ("EffectMiscValueB_3", "i"),
    ("EffectTriggerSpell_1", "i"),
    ("EffectTriggerSpell_2", "i"),
    ("EffectTriggerSpell_3", "i"),
    ("EffectPointsPerCombo_1", "f"),
    ("EffectPointsPerCombo_2", "f"),
    ("EffectPointsPerCombo_3", "f"),
    ("EffectSpellClassMaskA_1", "i"),
    ("EffectSpellClassMaskA_2", "i"),
    ("EffectSpellClassMaskA_3", "i"),
    ("EffectSpellClassMaskB_1", "i"),
    ("EffectSpellClassMaskB_2", "i"),
    ("EffectSpellClassMaskB_3", "i"),
    ("EffectSpellClassMaskC_1", "i"),
    ("EffectSpellClassMaskC_2", "i"),
    ("EffectSpellClassMaskC_3", "i"),
    ("SpellVisualID_1", "i"),
    ("SpellVisualID_2", "i"),
    ("SpellIconID", "i"),
    ("ActiveIconID", "i"),
    ("SpellPriority", "i"),
    ("Name_Lang_enUS", "s"),
    ("Name_Lang_enGB", "s"),
    ("Name_Lang_koKR", "s"),
    ("Name_Lang_frFR", "s"),
    ("Name_Lang_deDE", "s"),
    ("Name_Lang_enCN", "s"),
    ("Name_Lang_zhCN", "s"),
    ("Name_Lang_enTW", "s"),
    ("Name_Lang_zhTW", "s"),
    ("Name_Lang_esES", "s"),
    ("Name_Lang_esMX", "s"),
    ("Name_Lang_ruRU", "s"),
    ("Name_Lang_ptPT", "s"),
    ("Name_Lang_ptBR", "s"),
    ("Name_Lang_itIT", "s"),
    ("Name_Lang_Unk", "s"),
    ("Name_Lang_Mask", "i"),
    ("NameSubtext_Lang_enUS", "s"),
    ("NameSubtext_Lang_enGB", "s"),
    ("NameSubtext_Lang_koKR", "s"),
    ("NameSubtext_Lang_frFR", "s"),
    ("NameSubtext_Lang_deDE", "s"),
    ("NameSubtext_Lang_enCN", "s"),
    ("NameSubtext_Lang_zhCN", "s"),
    ("NameSubtext_Lang_enTW", "s"),
    ("NameSubtext_Lang_zhTW", "s"),
    ("NameSubtext_Lang_esES", "s"),
    ("NameSubtext_Lang_esMX", "s"),
    ("NameSubtext_Lang_ruRU", "s"),
    ("NameSubtext_Lang_ptPT", "s"),
    ("NameSubtext_Lang_ptBR", "s"),
    ("NameSubtext_Lang_itIT", "s"),
    ("NameSubtext_Lang_Unk", "s"),
    ("NameSubtext_Lang_Mask", "i"),
    ("Description_Lang_enUS", "s"),
    ("Description_Lang_enGB", "s"),
    ("Description_Lang_koKR", "s"),
    ("Description_Lang_frFR", "s"),
    ("Description_Lang_deDE", "s"),
    ("Description_Lang_enCN", "s"),
    ("Description_Lang_zhCN", "s"),
    ("Description_Lang_enTW", "s"),
    ("Description_Lang_zhTW", "s"),
    ("Description_Lang_esES", "s"),
    ("Description_Lang_esMX", "s"),
    ("Description_Lang_ruRU", "s"),
    ("Description_Lang_ptPT", "s"),
    ("Description_Lang_ptBR", "s"),
    ("Description_Lang_itIT", "s"),
    ("Description_Lang_Unk", "s"),
    ("Description_Lang_Mask", "i"),
    ("AuraDescription_Lang_enUS", "s"),
    ("AuraDescription_Lang_enGB", "s"),
    ("AuraDescription_Lang_koKR", "s"),
    ("AuraDescription_Lang_frFR", "s"),
    ("AuraDescription_Lang_deDE", "s"),
    ("AuraDescription_Lang_enCN", "s"),
    ("AuraDescription_Lang_zhCN", "s"),
    ("AuraDescription_Lang_enTW", "s"),
    ("AuraDescription_Lang_zhTW", "s"),
    ("AuraDescription_Lang_esES", "s"),
    ("AuraDescription_Lang_esMX", "s"),
    ("AuraDescription_Lang_ruRU", "s"),
    ("AuraDescription_Lang_ptPT", "s"),
    ("AuraDescription_Lang_ptBR", "s"),
    ("AuraDescription_Lang_itIT", "s"),
    ("AuraDescription_Lang_Unk", "s"),
    ("AuraDescription_Lang_Mask", "i"),
    ("ManaCostPct", "i"),
    ("StartRecoveryCategory", "i"),
    ("StartRecoveryTime", "i"),
    ("MaxTargetLevel", "i"),
    ("SpellClassSet", "i"),
    ("SpellClassMask_1", "i"),
    ("SpellClassMask_2", "i"),
    ("SpellClassMask_3", "i"),
    ("MaxTargets", "i"),
    ("DefenseType", "i"),
    ("PreventionType", "i"),
    ("StanceBarOrder", "i"),
    ("EffectChainAmplitude_1", "f"),
    ("EffectChainAmplitude_2", "f"),
    ("EffectChainAmplitude_3", "f"),
    ("MinFactionID", "i"),
    ("MinReputation", "i"),
    ("RequiredAuraVision", "i"),
    ("RequiredTotemCategoryID_1", "i"),
    ("RequiredTotemCategoryID_2", "i"),
    ("RequiredAreasID", "i"),
    ("SchoolMask", "i"),
    ("RuneCostID", "i"),
    ("SpellMissileID", "i"),
    ("PowerDisplayID", "i"),
    ("EffectBonusMultiplier_1", "f"),
    ("EffectBonusMultiplier_2", "f"),
    ("EffectBonusMultiplier_3", "f"),
    ("SpellDescriptionVariableID", "i"),
    ("SpellDifficultyID", "i"),
]


def effects_for(group, v, v2):
    """Aura effects (aura, misc, miscB, amount) and tooltip text for one group at one size."""
    school = {"Venomous": (SCHOOL_NATURE, "Nature"), "Fiery": (SCHOOL_FIRE, "Fire"), "Frosty": (SCHOOL_FROST, "Frost"),
              "Shadowy": (SCHOOL_SHADOW, "Shadow"), "Predator": (SCHOOL_PHYSICAL, "Physical")}
    if group == "Wise":
        return ([(AURA_MOD_POWER_REGEN_PERCENT, POWER_MANA, 0, v), (AURA_MOD_POWER_REGEN_PERCENT, POWER_ENERGY, 0, v),
                 (AURA_MOD_RAGE_FROM_DAMAGE_DEALT, 0, 0, v)],
                f"Mana and energy regeneration, rage gained and rune regeneration increased by {v}%.")
    if group == "Feline":
        return [(AURA_MOD_CRIT_PCT, 0, 0, v)], f"Critical strike chance increased by {v}%."
    if group in school:
        mask, name = school[group]
        return [(AURA_MOD_DAMAGE_PERCENT_DONE, mask, 0, v)], f"{name} damage done increased by {v}%."
    if group == "Sturdy":
        return [(AURA_MOD_TOTAL_STAT_PERCENTAGE, STAT_STAMINA, 0, v)], f"Stamina increased by {v}%."
    if group == "Dragonkin":
        return [(AURA_MOD_DAMAGE_PERCENT_TAKEN, SCHOOL_ALL, 0, -v)], f"Damage taken reduced by {v}%."
    if group == "Soothing":
        return ([(AURA_MOD_HEALING_DONE_PERCENT, 0, 0, v), (AURA_MOD_THREAT, SCHOOL_ALL, 0, -v2)],
                f"Healing done increased by {v}% and threat generated reduced by {v2}%.")
    if group == "Aquatic":
        return [(AURA_MOD_SWIM_SPEED, 0, 0, v)], f"Swim speed increased by {v}%."
    if group == "Feathered":
        return [(AURA_SAFE_FALL, 0, 0, v)], f"Falling damage reduced, as if you fell {v} yards less."
    if group == "Mechanical":
        return [(AURA_MOD_SKILL, SKILL_ENGINEERING, 0, v)], f"Engineering skill increased by {v}."
    if group == "Champion":
        return [(AURA_MOD_REPUTATION_GAIN, 0, 0, v)], f"Reputation gained increased by {v}%."
    if group == "Lucky":
        return [(AURA_DUMMY, 0, 0, 0)], f"Your share of looted gold increased by {v}%."
    gather = {"Prospector": "Mining gives 1 extra of the vein's main ore",
              "Green thumb": "Herbalism gives 1 extra of the node's main herb",
              "Scavenger": "Skinning gives 1 extra of the main leather"}
    if group in gather:
        text = gather[group] + (f", with a {v2}% chance of 2." if v2 else ".")
        return [(AURA_DUMMY, 0, 0, 0)], text
    sys.exit(f"Unknown group in groups.csv: {group}")


def bonus_for(group, v, v2):
    return {"Lucky": (BONUS_GOLD, v), "Prospector": (BONUS_ORE, v2), "Green thumb": (BONUS_HERB, v2),
            "Scavenger": (BONUS_LEATHER, v2)}.get(group, (BONUS_NONE, 0))


def number(value):
    return int(value) if value not in ("", None) else 0


def load_tables():
    with open(os.path.join(HERE, "groups.csv"), newline="", encoding="utf-8") as f:
        groups = list(csv.DictReader(f))
    with open(os.path.join(HERE, "companions.csv"), newline="", encoding="utf-8") as f:
        companions = list(csv.DictReader(f))
    return groups, companions


def build_spells(groups, icon_ids):
    """One visible aura per group and size, plus the hidden rune aura for Wise."""
    spells, by_group = [], {}
    for i, g in enumerate(groups):
        name = g["group"]
        for tier, (v, v2) in enumerate([(number(g["common"]), number(g["common2"])), (number(g["rare"]), number(g["rare2"]))]):
            effects, text = effects_for(name, v, v2)
            spell_id = SPELL_BASE + 2 * i + tier
            spells.append(make_spell(spell_id, f"Companion: {name}", text, effects, icon_ids.get(name, 1), hidden=False))
            helper = 0
            if name == "Wise":
                helper = HELPER_BASE + tier
                rune_effects = [(AURA_MOD_POWER_REGEN_PERCENT, POWER_RUNE, rune, v) for rune in (0, 1, 2)]
                spells.append(make_spell(helper, f"Companion: {name} (runes)", text, rune_effects, 1, hidden=True))
            by_group[(name, tier)] = (spell_id, helper, text, bonus_for(name, v, v2))
    return spells, by_group


def make_spell(spell_id, name, text, effects, icon, hidden):
    s = {c: (0.0 if k == "f" else ("" if k == "s" else 0)) for c, k in COLUMNS}
    attributes = ATTR0_NO_AURA_CANCEL | (ATTR0_DO_NOT_DISPLAY if hidden else 0)
    s.update({
        "ID": spell_id,
        "Attributes": attributes - (1 << 32) if attributes >= (1 << 31) else attributes,
        "AttributesEx3": ATTR3_ALLOW_AURA_WHILE_DEAD,
        "CastingTimeIndex": CAST_INSTANT,
        "DurationIndex": DURATION_INFINITE,
        "RangeIndex": RANGE_SELF,
        "SpellIconID": icon,
        "SchoolMask": 1,
        "Name_Lang_enUS": name,
        "Name_Lang_Mask": LANG_MASK,
        "NameSubtext_Lang_Mask": LANG_MASK,
        "Description_Lang_enUS": text,
        "Description_Lang_Mask": LANG_MASK,
        "AuraDescription_Lang_enUS": text,
        "AuraDescription_Lang_Mask": LANG_MASK,
    })
    for n, (aura, misc, misc_b, amount) in enumerate(effects, start=1):
        s[f"Effect_{n}"] = SPELL_EFFECT_APPLY_AURA
        s[f"EffectAura_{n}"] = aura
        s[f"EffectMiscValue_{n}"] = misc
        s[f"EffectMiscValueB_{n}"] = misc_b
        s[f"EffectDieSides_{n}"] = 1
        s[f"EffectBasePoints_{n}"] = amount - 1   # base + 1d1 = amount
        s[f"ImplicitTargetA_{n}"] = TARGET_UNIT_CASTER
        s[f"EffectMultipleValue_{n}"] = 1.0
        s[f"EffectChainAmplitude_{n}"] = 1.0
        s[f"EffectBonusMultiplier_{n}"] = 1.0
    return s


def sql_value(v):
    if isinstance(v, str):
        return "'" + v.replace("\\", "\\\\").replace("'", "''") + "'"
    if isinstance(v, float):
        return repr(v)
    return str(v)


def write_sql(spells, by_group, companions):
    lines = [
        "-- mod-companion-auras: generated by tools/generate.py from tools/groups.csv and tools/companions.csv.",
        "-- Do not edit by hand: change the CSV files and run the generator again.",
        "",
        "CREATE TABLE IF NOT EXISTS `mod_companion_auras` (",
        "    `item_entry`   INT UNSIGNED     NOT NULL COMMENT 'Companion item',",
        "    `aura_spell`   INT UNSIGNED     NOT NULL COMMENT 'Aura while the companion is out',",
        "    `helper_spell` INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT 'Hidden extra aura, 0 for none',",
        "    `bonus_type`   TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 none, 1 gold, 2 ore, 3 herb, 4 leather',",
        "    `bonus_value`  INT UNSIGNED     NOT NULL DEFAULT 0 COMMENT 'Gold percent, or percent chance of a second gathered item',",
        "    `group_name`   VARCHAR(32)      NOT NULL DEFAULT '',",
        "    `rare`         TINYINT UNSIGNED NOT NULL DEFAULT 0,",
        "    `obtainable`   TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'For the planned world drops',",
        "    PRIMARY KEY (`item_entry`)",
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;",
        "",
        "DELETE FROM `mod_companion_auras`;",
        "INSERT INTO `mod_companion_auras` (`item_entry`, `aura_spell`, `helper_spell`, `bonus_type`, `bonus_value`, `group_name`, `rare`, `obtainable`) VALUES",
    ]
    rows = []
    for c in companions:
        tier = 1 if c["rarity"] == "Rare" else 0
        spell_id, helper, text, (bonus_type, bonus_value) = by_group[(c["group"], tier)]
        rows.append(f"({int(c['item_entry'])}, {spell_id}, {helper}, {bonus_type}, {bonus_value}, {sql_value(c['group'])}, {tier}, {1 if c['obtainable'] == 'Yes' else 0})")
    lines.append(",\n".join(rows) + ";")

    ids = ", ".join(str(s["ID"]) for s in spells)
    cols = ", ".join(f"`{c}`" for c, _ in COLUMNS)
    lines += ["", "-- Aura spells (the client gets the same spells from the patch)",
              f"DELETE FROM `spell_dbc` WHERE `ID` IN ({ids});",
              f"INSERT INTO `spell_dbc` ({cols}) VALUES"]
    lines.append(",\n".join("(" + ", ".join(sql_value(s[c]) for c, _ in COLUMNS) + ")" for s in spells) + ";")

    lines += ["", "-- \"Companion: ...\" line on each companion item's tooltip. Anything after an earlier",
              "-- \"Companion:\" line is replaced, so running this again just updates the text."]
    for c in companions:
        tier = 1 if c["rarity"] == "Rare" else 0
        text = by_group[(c["group"], tier)][2]
        line = sql_value(f"Companion: {text[:-1]} while summoned.")
        lines.append(
            f"UPDATE `item_template` SET `description` = CONCAT(TRIM(SUBSTRING_INDEX(`description`, 'Companion:', 1)), "
            f"IF(TRIM(SUBSTRING_INDEX(`description`, 'Companion:', 1)) = '', '', ' '), {line}) WHERE `entry` = {int(c['item_entry'])};")

    os.makedirs(os.path.dirname(SQL_OUT), exist_ok=True)
    with open(SQL_OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Wrote {os.path.relpath(SQL_OUT, ROOT)} ({len(companions)} companions, {len(spells)} spells)")


def read_dbc(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, count, fields, size, string_size = struct.unpack_from("<4s4I", data, 0)
    if magic != b"WDBC":
        sys.exit(f"{path} is not a DBC file")
    records = [data[20 + i * size: 20 + (i + 1) * size] for i in range(count)]
    strings = data[20 + count * size: 20 + count * size + string_size]
    return fields, size, records, strings


def dbc_string(strings, offset):
    end = strings.index(b"\0", offset)
    return strings[offset:end].decode("utf-8", "replace")


def find_icons(icons_path, groups):
    """SpellIcon.dbc: id, path. Each group lists one or more icon paths; the first found wins."""
    _, _, records, strings = read_dbc(icons_path)
    by_path = {}
    for r in records:
        icon_id, offset = struct.unpack_from("<2I", r, 0)
        by_path[dbc_string(strings, offset).lower()] = icon_id
    ids = {}
    for g in groups:
        for candidate in g["icon"].split("|"):
            if candidate.lower() in by_path:
                ids[g["group"]] = by_path[candidate.lower()]
                break
        else:
            ids[g["group"]] = by_path.get("interface\\icons\\inv_misc_questionmark", 1)
            print(f"  Icon not found for {g['group']} ({g['icon']}): using a question mark. Edit groups.csv.")
    return ids


def write_dbc(dbc_in, dbc_out, spells):
    fields, size, records, strings = read_dbc(dbc_in)
    if fields != len(COLUMNS) or size != 4 * fields:
        sys.exit(f"{dbc_in} has {fields} fields; a 3.3.5a Spell.dbc has {len(COLUMNS)}")

    ours = {s["ID"] for s in spells}
    kept = [r for r in records if struct.unpack_from("<I", r, 0)[0] not in ours]
    if len(kept) != len(records):
        print(f"  Replacing {len(records) - len(kept)} companion aura spells already in {os.path.basename(dbc_in)}")

    string_block = bytearray(strings if strings else b"\0")
    cache = {}

    def add_string(text):
        if not text:
            return 0
        if text not in cache:
            cache[text] = len(string_block)
            string_block.extend(text.encode("utf-8") + b"\0")
        return cache[text]

    for s in spells:
        record = bytearray()
        for column, kind in COLUMNS:
            v = s[column]
            if kind == "f":
                record += struct.pack("<f", v)
            elif kind == "s":
                record += struct.pack("<I", add_string(v))
            else:
                record += struct.pack("<i", v)
        kept.append(bytes(record))

    os.makedirs(os.path.dirname(os.path.abspath(dbc_out)), exist_ok=True)
    with open(dbc_out, "wb") as f:
        f.write(struct.pack("<4s4I", b"WDBC", len(kept), fields, size, len(string_block)))
        for r in kept:
            f.write(r)
        f.write(string_block)
    print(f"Wrote {dbc_out} ({len(spells)} companion aura spells)")


def write_mpq(dbc_out, mpq_out):
    lib_path = os.path.expanduser(os.environ.get("STORMLIB", ""))
    if not lib_path or not os.path.exists(lib_path):
        sys.exit("--mpq needs STORMLIB=/path/to/libstorm.so")
    storm = ctypes.CDLL(lib_path)
    handle = ctypes.c_void_p()
    if os.path.exists(mpq_out):
        os.remove(mpq_out)
    if not storm.SFileCreateArchive(mpq_out.encode(), 0, 16, ctypes.byref(handle)):
        sys.exit(f"Could not create {mpq_out}")
    ok = storm.SFileAddFileEx(handle, dbc_out.encode(), b"DBFilesClient\\Spell.dbc", 0x00000200 | 0x80000000, 0x02, 0x02)
    storm.SFileCloseArchive(handle)
    if not ok:
        sys.exit("Could not add Spell.dbc to the MPQ")
    print(f"Wrote {mpq_out}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dbc", help="Spell.dbc your client uses now (to build the client patch)")
    parser.add_argument("--icons", help="SpellIcon.dbc (to pick the buff icons)")
    parser.add_argument("--out", default=os.path.join(HERE, "out", "Spell.dbc"), help="Where to write the new Spell.dbc")
    parser.add_argument("--mpq", help="Also pack the new Spell.dbc into this patch MPQ (needs STORMLIB)")
    args = parser.parse_args()

    groups, companions = load_tables()
    known = {g["group"] for g in groups}
    for c in companions:
        if c["group"] not in known:
            sys.exit(f"{c['companion']} ({c['item_entry']}) has unknown group {c['group']}")

    icon_ids = find_icons(args.icons, groups) if args.icons else {}
    spells, by_group = build_spells(groups, icon_ids)
    write_sql(spells, by_group, companions)

    if args.dbc:
        if not args.icons:
            print("  No --icons given: every buff gets the same icon.")
        write_dbc(args.dbc, args.out, spells)
        if args.mpq:
            write_mpq(os.path.abspath(args.out), os.path.abspath(args.mpq))



if __name__ == "__main__":
    main()
