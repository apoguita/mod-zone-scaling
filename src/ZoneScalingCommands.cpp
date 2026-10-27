/*
 * Copyright (C) 2026+ mod-zone-scaling contributors, released under GNU AGPL v3
 * license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * The `.zs` command family: the module's only window into what it thinks is
 * true. Every subcommand is read-only except `reload`.
 */

#include "ZoneScaling.h"

#include "Chat.h"
#include "CommandScript.h"
#include "Common.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCEnums.h"
#include "ItemTemplate.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Unit.h"
#include "UnitDefines.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace Acore::ChatCommands;

namespace
{
    // Copper as a player would read it. Only `.zs coin` needs this, and it needs
    // it badly: "x14.0000" is not a claim anyone can sanity-check, while
    // "1g 40s" against "10s" is.
    std::string FormatCopper(uint32 copper)
    {
        uint32 const gold   = copper / 10000;
        uint32 const silver = (copper % 10000) / 100;
        uint32 const rest   = copper % 100;

        if (gold)
            return Acore::StringFormat("{}g {}s {}c", gold, silver, rest);
        if (silver)
            return Acore::StringFormat("{}s {}c", silver, rest);

        return Acore::StringFormat("{}c", rest);
    }
}

class zonescaling_commandscript : public CommandScript
{
public:
    zonescaling_commandscript() : CommandScript("zonescaling_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable zsCommandTable =
        {
            { "status",   HandleZsStatusCommand,   SEC_GAMEMASTER, Console::Yes },
            { "zone",     HandleZsZoneCommand,     SEC_GAMEMASTER, Console::Yes },
            { "info",     HandleZsInfoCommand,     SEC_GAMEMASTER, Console::No  },
            { "level",    HandleZsLevelCommand,    SEC_GAMEMASTER, Console::Yes },
            { "ratio",    HandleZsRatioCommand,    SEC_GAMEMASTER, Console::Yes },
            { "questxp",  HandleZsQuestXpCommand,  SEC_GAMEMASTER, Console::Yes },
            { "coin",     HandleZsCoinCommand,     SEC_GAMEMASTER, Console::Yes },
            { "gear",     HandleZsGearCommand,     SEC_GAMEMASTER, Console::Yes },
            { "aggro",    HandleZsAggroCommand,    SEC_GAMEMASTER, Console::Yes },
            { "sweep",    HandleZsSweepCommand,    SEC_GAMEMASTER, Console::Yes },
            { "excluded", HandleZsExcludedCommand, SEC_GAMEMASTER, Console::Yes },
            { "stat",     HandleZsStatCommand,     SEC_GAMEMASTER, Console::Yes },
            { "reload",   HandleZsReloadCommand,   SEC_ADMINISTRATOR, Console::Yes },
            { "",         HandleZsStatusCommand,   SEC_GAMEMASTER, Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "zs", zsCommandTable }
        };

        return commandTable;
    }

    static bool HandleZsStatusCommand(ChatHandler* handler)
    {
        handler->SendSysMessage(sZoneScalingMgr->DescribeStatus());
        return true;
    }

