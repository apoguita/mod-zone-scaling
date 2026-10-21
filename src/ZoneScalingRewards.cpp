/*
 * Copyright (C) 2026+ mod-zone-scaling contributors, released under GNU AGPL v3
 * license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * Rewards: paying a player for a fight the module changed.
 *
 * WHY THIS MILESTONE EXISTS AT ALL
 *
 * Every earlier milestone made an outlevelled zone *harder*. On its own that is
 * strictly worse than no module: the player fights a level-40 Defias thug for
 * level-8 experience. Scaling only becomes a feature when the reward moves with
 * the difficulty, and this file is the whole of that.
 *
 * THE CORRECTION IS A RATIO, AND THAT IS NOT A SHORTCUT
 *
 * `Acore::XP::Gain` is a product of terms, and exactly one of them depends on
 * the victim's level: `BaseGain(playerLevel, victimLevel, content)`. Everything
 * else -- the elite doubling, `creature_template.ModExperience`, `RATE_XP_KILL`
 * or the per-battleground rates, the `GetPlayerDamageReq` taper -- is computed
 * without reference to the victim's level, and `KillRewarder` then applies the
 * group rate, the gray-helper penalty and `SPELL_AURA_MOD_XP_PCT` on top. So
 *
 *     corrected = amount * BaseGain(pl, effective) / BaseGain(pl, canonical)
 *
 * arrives at the right answer while preserving every one of those factors
 * *without the module needing to know that any of them exist*. When the core
 * gains a new multiplier, this keeps working. Recomputing the award from
 * scratch would silently drop it.
 *
 * That is the whole of the kill path -- when it can be reached.
 *
 * ⚠ THE CASE THE HOOK CANNOT REACH, AND WHY IT IS THE IMPORTANT ONE
 *
 * `BaseGain` returns 0 when the victim is at or below the player's gray level.
 * `KillRewarder::_RewardXP` wraps the hook in `if (xp)`, and `Player::GiveXP`
 * returns immediately on `xp < 1`. So when the stock award is zero,
 * `OnPlayerGiveXP` never fires at all, and there is no in-place value for a
 * module to correct.
 *
 * That is not an edge case here. It is the module's headline scenario. A
 * level-40 player in Elwynn has a gray level of 31; every creature in the zone
 * is below it. Scaled to 40 they are a real fight, and the stock pipeline pays
 * nothing for any of them. A ratio correction also degenerates in exactly this
 * case, because the denominator is the zero that suppressed the hook.
 *
 * Two upstream levers were checked and are dead ends, both worth recording:
 *
 *   - `FormulaScript::OnBaseGainCalculation` and `OnGainCalculation` are the
 *     natural place to fix this, and both call sites are commented out in
 *     `Formulas.cpp` (`// pussywizard: optimization`). A published module
 *     cannot rely on either.
 *   - `OnPlayerBeforeGetLevelForXPGain` *is* live and does sit in front of the
 *     `BaseGain` call, but it is handed only the player. There is no hook
 *     anywhere in `Unit::Kill` before `RewardPlayerAndGroupAtKill`, so a module
 *     cannot know which victim the level is about to be measured against.
 *     `OnPlayerCreatureKill` and `OnUnitDeath` both fire afterwards.
 *
 * So the gray case is paid as a top-up from `OnUnitDeath`, after the stock
 * pipeline has finished paying zero. This means reimplementing the reward
 * arithmetic and the group eligibility rules, which is a real maintenance
 * liability and the reason `ZoneScaling.Xp.GreyKills` is a separate switch: if
 * a core change breaks this half, an administrator can turn it off and keep the
 * half that rides the hook.
 *
 * The two halves are mutually exclusive by construction, so nothing is ever
 * paid twice. The stock award for a player is zero precisely when the victim's
 * canonical level is gray to *that player* -- if it is not gray, the player is
 * their own eligible reference and `_maxNotGrayMemberLevel >= their level`
 * holds, so `_RewardXP` pays them and the hook fires. One test, `IsGray`,
 * therefore partitions every kill cleanly between the two paths.
 *
 * WHAT THE GROUP CASE COSTS IN ACCURACY
 *
 * In a group the core computes `_xp` once, from the highest member for whom the
 * victim is not gray, and shares it out. The hook hands each member their share,
 * so the ratio's denominator strictly ought to use *that* member's level rather
 * than the receiving player's. This file uses the receiving player's level on
 * purpose: `effective` is itself derived from that player, so the ratio answers
 * "what is this creature worth to me at the level I am fighting it", which is
 * the question a per-player scaling module is built around. In a group of mixed
 * levels the two differ; both are defensible and the difference is small next to
 * the correction itself.
 *
 * QUESTS
 *
 * `Quest::XPValue` reads `QuestXP.dbc` at the quest's own level and tapers by
 * the distance to the player. A quest is corrected by running its level through
 * the same band its zone uses, then taking the same kind of ratio. Two things
 * fall out and are deliberate: a quest with `Level == -1` is left alone, because
 * it already tracks the player and there is nothing to correct; and a quest
 * whose `ZoneOrSort` is negative is a *sort* (a class or profession bucket), not
 * a zone, so there is no band to look up and it is left alone too.
 *
 * QUEST GOLD IS ALREADY CORRECT AND IS NOT TOUCHED
 *
 * Worth stating so nobody re-derives it: `Quest::GetRewOrReqMoney` is called
 * with `GetLevel()` -- the *player's* level -- and looks the reward up in
 * `_questMoneyRewards` by that level. Quest money already scales to the player
 * on stock AzerothCore. There is no hook to change it and no reason to want
 * one. `OnPlayerCompleteQuest` fires from `Player::CompleteQuest`, when the
 * objectives are met, which is before the reward is handed out in any case.
 */

