/*
 * Copyright (C) 2026+ mod-zone-scaling contributors, released under GNU AGPL v3
 * license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "ZoneScaling.h"

#include "Config.h"
#include "CreatureData.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "UnitDefines.h"
#include "World.h"

#include <algorithm>
#include <sstream>

// Warband: which level every creature presents at for a viewer. The module
// inside is per-viewer; the Warband rule makes the group the unit, so all
// members see the world at the highest member's level. Opt-out (per-viewer)
// via ZoneScaling.GroupAnchor = 0 restores the shipped behaviour exactly.
uint8 ZoneScalingAnchorLevel(Player const* player)
{
    if (!player)
        return 1;

    uint8 anchor = std::max<uint8>(static_cast<uint8>(1), player->GetLevel());

    if (!sConfigMgr->GetOption<bool>("ZoneScaling.GroupAnchor", true))
        return anchor;

    // Player::GetGroup() is const-qualified on a const player and returns a
    // const group; the member list has no const iteration path, so unconst it.
    if (Group* group = const_cast<Group*>(player->GetGroup()))
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* member = itr->GetSource())
                anchor = std::max(anchor, member->GetLevel());
        }
    }

    return anchor;
}

char const* ZoneScalingExclusionName(ZoneScalingExclusion reason)
{
    switch (reason)
    {
        case ZS_NOT_EXCLUDED:  return "not excluded";
        case ZS_EXCL_TABLE:    return "listed in mod_zone_scaling_exclude";
        case ZS_EXCL_WORLDBOSS:return "world boss";
        case ZS_EXCL_CRITTER:  return "critter";
        case ZS_EXCL_TOTEM:    return "totem";
        case ZS_EXCL_TRIGGER:  return "trigger creature";
        case ZS_EXCL_QUESTGIVER: return "quest giver";
        case ZS_EXCL_VENDOR:   return "vendor";
        case ZS_EXCL_TRAINER:  return "trainer";
        case ZS_EXCL_NO_XP:    return "grants no XP";
        case ZS_EXCL_UNATTACKABLE: return "cannot be attacked by players";
        default:               return "unknown";
    }
}

ZoneScalingMgr* ZoneScalingMgr::instance()
{
    static ZoneScalingMgr instance;
    return &instance;
}

void ZoneScalingMgr::LoadConfig()
{
    _enable = sConfigMgr->GetOption<bool>("ZoneScaling.Enable", false);
    _debug  = sConfigMgr->GetOption<bool>("ZoneScaling.Debug", false);
    _instancedRequireExplicit =
        sConfigMgr->GetOption<bool>("ZoneScaling.Instanced.RequireExplicitBand", true);

    _scaleDamage    = sConfigMgr->GetOption<bool>("ZoneScaling.ScaleDamage", true);
    _scaleHitChance = sConfigMgr->GetOption<bool>("ZoneScaling.ScaleHitChance", true);
    _scaleLevelDisplay = sConfigMgr->GetOption<bool>("ZoneScaling.ScaleLevelDisplay", true);
    _scalePoolDisplay  = sConfigMgr->GetOption<bool>("ZoneScaling.ScalePoolDisplay", true);
    _coreView          = sConfigMgr->GetOption<bool>("ZoneScaling.CoreView", true);

    std::string const behaviour =
        sConfigMgr->GetOption<std::string>("ZoneScaling.DefaultBehaviour", "off");

    if (behaviour == "clamp")
        _defaultBehaviour = ZS_DEFAULT_CLAMP;
    else
    {
        if (behaviour != "off")
            LOG_ERROR("module.zonescaling",
                "ZoneScaling.DefaultBehaviour = '{}' is not recognised; using 'off'.",
                behaviour);

        _defaultBehaviour = ZS_DEFAULT_OFF;
    }

    std::string const direction =
        sConfigMgr->GetOption<std::string>("ZoneScaling.Direction", "both");

    if (direction == "up")
        _direction = ZS_DIR_UP;
    else
    {
        if (direction != "both")
            LOG_ERROR("module.zonescaling",
                "ZoneScaling.Direction = '{}' is not recognised; using 'both'.",
                direction);

        _direction = ZS_DIR_BOTH;
    }

    std::string const canonical =
        sConfigMgr->GetOption<std::string>("ZoneScaling.CanonicalLevel", "native");

    if (canonical == "midpoint")
        _canonical = ZS_CANON_MIDPOINT;
    else
    {
        if (canonical != "native")
            LOG_ERROR("module.zonescaling",
                "ZoneScaling.CanonicalLevel = '{}' is not recognised; using 'native'.",
                canonical);

        _canonical = ZS_CANON_NATIVE;
    }

    std::string const aggro =
        sConfigMgr->GetOption<std::string>("ZoneScaling.AggroRange", "stock");

    if (aggro == "normalize")
        _aggroMode = ZS_AGGRO_NORMALIZE;
    else
    {
        if (aggro != "stock")
            LOG_ERROR("module.zonescaling",
                "ZoneScaling.AggroRange = '{}' is not recognised; using 'stock'.",
                aggro);

        _aggroMode = ZS_AGGRO_STOCK;
    }

    // Instanced content is opt-in, one kind at a time. Read individually rather
    // than as a list so that a typo disables one kind loudly instead of
    // silently re-enabling everything, and so the defaults survive an operator
    // who sets only the key they care about.
    _mapKindEnabled[ZS_MAP_WORLD] =
        sConfigMgr->GetOption<bool>("ZoneScaling.Maps.World", true);
    _mapKindEnabled[ZS_MAP_DUNGEON] =
        sConfigMgr->GetOption<bool>("ZoneScaling.Maps.Dungeon", false);
    _mapKindEnabled[ZS_MAP_RAID] =
        sConfigMgr->GetOption<bool>("ZoneScaling.Maps.Raid", false);
    _mapKindEnabled[ZS_MAP_BATTLEGROUND] =
        sConfigMgr->GetOption<bool>("ZoneScaling.Maps.Battleground", false);
    _mapKindEnabled[ZS_MAP_ARENA] =
        sConfigMgr->GetOption<bool>("ZoneScaling.Maps.Arena", false);

    _chronoEnable  = sConfigMgr->GetOption<bool>("ZoneScaling.Chrono.Enable", false);
    _chronoDefault = sConfigMgr->GetOption<bool>("ZoneScaling.Chrono.Default", false);

    // -1 inherits Chrono.Default; anything else is clamped to 0/1.
    {
        int32 const botDefault = sConfigMgr->GetOption<int32>("ZoneScaling.Chrono.BotDefault", -1);
        _chronoBotDefault = botDefault < 0 ? int8(-1) : int8(botDefault != 0);
    }

    _xpEnable    = sConfigMgr->GetOption<bool>("ZoneScaling.Xp.Enable", true);
    _xpGreyKills = sConfigMgr->GetOption<bool>("ZoneScaling.Xp.GreyKills", true);
    _xpQuests    = sConfigMgr->GetOption<bool>("ZoneScaling.Xp.Quests", true);

    // Capped rather than trusted. This multiplies every award a Chrono opt-in
    // receives, so a mistyped value is a realm-wide XP rate change, and one
    // that no `Rate.XP.*` setting explains when someone later goes looking.
    _chronoXpBonus =
        std::min<uint32>(sConfigMgr->GetOption<uint32>("ZoneScaling.Chrono.XpBonus", 10), 1000);

    _lootEnable = sConfigMgr->GetOption<bool>("ZoneScaling.Loot.Enable", true);
    _lootMoney  = sConfigMgr->GetOption<bool>("ZoneScaling.Loot.Money", true);
    _lootGear   = sConfigMgr->GetOption<bool>("ZoneScaling.Loot.Gear", false);

    // Floored at 1.0 rather than trusted: a value below 1 would invert the
    // clamp in DescribeMoneyMultiplier, making its lower bound larger than its
    // upper one, and std::clamp on a reversed range is undefined behaviour.
    _lootMoneyMax =
        std::max(sConfigMgr->GetOption<float>("ZoneScaling.Loot.MoneyMax", 50.0f), 1.0f);

    _lootGearTolerance =
        uint8(std::min<uint32>(sConfigMgr->GetOption<uint32>("ZoneScaling.Loot.Gear.Tolerance", 5),
                               255));

    _lootGearMaxQuality =
        uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("ZoneScaling.Loot.Gear.MaxQuality", 3),
                                 2, 4));

    _preserveEliteOffset = sConfigMgr->GetOption<bool>("ZoneScaling.PreserveEliteOffset", true);

    _eliteOffsetMax =
        uint8(std::min<uint32>(sConfigMgr->GetOption<uint32>("ZoneScaling.PreserveEliteOffset.Max", 3),
                               255));

    // Capped at 10: past the grey threshold (5 + level/10 at the top of the
    // curve) a scaled creature would pay no XP, which contradicts the reason
    // the option exists. Anyone wanting more than that wants a narrower band.
    _targetOffset =
        uint8(std::min<uint32>(sConfigMgr->GetOption<uint32>("ZoneScaling.TargetOffset", 0), 10));

    // Loud, because the two together are the one combination that can flatten
    // hand-tuned content, and an operator who set them months ago should be
    // reminded every startup rather than discover it from a raid group.
    if (_enable && (_mapKindEnabled[ZS_MAP_DUNGEON] || _mapKindEnabled[ZS_MAP_RAID]))
        LOG_WARN("module.zonescaling",
            "instanced content is enabled for scaling ({}{}{}). Dungeon and raid encounters are "
            "hand-tuned; scaling them is supported but is not a tested configuration.",
            _mapKindEnabled[ZS_MAP_DUNGEON] ? "dungeons" : "",
            _mapKindEnabled[ZS_MAP_DUNGEON] && _mapKindEnabled[ZS_MAP_RAID] ? " and " : "",
            _mapKindEnabled[ZS_MAP_RAID] ? "raids" : "");

    // Every startup, not just the first. Turning Chrono on with the shipped
    // default means every character who has not toggled -- which on the day it
    // is enabled is all of them -- stops being scaled at their next login. That
    // is one config line silently undoing every zone band on the realm, and it
    // is exactly the sort of thing an operator sets once and forgets.
    if (_enable && _chronoEnable && !_chronoDefault)
        LOG_WARN("module.zonescaling",
            "Chrono Mode is on and ZoneScaling.Chrono.Default = 0, so characters without a "
            "stored choice are NOT scaled. Zone bands apply only to players who have run "
            "'.chrono on'.");

    // The one configuration that is strictly worse than not installing this
    // module: creatures scale up, rewards do not, and an outlevelled zone
    // becomes harder *and* still worthless. Not refused -- an operator may want
    // exactly this while debugging -- but never silent.
    if (_enable && !_xpEnable)
        LOG_WARN("module.zonescaling",
            "ZoneScaling.Xp.Enable = 0 while scaling is on. Creatures will be scaled up but "
            "will still pay experience for their canonical level.");

    // Not the same failure as the XP one -- gear banding changes what drops
    // rather than how much of it -- so it gets its own line, and it is INFO
    // rather than WARN because it is a choice an operator has to make
    // deliberately in the first place.
    if (_enable && IsLootGearBandingEnabled())
        LOG_INFO("module.zonescaling",
            "loot gear banding is ON: creature drops of uncommon quality and above will be "
            "replaced with items of the same slot at the effective level. This changes which "
            "items the world drops.");

    _defaultBand.ZoneId   = 0;
    _defaultBand.MinLevel = sConfigMgr->GetOption<uint32>("ZoneScaling.Default.MinLevel", 1);
    _defaultBand.MaxLevel = sConfigMgr->GetOption<uint32>("ZoneScaling.Default.MaxLevel", 80);
    _defaultBand.Enabled  = true;

    if (_defaultBand.MinLevel == 0 || _defaultBand.MinLevel > _defaultBand.MaxLevel)
    {
        LOG_ERROR("module.zonescaling",
            "ZoneScaling.Default.MinLevel/MaxLevel is an invalid band ({}-{}). "
            "Falling back to DefaultBehaviour = off.",
            _defaultBand.MinLevel, _defaultBand.MaxLevel);

        _defaultBehaviour = ZS_DEFAULT_OFF;
    }
}

// Querying a table that does not exist is FATAL, not merely unsuccessful:
// MySQLConnection::_HandleMySQLErrno treats ER_NO_SUCH_TABLE (1146) as a
// database-out-of-date condition and calls ABORT(), taking the worldserver
// down. A module cannot afford that -- an admin who installs it and forgets
// the SQL would get a server that will not boot, blaming their whole database.
//
// So probe information_schema first. That query is always valid, so it can
// only answer the question rather than become the problem. Verified the hard
// way on 2026-08-08: renaming the table away crashed a running worldserver.
bool ZoneScalingMgr::TableExists(char const* table)
{
    QueryResult result = WorldDatabase.Query(
        "SELECT 1 FROM information_schema.TABLES "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}'", table);

    return result != nullptr;
}

// The same probe against the characters database. Separate rather than
// parameterised because the two Database objects are distinct types, and
// because DATABASE() resolves per connection -- which is the whole reason one
// function cannot answer for both.
bool ZoneScalingMgr::CharacterTableExists(char const* table)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM information_schema.TABLES "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}'", table);

    return result != nullptr;
}

bool ZoneScalingMgr::LoadBands()
{
    _bands.clear();

    if (!TableExists("mod_zone_scaling_zone"))
    {
        LOG_ERROR("module.zonescaling",
            "Table mod_zone_scaling_zone is missing, so mod-zone-scaling is "
            "inactive. Apply data/sql/db-world/base/mod_zone_scaling_base.sql "
            "to your world database. Nothing else is affected.");
        return false;
    }

    QueryResult result = WorldDatabase.Query(
        "SELECT zoneId, minLevel, maxLevel, enabled, direction, ignore_chrono "
        "FROM mod_zone_scaling_zone");

    // Now unambiguous: the table is there, so no rows means no rows. That is
    // the shipped state and is not an error.
    if (!result)
    {
        LOG_INFO("module.zonescaling",
            "mod_zone_scaling_zone is empty; no zone is configured for scaling.");
        return true;
    }

    uint32 skipped = 0;

    do
    {
        Field* fields = result->Fetch();

        ZoneScalingBand band;
        band.ZoneId   = fields[0].Get<uint32>();
        band.MinLevel = fields[1].Get<uint8>();
        band.MaxLevel = fields[2].Get<uint8>();
        band.Enabled  = fields[3].Get<bool>();

        // 0 = inherit the global setting, 1 = both, 2 = up. Deliberately not
        // the ZS_DIR_* values: 0 is the column default, and "inherit" is the
        // answer that must fall out of a row nobody edited.
        switch (fields[4].Get<uint8>())
        {
            case 1:  band.Direction = ZS_DIR_BOTH; break;
            case 2:  band.Direction = ZS_DIR_UP;   break;
            default: band.Direction = ZS_DIR_INHERIT; break;
        }

        band.IgnoreChrono = fields[5].Get<bool>();

        if (band.MinLevel == 0 || band.MinLevel > band.MaxLevel)
        {
            LOG_ERROR("module.zonescaling",
                "mod_zone_scaling_zone: zone {} has an invalid band {}-{}; skipped.",
                band.ZoneId, band.MinLevel, band.MaxLevel);
            ++skipped;
            continue;
        }

        // A band reaching past the usable stat data is the "1 HP mob" trap:
        // ComputeStats will refuse those levels, so the zone would half-work in
        // a way that is very hard to diagnose from in-game symptoms. Say so at
        // load time, naming the ceiling, rather than letting it surface later.
        if (uint8 const ceiling = LowestMaxUsableLevel())
            if (band.MaxLevel > ceiling)
                LOG_ERROR("module.zonescaling",
                    "mod_zone_scaling_zone: zone {} has maxLevel {}, above level {} "
                    "where creature_classlevelstats data runs out for at least one "
                    "unit_class. Creatures of that class will not be scaled above {}.",
                    band.ZoneId, band.MaxLevel, ceiling, ceiling);

        _bands[band.ZoneId] = band;
    } while (result->NextRow());

    LOG_INFO("module.zonescaling", "mod-zone-scaling: loaded {} zone band(s), {} skipped.",
        static_cast<uint32>(_bands.size()), skipped);

    return true;
}

bool ZoneScalingMgr::LoadExclusions()
{
    _excluded.clear();

    if (!TableExists("mod_zone_scaling_exclude"))
    {
        LOG_ERROR("module.zonescaling",
            "Table mod_zone_scaling_exclude is missing; rule-based exclusions "
            "still apply, but no hand-listed entry will be honoured.");
        return false;
    }

    QueryResult result = WorldDatabase.Query(
        "SELECT entry FROM mod_zone_scaling_exclude");

    // No rows is the shipped state and is not an error: the interesting
    // exclusions are derived from the template, not listed by hand.
    if (!result)
        return true;

    do
    {
        _excluded.insert(result->Fetch()[0].Get<uint32>());
    } while (result->NextRow());

    LOG_INFO("module.zonescaling", "mod-zone-scaling: loaded {} creature exclusion(s).",
        static_cast<uint32>(_excluded.size()));

    return true;
}

bool ZoneScalingMgr::LoadForced()
{
    _forced.clear();

    // Missing table is a warning, not an error, and deliberately quieter than
    // the exclude one: an install predating this table has no forced entries to
    // lose, whereas a missing exclude table silently re-enables creatures an
    // administrator had turned off.
    if (!TableExists("mod_zone_scaling_force"))
    {
        LOG_WARN("module.zonescaling",
            "Table mod_zone_scaling_force is missing; world bosses cannot be "
            "opted back into scaling. Run the module's base SQL to add it.");
        return false;
    }

    QueryResult result = WorldDatabase.Query(
        "SELECT entry FROM mod_zone_scaling_force");

    if (!result)
        return true;

    do
    {
        _forced.insert(result->Fetch()[0].Get<uint32>());
    } while (result->NextRow());

    LOG_INFO("module.zonescaling",
        "mod-zone-scaling: loaded {} forced world boss(es).",
        static_cast<uint32>(_forced.size()));

    return true;
}

// creature_classlevelstats is a core table, so it is always present; what is
// not guaranteed is that its rows mean anything. Rows past the real data
// ceiling are placeholders of basehp 1 / damage 0, and there are holes below
// the ceiling as well, so select on *content* rather than on existence.
//
// Measured on a stock 3.3.5a world DB, 2026-08-08: rows run to level 100 for
// every class, but usable data ends at 85 (class 1), 88 (class 2, with holes),
// 83 (class 4) and 85 (class 8, with a hole).
void ZoneScalingMgr::LoadBaseStatsCoverage()
{
    _maxUsableLevel.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT class, level, basehp0, basehp1, basehp2, "
        "damage_base, damage_exp1, damage_exp2 FROM creature_classlevelstats");

    if (!result)
    {
        LOG_ERROR("module.zonescaling",
            "creature_classlevelstats is empty. No creature stat can be computed, "
            "so mod-zone-scaling will decline to scale anything.");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        uint8 const unitClass = fields[0].Get<uint8>();
        uint8 const level     = fields[1].Get<uint8>();

        // Per expansion column, because that is the granularity at which the
        // placeholders actually appear.
        for (uint8 expansion = 0; expansion < MAX_EXPANSIONS; ++expansion)
        {
            uint32 const hp     = fields[2 + expansion].Get<uint32>();
            float const  damage = fields[5 + expansion].Get<float>();

            if (hp <= 1 || damage <= 0.0f)
                continue;

            uint8& highest = _maxUsableLevel[CoverageKey(unitClass, expansion)];
            highest = std::max(highest, level);
        }
    } while (result->NextRow());

    for (auto const& [key, highest] : _maxUsableLevel)
        LOG_INFO("module.zonescaling",
            "creature_classlevelstats: unit_class {} expansion {} has usable data up to level {}.",
            uint32(key >> 8), uint32(key & 0xFF), uint32(highest));
}

// Reads the core's own stat block rather than the module's coverage map. That
// is exact: it sees the same numbers SelectLevel would see, for the same
// expansion column, including the static default returned for a missing row.
bool ZoneScalingMgr::HasBaseStats(CreatureTemplate const* cinfo, uint8 level)
{
    if (!cinfo || cinfo->expansion >= MAX_EXPANSIONS)
        return false;

    CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(level, cinfo->unit_class);
    if (!stats)
        return false;

    return stats->BaseHealth[cinfo->expansion] > 1
        && stats->BaseDamage[cinfo->expansion] > 0.0f;
}

// The ceiling that holds for every class, which is the one a zone band has to
// respect: a zone contains creatures of all four unit classes.
uint8 ZoneScalingMgr::LowestMaxUsableLevel() const
{
    uint8 lowest = 0;

    for (auto const& [key, highest] : _maxUsableLevel)
        if (!lowest || highest < lowest)
            lowest = highest;

    return lowest;
}

// Mirrors Creature::SelectLevel (Creature.cpp) without a Creature. Any change
// to that function upstream must be reflected here; Z-11 CI is the place to
// notice, since a silent divergence would show up only as subtly wrong damage.
bool ZoneScalingMgr::ComputeStats(CreatureTemplate const* cinfo, uint8 level,
                                  ZoneScalingStats& out) const
{
    if (!HasBaseStats(cinfo, level))
        return false;

    CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(level, cinfo->unit_class);
    if (!stats)
        return false;

    // SelectLevel uses `IsPet() ? 0 : cInfo->rank`. There is no creature here,
    // and pets are not scaled by this module, so the template rank is correct.
    uint32 const rank = cinfo->rank;

    // Creature::_GetHealthMod is protected, so a module cannot call it. This
    // reproduces it. Note the default branch really does fall through to the
    // ELITE rate rather than the NORMAL one -- that is the core's behaviour,
    // not a typo here.
    float healthMod;
    switch (rank)
    {
        case CREATURE_ELITE_NORMAL:    healthMod = sWorld->getRate(RATE_CREATURE_NORMAL_HP); break;
        case CREATURE_ELITE_ELITE:     healthMod = sWorld->getRate(RATE_CREATURE_ELITE_ELITE_HP); break;
        case CREATURE_ELITE_RAREELITE: healthMod = sWorld->getRate(RATE_CREATURE_ELITE_RAREELITE_HP); break;
        case CREATURE_ELITE_WORLDBOSS: healthMod = sWorld->getRate(RATE_CREATURE_ELITE_WORLDBOSS_HP); break;
        case CREATURE_ELITE_RARE:      healthMod = sWorld->getRate(RATE_CREATURE_ELITE_RARE_HP); break;
        default:                       healthMod = sWorld->getRate(RATE_CREATURE_ELITE_ELITE_HP); break;
    }

    uint32 const baseHp = std::max<uint32>(1, stats->GenerateHealth(cinfo));
    float const baseDamage = stats->GenerateBaseDamage(cinfo);

    out.Health = uint32(baseHp * healthMod);
    out.Mana   = stats->GenerateMana(cinfo);
    out.Armor  = stats->GenerateArmor(cinfo);

    // SelectLevel sets min = base and max = base * 1.5 for all three attack
    // types. _GetDamageMod is deliberately NOT applied: SelectLevel does not
    // apply it either, it is used elsewhere.
    out.MinDamage = baseDamage;
    out.MaxDamage = baseDamage * 1.5f;

    out.AttackPower       = stats->AttackPower;
    out.RangedAttackPower = stats->RangedAttackPower;

    return true;
}

void ZoneScalingMgr::LoadFromDB()
{
    _bands.clear();
    _excluded.clear();
    _forced.clear();
    _tablesPresent = false;
    _chronoTablePresent = false;

    if (!_enable)
        return;

    LoadBaseStatsCoverage();

    // Bands are the load-bearing table; exclusions are advisory. Treat only a
    // bands failure as fatal to activation.
    bool const bandsOk = LoadBands();
    LoadExclusions();
    LoadForced();

    _tablesPresent = bandsOk;

    // Probed even when Chrono is off, so that `.zs status` can distinguish "the
    // opt-in is switched off" from "the opt-in is switched on but cannot work",
    // and so that turning it on with `.reload config` does not need a restart to
    // find its table. Probed rather than assumed for the reason recorded above
    // TableExists: a SELECT against a missing table calls ABORT() and takes the
    // worldserver down.
    _chronoTablePresent = CharacterTableExists("mod_zone_scaling_character");

    if (_chronoEnable && !_chronoTablePresent)
        LOG_ERROR("module.zonescaling",
            "ZoneScaling.Chrono.Enable = 1 but table mod_zone_scaling_character is missing from "
            "the characters database, so Chrono Mode is inactive and scaling applies to everyone. "
            "Apply data/sql/db-characters/base/mod_zone_scaling_character.sql. Nothing else is "
            "affected.");
}

ZoneScalingBand const* ZoneScalingMgr::GetBand(uint32 zoneId, Map const* map) const
{
    if (!IsActive())
        return nullptr;

    auto const itr = _bands.find(zoneId);
    if (itr != _bands.end())
        return itr->second.Enabled ? &itr->second : nullptr;

    // An explicit row always wins over the default, including a disabled one:
    // "enabled = 0" means "specifically not this zone", not "fall through".

    // The default band is an open-world statement, and applying it to an
    // instanced map is the failure the breadth rules exist to prevent: with
    // DefaultBehaviour = clamp, enabling Maps.Raid would scale not just the
    // maps an administrator meant but every raid on the realm, Naxxramas
    // included. Enabling a map kind says "these maps MAY scale"; a row in
    // mod_zone_scaling_zone says which ones do.
    if (_instancedRequireExplicit && map && ResolveMapKind(map) != ZS_MAP_WORLD)
        return nullptr;

    if (_defaultBehaviour == ZS_DEFAULT_CLAMP)
        return &_defaultBand;

    return nullptr;
}

Optional<uint8> ZoneScalingMgr::GetEffectiveLevel(uint32 zoneId, uint8 playerLevel,
                                                  uint8 canonicalLevel, uint8 rank) const
{
    ZoneScalingBand const* band = GetBand(zoneId);
    if (!band)
        return std::nullopt;

    // TargetOffset first, band second: a level-5 player with an offset of 4
    // asks for level 1, and a 10-60 band still answers 10. The offset expresses
    // "a little below me"; the band floor expresses "never below this", and
    // the floor is the harder statement.
    uint8 banded = band->Clamp(TargetLevelFor(playerLevel));

    // The elite offset lifts the presented level above the band, so that a rare
    // elite still cons dangerous after scaling has flattened everything around
    // it. Applied to the banded level and before the direction rule: it is part
    // of "what level does this creature present at", not an exception to it.
    //
    // Bounded by LowestMaxUsableLevel rather than by the band, and that bound is
    // the reason this is not just an addition. The offset deliberately pushes
    // past the band ceiling, so nothing else stops it walking off the end of
    // creature_classlevelstats -- where the rows exist but hold placeholder
    // values, and a lookup succeeds while returning nonsense. See HasBaseStats.
    if (uint8 const offset = EliteOffsetFor(zoneId, canonicalLevel, rank))
    {
        uint8 const raised = uint8(std::min<uint32>(uint32(banded) + uint32(offset),
                                                    uint32(LowestMaxUsableLevel())));

        // Only ever upward. The ceiling clamp can sit *below* the banded level
        // -- a band running to 80 against a usable ceiling of 73 is the obvious
        // case, and the build plan already warns a band cannot safely exceed 73
        // -- and taking the clamped value unconditionally would then present an
        // elite at 73 while the normal mobs beside it present at 80. An offset
        // that makes a creature weaker than its neighbours is worse than no
        // offset at all: it inverts the exact signal it exists to preserve.
        if (raised > banded)
            banded = raised;
    }

    // The band clamp comes first; the direction rule is then applied to its
    // result, and the order is not interchangeable. Take a 10-60 zone holding a
    // canonical level-63 mob, with a level-70 player: clamping first gives
    // banded = 60, and the rule keeps the mob at 63. Applying the rule first
    // gives max(63, 70) = 70, which the clamp then drags down to 60 -- lowering
    // a creature, which is precisely what this mode exists to prevent.
    //
    // Nothing above the band ceiling is raised, either way. A band maximum
    // still means "this zone never scales anything past here"; it just stops
    // being a level *reduction* for creatures that were already past it.
    ZoneScalingDirection const direction =
        band->Direction == ZS_DIR_INHERIT ? _direction : band->Direction;

    if (direction == ZS_DIR_UP && canonicalLevel > banded)
        return canonicalLevel;

    return banded;
}

ZoneScalingExclusion ZoneScalingMgr::GetExclusion(CreatureTemplate const* cinfo) const
{
    if (!cinfo)
        return ZS_EXCL_TABLE;

    if (_excluded.find(cinfo->Entry) != _excluded.end())
        return ZS_EXCL_TABLE;

    // Checked before the rank rule rather than after, so that an entry listed
    // in both tables stays excluded -- mod_zone_scaling_exclude is the harder
    // statement of the two and should win.
    if (cinfo->rank == CREATURE_ELITE_WORLDBOSS && _forced.find(cinfo->Entry) == _forced.end())
        return ZS_EXCL_WORLDBOSS;

    if (cinfo->type == CREATURE_TYPE_CRITTER)
        return ZS_EXCL_CRITTER;

    if (cinfo->type == CREATURE_TYPE_TOTEM)
        return ZS_EXCL_TOTEM;

    if (cinfo->flags_extra & CREATURE_FLAG_EXTRA_TRIGGER)
        return ZS_EXCL_TRIGGER;

    // The most fundamental case, and checked before the service-NPC flags
    // because it is the more informative answer: a player can never fight this,
    // so its level is decoration. Catches spirit healers, spirit guides and
    // most friendly scenery, none of which carry any other distinguishing flag.
    if (cinfo->unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC))
        return ZS_EXCL_UNATTACKABLE;

    // Friendly service NPCs. Scaling these achieves nothing and risks making a
    // quest giver look hostile-tier to a low-level player.
    if (cinfo->npcflag & UNIT_NPC_FLAG_QUESTGIVER)
        return ZS_EXCL_QUESTGIVER;

    if (cinfo->npcflag & UNIT_NPC_FLAG_VENDOR_MASK)
        return ZS_EXCL_VENDOR;

    if (cinfo->npcflag & UNIT_NPC_FLAG_TRAINER)
        return ZS_EXCL_TRAINER;

    // Anything that grants no XP is scenery, a training dummy, or an event
    // prop. Nothing is gained by making it level with the player.
    if (cinfo->flags_extra & CREATURE_FLAG_EXTRA_NO_XP)
        return ZS_EXCL_NO_XP;

    return ZS_NOT_EXCLUDED;
}

std::string ZoneScalingMgr::DescribeBand(uint32 zoneId) const
{
    std::ostringstream out;

    if (!_enable)
        return "module disabled (ZoneScaling.Enable = 0)";

    if (!_tablesPresent)
        return "inactive: mod_zone_scaling_zone not loaded";

    auto const itr = _bands.find(zoneId);
    if (itr == _bands.end())
    {
        out << "zone " << zoneId << ": no row; ";

        if (_defaultBehaviour == ZS_DEFAULT_CLAMP)
            out << "DefaultBehaviour = clamp, so band "
                << uint32(_defaultBand.MinLevel) << "-" << uint32(_defaultBand.MaxLevel);
        else
            out << "DefaultBehaviour = off, so not scaled";

        return out.str();
    }

    out << "zone " << zoneId << ": band " << uint32(itr->second.MinLevel) << "-"
        << uint32(itr->second.MaxLevel)
        << (itr->second.Enabled ? " (enabled)" : " (disabled by row, overrides the default)");

    return out.str();
}

std::string ZoneScalingMgr::DescribeStatus() const
{
    std::ostringstream out;

    out << "mod-zone-scaling: " << (_enable ? "enabled" : "DISABLED")
        << ", tables " << (_tablesPresent ? "loaded" : "NOT loaded")
        << ", " << _bands.size() << " band(s), " << _excluded.size() << " exclusion(s)"
        << ", " << _forced.size() << " forced boss(es)"
        << ", default " << (_defaultBehaviour == ZS_DEFAULT_CLAMP ? "clamp" : "off");

    if (_defaultBehaviour == ZS_DEFAULT_CLAMP)
        out << " " << uint32(_defaultBand.MinLevel) << "-" << uint32(_defaultBand.MaxLevel);

    // Listed by name rather than as five on/off flags. The question an operator
    // asks here is "what is this touching", and a list of what is enabled
    // answers it in one glance; five flags make them read all five.
    out << ", maps ";
    {
        bool first = true;
        for (uint8 kind = 0; kind < ZS_MAP_KIND_MAX; ++kind)
            if (_mapKindEnabled[kind])
            {
                out << (first ? "" : "+") << ZoneScalingMapKindName(ZoneScalingMapKind(kind));
                first = false;
            }

        if (first)
            out << "NONE (nothing can scale anywhere)";
    }

    out << ", elite offset "
        << (_preserveEliteOffset ? "+" + std::to_string(uint32(_eliteOffsetMax)) + " max" : "off");

    out << ", target offset " << (_targetOffset ? "-" + std::to_string(uint32(_targetOffset)) : "0");

    // Spelled out rather than reported as a flag, because "chrono on" alone
    // does not say what it does to a player who has never toggled -- and that
    // is the entire population on the day it is switched on.
    out << ", chrono ";
    if (!_chronoEnable)
        out << "off (everyone scales)";
    else if (!_chronoTablePresent)
        out << "ON but TABLE MISSING (everyone scales)";
    else
    {
        out << "on, default " << (_chronoDefault ? "opted in" : "opted OUT");
        if (_chronoBotDefault >= 0)
            out << ", bots default " << (_chronoBotDefault ? "opted in" : "opted OUT");
    }

    out << ", direction "
        << (_direction == ZS_DIR_UP ? "up (level-up only)" : "both (full scaling)")
        << ", canonical " << (_canonical == ZS_CANON_MIDPOINT ? "midpoint" : "native")
        << ", damage scaling " << (_scaleDamage ? "ON" : "off")
        << ", hit-chance scaling " << (_scaleHitChance ? "ON" : "off")
        << ", level display " << (_scaleLevelDisplay ? "ON" : "off")
        << ", pool display " << (_scalePoolDisplay ? "ON" : "off")
        << ", core view " << (!_coreViewInstalled ? "ABSENT (core unpatched)" : _coreView ? "ON" : "off")
        << ", aggro " << (_aggroMode == ZS_AGGRO_NORMALIZE ? "normalize" : "stock")
        << ", " << GetTaggedCount() << " creature(s) tagged";

    // The state blocks only. Reported because "how much is this module costing
    // me" is otherwise unanswerable from outside: worldserver RSS moves by
    // hundreds of MB on its own as grids load and unload, so it cannot be
    // differenced to find this number. The DataMap node holding each block adds
    // its own overhead on top, of the same order.
    out << " (" << (uint64(GetTaggedCount()) * sizeof(ZoneScalingCreatureState)) / 1024
        << " KiB of state, " << sizeof(ZoneScalingCreatureState) << " B each)";

    out << ". Since startup: " << GetDamageScaledCount() << " damage event(s) scaled, "
        << GetHitTableFixedCount() << " melee hit-table correction(s), "
        << GetLevelPatchedCount() << " level field(s) patched";

    // The Z-05 acceptance criterion, reported rather than assumed. Served
    // counts observers, tracked counts the buffer builds behind them, so their
    // difference is exactly the work the core's per-observer values cache
    // saved. A rate near zero does not indict this module -- the patch runs
    // after the buffer is appended and cannot prevent a hit -- but it does say
    // the cache is not coalescing observers, and that the per-observer patch is
    // being paid in full every time.
    if (uint64 const served = GetLevelServedCount())
        out << " (" << served << " observer(s) served by "
            << GetLevelTrackedCount() << " buffer build(s), "
            << (100 - std::min<uint64>(100, GetLevelTrackedCount() * 100 / served))
            << "% values-cache hit rate)";

    // After the parenthetical, deliberately: that clause qualifies the level
    // count, and anything inserted between the two would read as qualifying the
    // aggro count instead.
    out << ", " << GetAggroAdjustedCount() << " aggro range(s) adjusted, "
        << GetEliteOffsetCount() << " elite offset(s) applied";

    // Only when Chrono is active. A permanent ", 0 suppressed" on a realm
    // without the opt-in reads as a feature that is failing rather than one
    // that is switched off.
    if (IsChronoActive())
        out << ", " << GetChronoSuppressedCount() << " scaled fight(s) suppressed by Chrono, "
            << GetChronoToggledCount() << " toggle(s)";

    // Reported unconditionally when XP scaling is on, and the two kill numbers
    // are reported separately on purpose: they come from different mechanisms
    // with different risk, and "corrected" staying at 0 while "topped up"
    // climbs is the signature of a band that only ever scales upward past the
    // gray line. Reading one merged total would hide that.
    if (_xpEnable)
    {
        out << ", " << GetXpKillAdjustedCount() << " kill XP award(s) corrected";

        if (_xpGreyKills)
            out << ", " << GetXpGreyPaidCount() << " grey kill(s) topped up ("
                << GetXpGreyAmount() << " XP)";

        if (_xpQuests)
            out << ", " << GetXpQuestAdjustedCount() << " quest XP award(s) corrected";

        if (IsChronoXpBonusActive())
            out << ", " << GetXpChronoBonusCount() << " Chrono XP bonus(es) at +"
                << GetChronoXpBonusPct() << "%";
    }

    // The coin delta is signed and is reported rather than folded into the
    // count: under Direction = both this module scales some loot down, and a
    // bare count cannot distinguish a realm paying out sensibly from one
    // quietly draining its economy.
    if (IsLootMoneyScalingEnabled())
        out << ", " << GetLootMoneyScaledCount() << " loot coin pot(s) scaled ("
            << (GetLootMoneyDelta() >= 0 ? "+" : "") << GetLootMoneyDelta() << " copper)";

    if (IsLootGearBandingEnabled())
        out << ", " << GetLootGearBandedCount() << " gear drop(s) rebanded";

    if (_debug)
        out << ", debug on";

    out << ".";

    return out.str();
}

// Owns the module's startup: config first, then the tables that depend on it.
class ZoneScalingWorldScript : public WorldScript
{
public:
    // Name the two hooks used. Passing an empty list would silently opt in to
    // every WorldScript hook, including OnUpdate.
    ZoneScalingWorldScript() : WorldScript("ZoneScalingWorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        sZoneScalingMgr->LoadConfig();

        // On a live `.reload config` the tables are already loaded and the
        // world is populated; re-reading them here keeps the two in step.
        if (reload)
            sZoneScalingMgr->LoadFromDB();
    }

    // The core's designated hook for module-owned tables; it runs during world
    // load, after config, and before any creature exists.
    void OnLoadCustomDatabaseTable() override
    {
        sZoneScalingMgr->LoadFromDB();

        if (!sZoneScalingMgr->IsEnabled())
            LOG_INFO("module.zonescaling",
                "mod-zone-scaling loaded but disabled (ZoneScaling.Enable = 0). "
                "No behaviour is changed.");
        else
            LOG_INFO("module.zonescaling", "{}", sZoneScalingMgr->DescribeStatus());
    }
};

void AddZoneScalingMgrScripts()
{
    new ZoneScalingWorldScript();
}