    // `.zs zone` with no argument reports the caller's current zone, which is
    // the common case while walking a zone checking bands. From the console
    // there is no current zone, so the id is required there.
    static bool HandleZsZoneCommand(ChatHandler* handler, Optional<uint32> zoneId)
    {
        uint32 resolved;

        if (zoneId)
            resolved = *zoneId;
        else if (Player* player = handler->GetPlayer())
            resolved = player->GetZoneId();
        else
        {
            handler->SendSysMessage("Usage: .zs zone <zoneId> (an id is required from the console).");
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->SendSysMessage(sZoneScalingMgr->DescribeBand(resolved));

        // The band alone does not say what a given player would face, which is
        // the question actually being asked most of the time.
        if (Player* player = handler->GetPlayer())
        {
            if (Optional<uint8> effective =
                    sZoneScalingMgr->GetEffectiveLevel(resolved, player->GetLevel()))
            {
                handler->PSendSysMessage("At your level ({}), creatures there would present at level {}.",
                    uint32(player->GetLevel()), uint32(*effective));

                // Under level-up-only that figure is a floor, not the answer:
                // it holds for creatures at or below it, and anything already
                // above it keeps its own level. Say so rather than let the
                // single number read as the whole story.
                if (sZoneScalingMgr->GetDirection() == ZS_DIR_UP)
                    handler->SendSysMessage("Direction = up, so that is the level creatures are raised TO. "
                                            "Any already above it keep their own level.");
            }
        }

        return true;
    }

    // Everything the module believes about one live creature. Selection-only:
    // it reports cached per-creature state, which only exists for a creature
    // actually in the world, and the console has no way to point at one.
    static bool HandleZsInfoCommand(ChatHandler* handler)
    {
        Creature* creature = handler->getSelectedCreature();
        if (!creature)
        {
            handler->SendSysMessage("Select a creature first.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->PSendSysMessage("{} ({}) {}", creature->GetName(), creature->GetEntry(),
            creature->GetGUID().ToString());

        ZoneScalingCreatureState const* state = ZoneScalingMgr::GetState(creature);
        if (!state)
        {
            // Untagged is the common case, and an ambiguous one. Work out
            // which reason applies rather than reporting a bare "no" -- the
            // whole point of this command is to remove guesswork.
            handler->SendSysMessage("Not tagged.");

            if (!sZoneScalingMgr->IsActive())
                handler->SendSysMessage("Reason: the module is not active.");
            else if (creature->IsPet() || creature->IsSummon())
                handler->SendSysMessage("Reason: owned by a player or summoner; scaled through "
                                        "its owner instead.");
            else if (ZoneScalingExclusion const reason =
                         sZoneScalingMgr->GetExclusion(creature->GetCreatureTemplate()))
                handler->PSendSysMessage("Reason: {}.", ZoneScalingExclusionName(reason));
            else if (!sZoneScalingMgr->IsMapAllowed(creature->FindMap()))
                handler->PSendSysMessage("Reason: this is a {} map, and "
                    "ZoneScaling.Maps.{} is off.",
                    ZoneScalingMapKindName(ZoneScalingMgr::ResolveMapKind(creature->FindMap())),
                    ZoneScalingMapKindName(ZoneScalingMgr::ResolveMapKind(creature->FindMap())));
            else if (!sZoneScalingMgr->GetBand(creature->GetZoneId()))
                handler->PSendSysMessage("Reason: {}", sZoneScalingMgr->DescribeBand(creature->GetZoneId()));
            else
                handler->SendSysMessage("Reason: it qualifies now but was already in the world "
                                        "when that became true. Tagging happens on entering the "
                                        "world, so it will be picked up on respawn or the next "
                                        "time its grid reloads.");

            return true;
        }

        handler->PSendSysMessage("zone {} on a {} map, canonical level {} (live level {}), {}",
            state->ZoneId, ZoneScalingMapKindName(state->MapKind),
            uint32(state->CanonicalLevel), uint32(creature->GetLevel()),
            state->Scalable ? "SCALABLE" : "not scaled");

        if (uint8 const offset = sZoneScalingMgr->EliteOffsetFor(
                state->ZoneId, state->CanonicalLevel, state->Rank))
            handler->PSendSysMessage("Elite offset: presents +{} above the band (rank {}).",
                uint32(offset), uint32(state->Rank));

        if (state->Exclusion != ZS_NOT_EXCLUDED)
            handler->PSendSysMessage("Excluded: {}.", ZoneScalingExclusionName(state->Exclusion));

        handler->SendSysMessage(sZoneScalingMgr->DescribeBand(state->ZoneId));

        if (!state->StatsValid)
            handler->SendSysMessage("Canonical stats UNUSABLE -- no creature_classlevelstats "
                                    "data at this level for its class and expansion.");
        else
            handler->PSendSysMessage("Canonical stats: HP {}, mana {}, melee {:.0f}-{:.0f}, AP {}",
                state->CanonicalStats.Health, state->CanonicalStats.Mana,
                state->CanonicalStats.MinDamage, state->CanonicalStats.MaxDamage,
                state->CanonicalStats.AttackPower);

        // The one place the aggro offset is directly observable on a real
        // creature rather than predicted. Both numbers, because the interesting
        // question is whether they differ and by how much.
        if (CreatureTemplate const* cinfo = creature->GetCreatureTemplate())
        {
            float const live = creature->GetDetectionRange();

            handler->PSendSysMessage("Detection range: {:.1f} yd (template {:.1f} yd){}",
                live, cinfo->detection_range,
                live == cinfo->detection_range ? "" : " -- offset by this module");

            if (sZoneScalingMgr->GetAggroMode() == ZS_AGGRO_NORMALIZE
                && live == cinfo->detection_range && state->Scalable)
                handler->SendSysMessage("NOTE: AggroRange = normalize but this creature still "
                                        "carries the template value. Either its level already "
                                        "equals the band midpoint, or it entered the world before "
                                        "the mode was switched on -- it will be adjusted on "
                                        "respawn.");
        }

        // The cache is only useful if it agrees with the creature. Say so
        // loudly when it does not -- a silent divergence here would surface
        // much later as wrong damage numbers with no obvious cause.
        if (state->CanonicalLevel != creature->GetLevel())
            handler->SendSysMessage("WARNING: cached canonical level disagrees with the "
                                    "creature's live level. The refresh hook has been missed.");

        return true;
    }

    // A direct probe of GetEffectiveLevel, the one function every later
    // milestone routes through. It exists because the interesting half of the
    // answer -- what ZoneScaling.Direction does -- depends on a creature's
    // canonical level, and until Z-03 there is no caller that has one. Without
    // this the direction setting could only be verified by playing the game.
    //
    // Pure arithmetic against loaded config: no creature is touched and the
    // zone need not contain one, so any hypothetical can be asked.
    static bool HandleZsLevelCommand(ChatHandler* handler, uint32 zoneId,
                                     uint32 playerLevel, Optional<uint32> canonicalLevel)
    {
        uint32 const canon = canonicalLevel.value_or(0);

        // Both are narrowed to uint8 below, so reject out-of-range input rather
        // than silently truncating it -- level 300 would otherwise answer for
        // level 44, which is the kind of wrong that looks like a real result.
        if (playerLevel == 0 || playerLevel > STRONG_MAX_LEVEL || canon > STRONG_MAX_LEVEL)
        {
            handler->PSendSysMessage("Level out of range (1-{}): player {}, canonical {}.",
                uint32(STRONG_MAX_LEVEL), playerLevel, canon);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Optional<uint8> const effective = sZoneScalingMgr->GetEffectiveLevel(
            zoneId, uint8(playerLevel), uint8(canon));

        if (!effective)
        {
            handler->PSendSysMessage("zone {}: not scaled -- {}",
                zoneId, sZoneScalingMgr->DescribeBand(zoneId));
            return true;
        }

        char const* verdict;
        if (canon == 0)
            verdict = "canonical level not given, so the banded level is the answer";
        else if (*effective == canon)
            verdict = "unchanged";
        else if (*effective > canon)
            verdict = "raised";
        else
            verdict = "lowered";

        handler->PSendSysMessage("zone {}, player level {}, canonical level {} -> presents at {} ({}).",
            zoneId, playerLevel, canon, uint32(*effective), verdict);

        handler->PSendSysMessage("Direction = {}.",
            sZoneScalingMgr->GetDirection() == ZS_DIR_UP ? "up (level-up only)" : "both (full scaling)");

        return true;
    }

    // Z-07's acceptance criterion, run from a console instead of from a
    // gryphon. The milestone asks for a flight over three zones checking that
    // no excluded creature is ever tagged and no quest giver or vendor is
    // touched; flying proves it only for the grids that happened to load along
    // the route, and proves nothing about the spawns behind you. This walks
    // every spawn point in the world DB for a map, classifies each one exactly
    // as the live path would, and reports what it found -- so the answer covers
    // the whole zone rather than a flight path, and can be re-run after any
    // exclusion-table edit.
    //
    // Costs a synchronous pass over the spawn table on the world thread, and
    // zone resolution reads terrain, so the first run on a large continent can
    // stall a tick. It reports its own elapsed time for that reason. Scoped to
    // one map deliberately: sMapMgr->GetZoneId instantiates a base map for
    // whatever id it is handed, and a sweep over every map would create all of
    // them as a side effect of a read-only diagnostic.
    static bool HandleZsSweepCommand(ChatHandler* handler, uint32 mapId,
                                     Optional<uint32> zoneFilter)
    {
        Map* map = sMapMgr->CreateBaseMap(mapId);
        if (!map)
        {
            handler->PSendSysMessage("No map with id {}.", mapId);
            handler->SetSentErrorMessage(true);
            return false;
        }

        ZoneScalingMapKind const kind = ZoneScalingMgr::ResolveMapKind(map);
        bool const kindAllowed = sZoneScalingMgr->IsMapKindEnabled(kind);

        uint32 total = 0;
        uint32 noTemplate = 0;
        uint32 scalable = 0;
        uint32 unbanded = 0;

        // Its own bucket, and it has to be: without it these creatures fall
        // into the "excluded by rule" remainder, which then disagrees with the
        // per-reason breakdown printed directly beneath it. Deadmines reported
        // 213 excluded against a breakdown listing 11.
        uint32 mapGated = 0;
        uint32 byReason[ZS_EXCL_UNATTACKABLE + 1] = {};

        // The whole point of the sweep. These are the categories the milestone
        // names explicitly, so they are counted separately rather than inferred
        // from the totals: a non-zero value here is a failure, not a statistic.
        uint32 leakedQuestVendorTrainer = 0;
        uint32 leakedWorldBoss = 0;

        uint32 const start = getMSTime();

        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            if (data.mapid != mapId)
                continue;

            uint32 const zoneId = map->GetZoneId(data.phaseMask, data.posX, data.posY, data.posZ);

            if (zoneFilter && zoneId != *zoneFilter)
                continue;

            ++total;

            CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(data.id);
            if (!cinfo)
            {
                ++noTemplate;
                continue;
            }

            ZoneScalingExclusion const reason = sZoneScalingMgr->GetExclusion(cinfo);
            ++byReason[reason];

            if (reason != ZS_NOT_EXCLUDED)
                continue;

            if (!sZoneScalingMgr->GetBand(zoneId))
            {
                ++unbanded;
                continue;
            }

            if (!kindAllowed)
            {
                ++mapGated;
                continue;
            }

            // Reached only by creatures the live path would tag and scale.
            ++scalable;

            if (cinfo->npcflag & (UNIT_NPC_FLAG_QUESTGIVER | UNIT_NPC_FLAG_VENDOR
                                  | UNIT_NPC_FLAG_TRAINER))
                ++leakedQuestVendorTrainer;

            if (cinfo->rank == CREATURE_ELITE_WORLDBOSS)
                ++leakedWorldBoss;
        }

        handler->PSendSysMessage("map {} ({}) {}: {} spawn(s) examined in {} ms.",
            mapId, ZoneScalingMapKindName(kind),
            zoneFilter ? Acore::StringFormat("zone {}", *zoneFilter) : std::string("all zones"),
            total, GetMSTimeDiffToNow(start));

        if (!total)
        {
            handler->SendSysMessage("Nothing to report. Check the map and zone ids.");
            return true;
        }

        if (!kindAllowed)
            handler->PSendSysMessage("|cffffff00This map kind is disabled "
                "(ZoneScaling.Maps.{} = 0), so nothing here scales at all.|r",
                ZoneScalingMapKindName(kind));

        handler->PSendSysMessage("WOULD SCALE: {} ({} left alone: {} excluded by rule, "
                                 "{} blocked by the map-kind gate, {} in unbanded zones, "
                                 "{} with no template)",
            scalable, total - scalable,
            total - scalable - mapGated - unbanded - noTemplate,
            mapGated, unbanded, noTemplate);

        for (uint8 reason = ZS_EXCL_TABLE; reason <= ZS_EXCL_UNATTACKABLE; ++reason)
            if (byReason[reason])
                handler->PSendSysMessage("  {:5} {}", byReason[reason],
                    ZoneScalingExclusionName(ZoneScalingExclusion(reason)));

        // Stated as a verdict rather than left for the reader to derive. The
        // command exists to answer one question, so it answers it.
        if (leakedQuestVendorTrainer || leakedWorldBoss)
        {
            handler->PSendSysMessage("|cffff0000FAIL: {} quest giver/vendor/trainer and {} world "
                "boss spawn(s) would be scaled. The exclusion rules have a hole.|r",
                leakedQuestVendorTrainer, leakedWorldBoss);
        }
        else
            handler->SendSysMessage("|cff00ff00PASS: no quest giver, vendor, trainer or world "
                                    "boss would be scaled.|r");

        return true;
    }

    // Proximity aggro for a hypothetical pairing, as three numbers: what the
    // core does today, what normalization would make it, and what it ought to
    // be. This command exists because Z-06's real goal is out of reach and the
    // honest response to that is to make the shortfall measurable rather than
    // to describe it. The residual it prints is the module's own error bar.
    static bool HandleZsAggroCommand(ChatHandler* handler, uint32 entry, uint32 zoneId,
                                     uint32 playerLevel, Optional<uint32> canonicalLevel)
    {
        CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(entry);
        if (!cinfo)
        {
            handler->PSendSysMessage("No creature template with entry {}.", entry);
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 const canon = canonicalLevel.value_or(cinfo->minlevel);

        if (playerLevel == 0 || playerLevel > STRONG_MAX_LEVEL
            || canon == 0 || canon > STRONG_MAX_LEVEL)
        {
            handler->PSendSysMessage("Level out of range (1-{}): player {}, canonical {}.",
                uint32(STRONG_MAX_LEVEL), playerLevel, canon);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Optional<uint8> const effective = sZoneScalingMgr->GetEffectiveLevel(
            zoneId, uint8(playerLevel), uint8(canon), cinfo->rank);

        if (!effective)
        {
            handler->PSendSysMessage("zone {}: not scaled -- {}",
                zoneId, sZoneScalingMgr->DescribeBand(zoneId));
            return true;
        }

        float const templateRange = cinfo->detection_range;

        // Stock: the level term reads the creature's real level.
        float const stock =
            sZoneScalingMgr->PredictAggroRadius(templateRange, uint8(playerLevel), uint8(canon));

        // Ideal: the level term reads the level the player is actually fighting,
        // which under full scaling is their own -- so the term vanishes.
        float const ideal =
            sZoneScalingMgr->PredictAggroRadius(templateRange, uint8(playerLevel), *effective);

        Optional<float> const adjusted =
            sZoneScalingMgr->NormalizedDetectionRange(cinfo, zoneId, uint8(canon));

        float const normalized = sZoneScalingMgr->PredictAggroRadius(
            adjusted.value_or(templateRange), uint8(playerLevel), uint8(canon));

        handler->PSendSysMessage(
            "entry {} '{}' in zone {}: canonical level {}, presents at {}, template detection range {:.1f} yd.",
            entry, cinfo->Name, zoneId, canon, uint32(*effective), templateRange);

        handler->PSendSysMessage("  stock aggro radius:      {:.1f} yd", stock);
        handler->PSendSysMessage("  normalized aggro radius: {:.1f} yd{}", normalized,
            sZoneScalingMgr->GetAggroMode() == ZS_AGGRO_NORMALIZE
                ? "" : "  (AggroRange = stock, so this is not in effect)");
        handler->PSendSysMessage("  ideal aggro radius:      {:.1f} yd", ideal);

        // The measured gap between the two numbers printed above, not the
        // formula's |effectiveLevel - midpoint|. The formula is right in the
        // open, but the core's [5, 45] clamps can swallow some or all of it --
        // at the 5-yard floor every pairing reads 5 and the true residual is
        // zero. Printing a claim the three radii above it contradict is exactly
        // what a real bug looks like, so print what is actually true here and
        // give the general rule as the rule.
        if (Optional<uint8> const midpoint = sZoneScalingMgr->GetBandMidpoint(zoneId))
        {
            handler->PSendSysMessage(
                "Band midpoint {}. Normalization removes the creature's level from the answer: "
                "every creature in this zone gives the same radius against a given player, "
                "whatever its own level. It cannot remove the player's level; no module can.",
                uint32(*midpoint));

            handler->PSendSysMessage(
                "Residual against ideal for this pairing: {:.1f} yd "
                "(generally |effective level - midpoint| = {} yd, before the core's 5/45 clamps).",
                std::fabs(normalized - ideal),
                uint32(std::abs(int32(*effective) - int32(*midpoint))));
        }

        if (std::abs(stock - ideal) < 0.05f)
            handler->PSendSysMessage("Stock already matches the ideal for this pairing; nothing to fix.");

        return true;
    }

    // The combat multipliers for a hypothetical pairing. Like `.zs level` this
    // is pure arithmetic against loaded config and the stat tables -- no
    // creature needs to exist in the zone, and no fight needs to happen. It is
    // the only way to falsify Z-04's numbers without two accounts in-game, so
    // it reports the inputs as well as the answer.
    // `.zs questxp <questId> <playerLevel>`
    //
    // The quest half of Z-09, checked without a client. This calls the same
    // function `OnPlayerQuestComputeXP` calls, so what it prints is what the
    // hook would decide -- see the comment on ZoneScalingDescribeQuestXp for
    // why that sharing is load-bearing rather than tidy.
    //
    // It cannot prove the hook fires. It proves everything downstream of the
    // hook firing.
    static bool HandleZsQuestXpCommand(ChatHandler* handler, uint32 questId, uint32 playerLevel)
    {
        if (playerLevel == 0 || playerLevel > STRONG_MAX_LEVEL)
        {
            handler->PSendSysMessage("Player level out of range (1-{}): {}.",
                uint32(STRONG_MAX_LEVEL), playerLevel);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
        {
            handler->PSendSysMessage("No quest template with id {}.", questId);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // Deliberately no player: this asks about a level, not a person, so it
        // answers the same for anyone and cannot be confused by whose Chrono
        // flag happened to be consulted.
        ZoneScalingQuestXp describe;
        bool const adjusts =
            ZoneScalingDescribeQuestXp(quest, nullptr, uint8(playerLevel), describe);

        handler->PSendSysMessage("'{}' (quest {}): level {}, zone/sort {}, xp difficulty {}.",
            quest->GetTitle(), questId, describe.QuestLevel, describe.ZoneOrSort,
            quest->GetXPId());

        if (!adjusts)
        {
            handler->PSendSysMessage("  not corrected for a level {} player: {}.",
                playerLevel, describe.Declined ? describe.Declined : "unknown");
            return true;
        }

        // The stock award alongside the ratio, so the line can be checked
        // against what a player would actually see rather than only against
        // itself.
        uint32 const stock = quest->XPValue(uint8(playerLevel));
        uint64 const corrected = describe.CanonicalXp
                               ? uint64(stock) * describe.EffectiveXp / describe.CanonicalXp : 0;

        handler->PSendSysMessage("  for a level {} player: priced at {} instead of {}.",
            playerLevel, uint32(describe.Effective), describe.QuestLevel);
        handler->PSendSysMessage("  QuestXP.dbc term {} -> {} (x{:.4f}), award {} -> {}.",
            describe.CanonicalXp, describe.EffectiveXp,
            float(describe.EffectiveXp) / float(describe.CanonicalXp),
            stock, uint32(corrected));

        if (sZoneScalingMgr->IsChronoXpBonusActive())
            handler->PSendSysMessage("  a Chrono opt-in would then receive +{}%: {}.",
                sZoneScalingMgr->GetChronoXpBonusPct(),
                uint32(corrected * (100 + sZoneScalingMgr->GetChronoXpBonusPct()) / 100));

        return true;
    }

    // `.zs coin <canonicalLevel> <effectiveLevel>`
    //
    // The coin curve, shown at two levels with the multiplier between them and
    // what it would do to a sample pot. Exists because the multiplier is the one
    // number in Z-10 an administrator is likely to disagree with -- it is read
    // from `quest_money_reward`, which inflates faster past level 60 than
    // creature coin does -- and disagreeing with it productively requires seeing
    // it before switching the feature on.
    static bool HandleZsCoinCommand(ChatHandler* handler, uint32 canonicalLevel,
                                    uint32 effectiveLevel)
    {
        if (!canonicalLevel || canonicalLevel > STRONG_MAX_LEVEL
            || !effectiveLevel || effectiveLevel > STRONG_MAX_LEVEL)
        {
            handler->PSendSysMessage("Levels out of range (1-{}).", uint32(STRONG_MAX_LEVEL));
            handler->SetSentErrorMessage(true);
            return false;
        }

        Optional<float> const multiplier =
            sZoneScalingMgr->DescribeMoneyMultiplier(uint8(canonicalLevel), uint8(effectiveLevel));

        if (!multiplier)
        {
            handler->PSendSysMessage(
                "No quest_money_reward row at level {} or {}; coin would be left alone.",
                canonicalLevel, effectiveLevel);
            return true;
        }

        handler->PSendSysMessage("Coin, level {} -> {}: x{:.4f} (clamp {:.1f}).",
            canonicalLevel, effectiveLevel, *multiplier, sZoneScalingMgr->GetLootMoneyMax());

        // A worked example, because a bare multiplier is hard to sanity-check
        // and the copper formatter makes the answer legible at a glance.
        for (uint32 sample : { uint32(100), uint32(1000), uint32(10000) })
            handler->PSendSysMessage("  {} -> {}.",
                FormatCopper(sample), FormatCopper(uint32(std::lround(sample * *multiplier))));

        if (!sZoneScalingMgr->IsLootMoneyScalingEnabled())
            handler->PSendSysMessage("  (coin scaling is currently off.)");

        return true;
    }

    // `.zs gear <itemId> <effectiveLevel>`
    //
    // The gear half of Z-10, checked without killing anything. Calls the same
    // function the loot hook calls, for the same reason `.zs questxp` does.
    //
    // Note that the answer is *not* stable across invocations: the substitution
    // picks uniformly from every candidate in the allowed level band, so running it
    // twice
    // gives two different items. That is the feature working, not the command
    // being flaky, and the candidate count is printed so the spread is visible.
    static bool HandleZsGearCommand(ChatHandler* handler, uint32 itemId, uint32 effectiveLevel)
    {
        if (!effectiveLevel || effectiveLevel > STRONG_MAX_LEVEL)
        {
            handler->PSendSysMessage("Effective level out of range (1-{}).",
                uint32(STRONG_MAX_LEVEL));
            handler->SetSentErrorMessage(true);
            return false;
        }

        ZoneScalingGearSwap swap;
        bool const swaps = ZoneScalingDescribeGearSwap(itemId, uint8(effectiveLevel), swap);

        ItemTemplate const* from = sObjectMgr->GetItemTemplate(itemId);

        handler->PSendSysMessage("Item {} ({}): required level {}.",
            itemId, from ? from->Name1 : "unknown", uint32(swap.FromLevel));

        if (!swaps)
        {
            handler->PSendSysMessage("  not rebanded at level {}: {}.",
                effectiveLevel, swap.Declined ? swap.Declined : "unknown");
            return true;
        }

        ItemTemplate const* to = sObjectMgr->GetItemTemplate(swap.ToItem);

        handler->PSendSysMessage("  at level {} it would become {} ({}), required level {}.",
            effectiveLevel, swap.ToItem, to ? to->Name1 : "unknown", uint32(swap.ToLevel));
        uint32 const minRequiredLevel = effectiveLevel > sZoneScalingMgr->GetLootGearTolerance()
            ? effectiveLevel - sZoneScalingMgr->GetLootGearTolerance() : 1;
        handler->PSendSysMessage("  chosen at random from {} candidate(s) of the same slot, "
            "subclass and quality, required level {}-{}.",
            swap.Candidates, minRequiredLevel, effectiveLevel);

        if (!sZoneScalingMgr->IsLootGearBandingEnabled())
            handler->PSendSysMessage("  (gear banding is currently off.)");

        return true;
    }

    static bool HandleZsRatioCommand(ChatHandler* handler, uint32 entry, uint32 zoneId,
                                     uint32 playerLevel, Optional<uint32> canonicalLevel)
    {
        CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(entry);
        if (!cinfo)
        {
            handler->PSendSysMessage("No creature template with entry {}.", entry);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // Default to the template's own minimum rather than to 0. Unlike
        // `.zs level`, a canonical level of 0 is meaningless here: there is no
        // stat row for it, so the ratios would have nothing to divide by.
        uint32 const canon = canonicalLevel.value_or(cinfo->minlevel);

        if (playerLevel == 0 || playerLevel > STRONG_MAX_LEVEL
            || canon == 0 || canon > STRONG_MAX_LEVEL)
        {
            handler->PSendSysMessage("Level out of range (1-{}): player {}, canonical {}.",
                uint32(STRONG_MAX_LEVEL), playerLevel, canon);
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint8 effective = 0;
        Optional<ZoneScalingRatios> const ratios = sZoneScalingMgr->DescribeRatios(
            cinfo, zoneId, uint8(canon), uint8(playerLevel), effective);

        if (!ratios)
        {
            // Two very different causes share this return, so separate them --
            // "the zone is not scaled" is configuration, "no usable stats" is a
            // data ceiling and needs a different fix entirely.
            if (!sZoneScalingMgr->GetEffectiveLevel(zoneId, uint8(playerLevel), uint8(canon)))
                handler->PSendSysMessage("zone {}: not scaled -- {}",
                    zoneId, sZoneScalingMgr->DescribeBand(zoneId));
            else
                handler->PSendSysMessage(
                    "No usable creature_classlevelstats for entry {} (class {}, expansion {}) "
                    "at level {} or {}. Nothing would be scaled.",
                    entry, uint32(cinfo->unit_class), uint32(cinfo->expansion),
                    canon, uint32(effective));

            return true;
        }

        handler->PSendSysMessage("'{}' (entry {}) in zone {}, canonical level {} -> presents at {} "
                                 "for a level {} player.",
            cinfo->Name, entry, zoneId, canon, uint32(effective), playerLevel);

        handler->PSendSysMessage("  creature -> player  x{:.4f}  (damage curve)",
            ratios->Incoming);
        handler->PSendSysMessage("  player -> creature  x{:.4f}  (health curve, inverted)",
            ratios->Outgoing);

        if (ratios->IsIdentity())
            handler->SendSysMessage("  identity: this pairing is left completely alone.");

        // A worked example is worth more than the multiplier here, because the
        // multiplier alone gives no sense of whether the fight is winnable.
        ZoneScalingStats canonStats;
        if (sZoneScalingMgr->ComputeStats(cinfo, uint8(canon), canonStats))
        {
            uint32 const perceivedHp =
                ZoneScalingMgr::ApplyRatio(canonStats.Health, 1.0f / ratios->Outgoing);

            handler->PSendSysMessage("  real health pool {} -> feels like {}.",
                canonStats.Health, perceivedHp);
        }

        if (!sZoneScalingMgr->IsDamageScalingEnabled())
            handler->SendSysMessage("  |cffff8080ZoneScaling.ScaleDamage is off -- "
                                    "nothing above is being applied.|r");

        return true;
    }

    // Explains the scalability of a creature template: the selected target's
    // by default, or an explicit entry.
    static bool HandleZsExcludedCommand(ChatHandler* handler, Optional<uint32> entry)
    {
        CreatureTemplate const* cinfo = nullptr;

        if (entry)
        {
            cinfo = sObjectMgr->GetCreatureTemplate(*entry);
            if (!cinfo)
            {
                handler->PSendSysMessage("No creature template with entry {}.", *entry);
                handler->SetSentErrorMessage(true);
                return false;
            }
        }
        else if (Unit* target = handler->getSelectedUnit())
        {
            if (Creature* creature = target->ToCreature())
                cinfo = creature->GetCreatureTemplate();
        }

        if (!cinfo)
        {
            handler->SendSysMessage("Select a creature or pass an entry: .zs excluded [entry]");
            handler->SetSentErrorMessage(true);
            return false;
        }

        ZoneScalingExclusion const reason = sZoneScalingMgr->GetExclusion(cinfo);

        if (reason == ZS_NOT_EXCLUDED)
            handler->PSendSysMessage("{} ({}): scalable.", cinfo->Name, cinfo->Entry);
        else
            handler->PSendSysMessage("{} ({}): NOT scaled -- {}.",
                cinfo->Name, cinfo->Entry, ZoneScalingExclusionName(reason));

        return true;
    }

    // Prints what the selected creature WOULD have at each level in a range,
    // without touching it. The point is to make the maths falsifiable before
    // anything depends on it: pick a level, find a real creature of that level
    // elsewhere in the world, and the numbers should agree.
    //
    // Range defaults to the band of the zone the creature is in, since that is
    // the range that will actually be used. With no band, it falls back to the
    // template's own min-max.
    //
    // Argument rule, to keep three optional numbers unambiguous: **if any
    // argument is given, the first is always the entry.** So `.zs stat` reads
    // your selection, and any explicit form starts with an entry. That also
    // makes the whole command usable from the console, where there is no
    // selection to read.
    static bool HandleZsStatCommand(ChatHandler* handler, Optional<uint32> argEntry,
                                    Optional<uint32> argMin, Optional<uint32> argMax)
    {
        CreatureTemplate const* cinfo = nullptr;
        Creature* creature = nullptr;

        if (argEntry)
        {
            cinfo = sObjectMgr->GetCreatureTemplate(*argEntry);
            if (!cinfo)
            {
                handler->PSendSysMessage("No creature template with entry {}.", *argEntry);
                handler->SetSentErrorMessage(true);
                return false;
            }
        }
        else if ((creature = handler->getSelectedCreature()))
            cinfo = creature->GetCreatureTemplate();

        if (!cinfo)
        {
            handler->SendSysMessage("Select a creature or pass an entry: "
                                    ".zs stat [entry] [minLevel] [maxLevel]");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 from;
        uint32 to;

        if (argMin)
        {
            from = *argMin;
            to   = argMax ? *argMax : *argMin;
        }
        else if (ZoneScalingBand const* band =
                     creature ? sZoneScalingMgr->GetBand(creature->GetZoneId()) : nullptr)
        {
            from = band->MinLevel;
            to   = band->MaxLevel;
        }
        else
        {
            from = std::min(cinfo->minlevel, cinfo->maxlevel);
            to   = std::max(cinfo->minlevel, cinfo->maxlevel);
        }

        if (from == 0 || from > to)
        {
            handler->PSendSysMessage("Invalid level range {}-{}.", from, to);
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->PSendSysMessage("{} ({}) -- rank {}, unit_class {}, expansion {}, template level {}-{}",
            cinfo->Name, cinfo->Entry, uint32(cinfo->rank), uint32(cinfo->unit_class),
            uint32(cinfo->expansion), uint32(cinfo->minlevel), uint32(cinfo->maxlevel));

        if (ZoneScalingExclusion const reason = sZoneScalingMgr->GetExclusion(cinfo))
            handler->PSendSysMessage("NOTE: this creature would not be scaled -- {}. "
                "Figures below are hypothetical.", ZoneScalingExclusionName(reason));

        // The strongest available check: when there is a real creature, its own
        // live numbers must equal the computed row at its own level. If they
        // diverge, ComputeStats has drifted from Creature::SelectLevel.
        if (creature)
            handler->PSendSysMessage("Currently in world: level {}, HP {}, mana {}, melee {:.0f}-{:.0f}, AP {}",
                uint32(creature->GetLevel()), creature->GetMaxHealth(),
                creature->GetMaxPower(POWER_MANA),
                creature->GetWeaponDamageRange(BASE_ATTACK, MINDAMAGE),
                creature->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE),
                uint32(creature->GetTotalAttackPowerValue(BASE_ATTACK)));

        handler->SendSysMessage("lvl |       HP |     mana |    melee |   AP |  RAP");

        // A 1-80 band is 80 lines of chat. Step so the table stays readable
        // while always showing both endpoints.
        uint32 const span = to - from;
        uint32 const step = span > 20 ? (span / 15) + 1 : 1;

        for (uint32 level = from; level <= to; level += step)
            PrintStatRow(handler, cinfo, level);

        if ((to - from) % step != 0)
            PrintStatRow(handler, cinfo, to);

        return true;
    }

    static void PrintStatRow(ChatHandler* handler, CreatureTemplate const* cinfo, uint32 level)
    {
        ZoneScalingStats stats;

        if (!sZoneScalingMgr->ComputeStats(cinfo, uint8(level), stats))
        {
            handler->PSendSysMessage("{:3} | no creature_classlevelstats row for class {} at this level",
                level, uint32(cinfo->unit_class));
            return;
        }

        handler->PSendSysMessage("{:3} | {:8} | {:8} | {:4.0f}-{:4.0f} | {:4} | {:4}",
            level, stats.Health, stats.Mana, stats.MinDamage, stats.MaxDamage,
            stats.AttackPower, stats.RangedAttackPower);
    }

    static bool HandleZsReloadCommand(ChatHandler* handler)
    {
        sZoneScalingMgr->LoadFromDB();
        handler->SendSysMessage("mod-zone-scaling: tables reloaded.");
        handler->SendSysMessage(sZoneScalingMgr->DescribeStatus());
        return true;
    }
};

void AddZoneScalingCommandScripts()
{
    new zonescaling_commandscript();
}