#include "ZoneScaling.h"

#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Formulas.h"
#include "Group.h"
#include "Log.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerScript.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SpellAuraDefines.h"
#include "UnitScript.h"
#include "World.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace
{
    // The gray test, spelled out rather than inferred from a zero `BaseGain`,
    // because the two paths in this file are partitioned on it and a reader has
    // to be able to see the partition. Mirrors `Acore::XP::BaseGain`'s own
    // branch: a victim at or above the player is never gray.
    bool IsGray(uint8 playerLevel, uint8 victimLevel)
    {
        return victimLevel < playerLevel && victimLevel <= Acore::XP::GetGrayLevel(playerLevel);
    }

    // The Chrono opt-in's reward for taking the harder realm, applied in one
    // place for every source of experience the module touches.
    //
    // Gated on Chrono being *active* rather than merely enabled, so a realm with
    // the table missing does not quietly pay a bonus to everyone: with Chrono
    // inactive `IsScalingActiveFor` is unconditionally true, and paying the
    // whole realm a permanent XP bonus is not a sensible reading of a missing
    // table.
    void ApplyChronoBonus(Player const* player, uint32& amount)
    {
        if (!amount || !sZoneScalingMgr->IsChronoXpBonusActive())
            return;

        if (!sZoneScalingMgr->IsScalingActiveFor(player))
            return;

        // The bonus is the reward for *choosing* the harder realm. A bot scaled
        // through Chrono.BotDefault did not choose anything.
        if (ZoneScalingMgr::IsBotPlayer(player))
            return;

        uint64 const bumped =
            uint64(amount) * (100 + sZoneScalingMgr->GetChronoXpBonusPct()) / 100;

        amount = uint32(std::min<uint64>(bumped, UINT32_MAX));
        sZoneScalingMgr->CountXpChronoBonus();
    }

    // `Quest::XPValue`'s core, with the quest level as a parameter instead of a
    // member so the same quest can be priced at two levels.
    //
    // The rounding buckets at the tail of the real function are deliberately not
    // reproduced. They quantise the final award to the nearest 5/10/25/50, which
    // is presentation; reproducing them inside a ratio would introduce error
    // rather than remove it, and the value this ratio multiplies has already
    // been through them.
    uint32 QuestXpAt(int32 questLevel, uint8 playerLevel, uint32 xpDifficulty)
    {
        if (questLevel <= 0 || xpDifficulty >= 10)
            return 0;

        QuestXPEntry const* entry = sQuestXPStore.LookupEntry(uint32(questLevel));
        if (!entry)
            return 0;

        int32 diffFactor = std::clamp(2 * (questLevel - int32(playerLevel)) + 20, 1, 10);
        return uint32(diffFactor) * entry->Exp[xpDifficulty] / 10;
    }

    // Everything in a kill award that does *not* depend on the victim's level,
    // reproduced from `Acore::XP::Gain` and `KillRewarder::_InitXP`.
    //
    // Only the gray top-up needs this. The hook path gets all of it for free by
    // multiplying the value the core already computed; this is the price of the
    // case the hook cannot reach, and the reason it is a separate switch.
    float StaticXpModifiers(Creature* creature)
    {
        float xpMod = 1.0f;

        if (creature->isElite())
            xpMod *= 2.0f;

        CreatureTemplate const* cinfo = creature->GetCreatureTemplate();
        if (cinfo)
            xpMod *= cinfo->ModExperience;

        xpMod *= sWorld->getRate(RATE_XP_KILL);

        // Players who were credited without dealing half the damage are paid
        // proportionally. At death this is normally zero, but a creature with
        // CREATURE_FLAG_EXTRA_NO_PLAYER_DAMAGE_REQ can die with it outstanding.
        if (uint32 damageReq = creature->GetPlayerDamageReq())
            if (uint32 maxHealth = creature->GetMaxHealth())
                xpMod *= 1.0f - 2.0f * float(damageReq) / float(maxHealth);

        // Low-health creatures pay less, applied outside Gain by _InitXP.
        if (cinfo && cinfo->ModHealth <= 0.75f && cinfo->ModHealth >= 0.0f)
            xpMod *= cinfo->ModHealth;

        return std::max(xpMod, 0.0f);
    }
}

// ---------------------------------------------------------------------------
// Shared with the loot path (Z-10). Declared in ZoneScaling.h and defined here
// rather than duplicated: coin and experience must agree about which creature
// is scaled and about what level it is being fought at, and two copies of that
// judgement would eventually disagree.
// ---------------------------------------------------------------------------

Creature* ZoneScalingScaledCreature(Unit* unit)
{
    if (!unit || !unit->IsCreature())
        return nullptr;

    Creature* creature = unit->ToCreature();
    if (creature->GetCharmerOrOwnerPlayerOrPlayerItself())
        return nullptr;

    ZoneScalingCreatureState const* state = ZoneScalingMgr::GetState(creature);
    return (state && state->Scalable && state->StatsValid) ? creature : nullptr;
}

Optional<uint8> ZoneScalingRewardLevelFor(Creature* creature, Player const* player, uint8& canonical)
{
    if (!creature || !player)
        return std::nullopt;

    ZoneScalingCreatureState const* state = ZoneScalingMgr::GetState(creature);
    if (!state || !state->Scalable || !state->StatsValid)
        return std::nullopt;

    if (!sZoneScalingMgr->IsScalingActiveFor(player, state->ZoneId))
        return std::nullopt;

    canonical = state->CanonicalLevel;

    // Warband: rewards follow the same group anchor as combat and display, so a
    // scaled kill pays for the level it was fought at for the whole party.
    Optional<uint8> const effective = sZoneScalingMgr->GetEffectiveLevel(
        state->ZoneId, ZoneScalingAnchorLevel(player), state->CanonicalLevel, state->Rank);

    if (!effective || *effective == state->CanonicalLevel)
        return std::nullopt;

    return effective;
}

// The whole of the quest decision, shared by the hook and by `.zs questxp`.
//
// Shared rather than duplicated on purpose. A diagnostic that reimplements what
// it diagnoses can agree with itself while disagreeing with the game, and this
// is the one reward path with no way to be checked against the game: a soak of
// AI-driven characters generates kills without pause and turns in no quests at
// all (measured, 2026-08-08: `character_queststatus_rewarded` did not move
// across a full soak). Every guard below is therefore only ever tested through
// this function.
//
// Returns true when the caller should apply CanonicalXp -> EffectiveXp; false
// when nothing should move, with `Declined` naming the reason for the command
// to print.
bool ZoneScalingDescribeQuestXp(Quest const* quest, Player const* player, uint8 playerLevel,
                                ZoneScalingQuestXp& out)
{
    if (!quest)
    {
        out.Declined = "no such quest";
        return false;
    }

    if (!sZoneScalingMgr->IsActive())
    {
        out.Declined = "module inactive";
        return false;
    }

    if (!sZoneScalingMgr->IsQuestXpScalingEnabled())
    {
        out.Declined = "ZoneScaling.Xp.Quests is off";
        return false;
    }

    out.QuestLevel = quest->GetQuestLevel();
    out.ZoneOrSort = quest->GetZoneOrSort();

    // Level -1 already tracks the player; there is nothing to correct.
    if (out.QuestLevel <= 0)
    {
        out.Declined = "quest level is -1 (already tracks the player)";
        return false;
    }

    // Negative ZoneOrSort is a sort bucket -- a class or profession -- not a
    // zone id, so no band applies. Left alone rather than guessed at from the
    // player's current position: a quest is routinely turned in a continent
    // away from where it was done.
    if (out.ZoneOrSort <= 0)
    {
        out.Declined = "ZoneOrSort is a sort, not a zone";
        return false;
    }

    // A null player is the command asking about a level rather than a person;
    // there is no Chrono flag to consult and none is needed.
    if (player && !sZoneScalingMgr->IsScalingActiveFor(player, uint32(out.ZoneOrSort)))
    {
        out.Declined = "player has opted out of Chrono Mode";
        return false;
    }

    Optional<uint8> const effective = sZoneScalingMgr->GetEffectiveLevel(
        uint32(out.ZoneOrSort), playerLevel, uint8(std::min<int32>(out.QuestLevel, 255)));

    if (!effective)
    {
        out.Declined = "zone is not scaled";
        return false;
    }

    out.Effective = *effective;

    if (int32(out.Effective) == out.QuestLevel)
    {
        out.Declined = "effective level equals quest level";
        return false;
    }

    out.CanonicalXp = QuestXpAt(out.QuestLevel, playerLevel, quest->GetXPId());
    out.EffectiveXp = QuestXpAt(int32(out.Effective), playerLevel, quest->GetXPId());

    if (!out.CanonicalXp || !out.EffectiveXp)
    {
        out.Declined = "no QuestXP.dbc row at one of the two levels";
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// The reachable half: correct an award the core already decided to pay.
// ---------------------------------------------------------------------------

class ZoneScalingRewardPlayerScript : public PlayerScript
{
public:
    ZoneScalingRewardPlayerScript() : PlayerScript("ZoneScalingRewardPlayerScript",
        {
            PLAYERHOOK_ON_GIVE_EXP,
            PLAYERHOOK_ON_QUEST_COMPUTE_EXP
        }) { }

    // Fired for kills, quests, exploration and battlegrounds. Only the kill
    // source carries a victim whose level this module may have changed; the
    // Chrono bonus applies to all four, because it is a property of the player
    // rather than of what they killed.
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override
    {
        if (!player || !amount)
            return;

        if (xpSource == PlayerXPSource::XPSOURCE_KILL && victim
            && sZoneScalingMgr->IsXpScalingEnabled() && sZoneScalingMgr->IsActive())
        {
            AdjustKillXP(player, amount, victim);
        }

        ApplyChronoBonus(player, amount);
    }

    // `xpValue` arrives already multiplied by the quest rate and by
    // SPELL_AURA_MOD_XP_QUEST_PCT, which is exactly why this is a ratio: both
    // survive it untouched.
    void OnPlayerQuestComputeXP(Player* player, Quest const* quest, uint32& xpValue) override
    {
        if (!player || !xpValue)
            return;

        uint8 const playerLevel = player->GetLevel();

        ZoneScalingQuestXp describe;
        if (!ZoneScalingDescribeQuestXp(quest, player, playerLevel, describe))
            return;

        uint32 const before = xpValue;
        xpValue = uint32(std::min<uint64>(
            uint64(xpValue) * describe.EffectiveXp / describe.CanonicalXp, UINT32_MAX));

        sZoneScalingMgr->CountXpQuestAdjusted();

        if (sZoneScalingMgr->IsDebug())
            LOG_DEBUG("module.zonescaling",
                      "[zone-scaling] quest {} for {} (level {}): quest level {} -> {}, xp {} -> {}",
                      quest->GetQuestId(), player->GetName(), playerLevel, describe.QuestLevel,
                      uint32(describe.Effective), before, xpValue);
    }

private:
    static void AdjustKillXP(Player* player, uint32& amount, Unit* victim)
    {
        Creature* creature = ZoneScalingScaledCreature(victim);
        if (!creature)
            return;

        uint8 canonical = 0;
        Optional<uint8> const effective = ZoneScalingRewardLevelFor(creature, player, canonical);
        if (!effective)
            return;

        uint8 const playerLevel = player->GetLevel();
        ContentLevels const content =
            GetContentLevelsForMapAndZone(creature->GetMapId(), creature->GetZoneId());

        uint32 const canonicalGain = Acore::XP::BaseGain(playerLevel, canonical, content);
        uint32 const effectiveGain = Acore::XP::BaseGain(playerLevel, *effective, content);

        // A zero denominator here means the victim was gray to this player, in
        // which case the core paid nothing and this hook should not have fired.
        // Bail rather than divide: the gray top-up owns that kill.
        if (!canonicalGain || !effectiveGain)
            return;

        uint32 const before = amount;
        amount = uint32(std::min<uint64>(uint64(amount) * effectiveGain / canonicalGain, UINT32_MAX));

        sZoneScalingMgr->CountXpKillAdjusted();

        if (sZoneScalingMgr->IsDebug())
            LOG_DEBUG("module.zonescaling",
                      "[zone-scaling] kill xp for {} (level {}): creature {} level {} -> {}, xp {} -> {}",
                      player->GetName(), playerLevel, creature->GetEntry(), canonical,
                      uint32(*effective), before, amount);
    }
};

// ---------------------------------------------------------------------------
// The unreachable half: pay for a kill the core paid nothing for.
// ---------------------------------------------------------------------------

class ZoneScalingRewardUnitScript : public UnitScript
{
public:
    ZoneScalingRewardUnitScript() : UnitScript("ZoneScalingRewardUnitScript", true,
        {
            UNITHOOK_ON_UNIT_DEATH
        }) { }

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        if (!sZoneScalingMgr->IsGreyKillXpEnabled() || !sZoneScalingMgr->IsActive())
            return;

        Creature* creature = ZoneScalingScaledCreature(unit);
        if (!creature)
            return;

        // Guards copied from `Acore::XP::Gain` and `KillRewarder::_InitXP`.
        // A creature these reject pays no experience at any level, and this
        // path must not become the one way to get XP from a critter.
        if (creature->IsTotem() || creature->IsPet() || creature->IsCritter()
            || creature->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_XP))
            return;

        Player* killerPlayer = killer ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (!killerPlayer || killerPlayer->GetVehicle())
            return;

        // Battlegrounds run their own rates through a branch this file does not
        // reproduce, and instanced maps are off by default anyway.
        if (killerPlayer->InBattleground())
            return;

        ContentLevels const content =
            GetContentLevelsForMapAndZone(creature->GetMapId(), creature->GetZoneId());

        float const staticMod = StaticXpModifiers(creature);

        // Eligibility, reproduced from `KillRewarder::_InitGroupData`: the
        // killer always, plus any group member within reward distance, counting
        // only the living toward the share denominators.
        std::vector<Player*> eligible;
        uint32 aliveCount = 0;
        uint32 aliveSumLevel = 0;

        if (Group* group = killerPlayer->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member || (member != killerPlayer && !member->IsAtGroupRewardDistance(creature)))
                    continue;

                eligible.push_back(member);

                if (member->IsAlive())
                {
                    ++aliveCount;
                    aliveSumLevel += member->GetLevel();
                }
            }

            if (!aliveCount || !aliveSumLevel)
                return;
        }
        else
        {
            eligible.push_back(killerPlayer);
            aliveCount = 1;
        }

        bool const isRaid = killerPlayer->GetGroup() && killerPlayer->GetGroup()->isRaidGroup()
                            && killerPlayer->GetMap() && killerPlayer->GetMap()->IsRaid();

        float const groupRate = killerPlayer->GetGroup()
                              ? Acore::XP::xp_in_group_rate(aliveCount, isRaid) : 1.0f;

        bool const inGroup = killerPlayer->GetGroup() != nullptr;

        for (Player* player : eligible)
            PayGrayKill(player, creature, content, staticMod, groupRate, aliveSumLevel, inGroup);
    }

private:
    static void PayGrayKill(Player* player, Creature* creature, ContentLevels content,
                            float staticMod, float groupRate, uint32 aliveSumLevel, bool inGroup)
    {
        // Dead members are paid nothing by `_RewardXP` either.
        if (!player->IsAlive())
            return;

        uint8 const playerLevel = player->GetLevel();

        // The partition. If the victim is not gray to this player the core has
        // already paid them and the hook has already corrected it.
        if (!IsGray(playerLevel, creature->GetLevel()))
            return;

        uint8 canonical = 0;
        Optional<uint8> const effective = ZoneScalingRewardLevelFor(creature, player, canonical);
        if (!effective)
            return;

        uint32 const base = Acore::XP::BaseGain(playerLevel, *effective, content);
        if (!base)
            return;

        float amount = float(base) * staticMod;

        // The group share, from `_RewardPlayer`. Note what is deliberately
        // *not* reproduced: `_isFullXP`'s halving, which exists to penalise a
        // group carrying someone the victim is gray to. In a scaled zone the
        // victim is not gray to them -- it is being fought at their level --
        // so applying that penalty here would charge them twice for a state
        // this module has removed.
        if (inGroup && aliveSumLevel)
            amount *= groupRate * float(playerLevel) / float(aliveSumLevel);

        amount *= player->GetTotalAuraMultiplier(SPELL_AURA_MOD_XP_PCT);

        uint32 xp = uint32(std::max(amount, 0.0f));
        ApplyChronoBonus(player, xp);

        if (!xp)
            return;

        player->GiveXP(xp, creature, groupRate);

        if (Pet* pet = player->GetPet())
            pet->GivePetXP(inGroup ? xp / 2 : xp);

        sZoneScalingMgr->CountXpGreyPaid(xp);

        if (sZoneScalingMgr->IsDebug())
            LOG_DEBUG("module.zonescaling",
                      "[zone-scaling] grey top-up for {} (level {}): creature {} level {} -> {}, xp {}",
                      player->GetName(), playerLevel, creature->GetEntry(), canonical,
                      uint32(*effective), xp);
    }
};

void AddZoneScalingRewardScripts()
{
    new ZoneScalingRewardPlayerScript();
    new ZoneScalingRewardUnitScript();
}
