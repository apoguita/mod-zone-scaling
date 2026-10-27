/*
 * Copyright (C) 2026+ mod-zone-scaling contributors, released under GNU AGPL v3
 * license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * Loot: coin and gear for a fight the module changed.
 *
 * Z-09 made experience follow the fight. This file does the same for the corpse.
 * The two halves are very different in kind, and the difference is why they have
 * separate switches and opposite defaults.
 *
 * COIN, AND WHY THE CURVE IS NOT INVENTED HERE
 *
 * A creature's coin comes from `creature_template.mingold/maxgold` and is fixed
 * per entry -- `Loot::generateMoneyLoot` never looks at a level. So a scaled-up
 * Defias thug still drops a level-8 thug's copper no matter how hard it hits.
 *
 * Unlike experience there is no core *formula* to take a ratio of; coin is data.
 * But there is core *data*: `quest_money_reward` is Blizzard's own per-level
 * statement of what a copper reward is worth, it is already loaded into memory,
 * and on a stock world DB it is complete and non-decreasing for every level from
 * 1 to 80. The multiplier is the ratio of that curve at the two levels. This
 * module therefore ships no coin constant of its own, which is the same
 * discipline Z-09 applied when it took a ratio of `BaseGain` rather than
 * recomputing an award.
 *
 * Sanity of the choice, measured against the shipped `creature_template` on
 * 2026-08-08 (mean coin for non-elite fixed-level creatures):
 *
 *      levels     creature data     quest_money_reward
 *      10 -> 20        3.9x                3.6x
 *      10 -> 40       20.7x               14.0x
 *      10 -> 60       22.8x               28.0x
 *      10 -> 80      116.0x              232.0x
 *
 * Close through the classic range and roughly a factor of two generous at 80,
 * because the WotLK end of `quest_money_reward` inflates harder than creature
 * coin does. That overshoot is stated in the conf rather than quietly corrected:
 * a fitted fudge factor would be a number nobody could check, and an
 * administrator who finds the honest one too rich has `Loot.MoneyMax`.
 *
 * ⚠ THE PART OF THE ACCEPTANCE THAT IS NOT REACHABLE, AND EXACTLY HOW FAR
 *
 * The milestone asks that two players of different levels looting one corpse
 * each get coin for their own effective level *and neither sees the other's
 * number*. Read `WorldSession::HandleLootMoneyOpcode`:
 *
 *   - `Loot::gold` is one number on the corpse, not one per looter.
 *   - `OnPlayerBeforeLootMoney` fires once, for whoever clicked, *before* the
 *     split -- so mutating `loot->gold` there moves everybody together.
 *   - the group branch then computes `goldPerPlayer = loot->gold / n` and sends
 *     that same uint32 to every member in `SMSG_LOOT_MONEY_NOTIFY`. There is no
 *     hook between the split and the packet.
 *
 * So the *money* can be made per-recipient and the *chat line* cannot. The lever
 * for the money is `Player::ModifyMoney`, which calls `OnPlayerMoneyChanged`
 * with a mutable `int32&` for each member in turn. This file pre-computes each
 * eligible member's corrected share in the before-loot hook, sets `loot->gold`
 * to the *sum* of those shares so the pot is conserved and the solo case reads
 * correctly, and then corrects each member's own credit as `ModifyMoney` reaches
 * them.
 *
 * Result against the acceptance, stated plainly:
 *
 *   - Solo looter: fully met. Coin and chat line both correct.
 *   - Grouped: each member's *balance* moves by their own level-appropriate
 *     amount, which is the substance of it. Each member's *chat line* shows the
 *     group average, so they see the same number rather than their own. Closing
 *     that needs an edit to LootHandler.cpp, and this module does not edit the
 *     core.
 *
 * WHY THE PENDING MAP IS SAFE
 *
 * `OnPlayerMoneyChanged` fires on every money change on the realm -- vendors,
 * mail, repairs -- so a stale entry would corrupt an unrelated transaction. Four
 * things prevent it, and they are deliberately redundant:
 *
 *   - the map is `thread_local`. Maps update on a thread pool, so only the
 *     thread running this loot handler can see these entries at all.
 *   - the whole sequence -- our hook, every `ModifyMoney`, the flush -- happens
 *     synchronously inside one `HandleLootMoneyOpcode` call on that thread, with
 *     no other money change possible in between.
 *   - `OnLootMoney` fires unconditionally at the end of that same block and
 *     clears whatever is left, so nothing survives the call.
 *   - each entry records the exact amount it expects, and a credit that does not
 *     match it is left alone.
 *
 * GEAR, AND WHY IT SHIPS OFF
 *
 * `OnAfterLootTemplateProcess` hands over the rolled `Loot*` before anyone sees
 * it, so an item's `itemid` can be swapped for one of the same class, subclass,
 * inventory type and quality at the effective level or up to three levels
 * below. That is the milestone's
 * "level-banded reference loot", built from an index over `item_template` rather
 * than from new module tables -- 9,984 items in 199 slot/quality buckets on this
 * realm's world DB (the index reported that at first use, 2026-08-08), which is
 * deep enough to band every slot. Do not trust a hand-written SQL count of the
 * same filter: one written for this comment came out 12 items low, because the
 * `FlagsCu` and DisableMgr clauses do not translate to column tests. The
 * startup line "gear banding index: N items across M buckets" is the only
 * figure that is actually the pool.
 *
 * Whites are excluded along with poor: `IsUsableReplacement` starts at
 * uncommon. Common gear is vendor filler that nobody chooses to wear, and
 * banding it to the player's level would dress an outlevelled zone in slightly
 * newer junk rather than in anything worth looting.
 *
 * It is off by default because it is a much larger claim than coin: it rewrites
 * *which items the world drops*. A realm that switches it on stops being one
 * where a particular mob drops a particular sword. Coin is arithmetic; this is
 * content.
 *
 * Two limits are structural rather than oversights. Loot is rolled once per
 * corpse, at the moment of death, for the round-robin looter -- so in a group
 * the gear bands to *that* member's level, not to each looter's, and per-looter
 * gear is not expressible on this loot model at all. And `quest_items` are never
 * touched: substituting one would break the quest it belongs to.
 */

#include "ZoneScaling.h"

#include "Creature.h"
#include "DisableMgr.h"
#include "Group.h"
#include "Item.h"
#include "ItemEnchantmentMgr.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "LootMgr.h"
#include "LootScript.h"
#include "Map.h"
#include "MiscScript.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    // Which column of `quest_money_reward` the coin curve reads.
    //
    // The table is ten columns of increasing generosity, indexed by a quest's
    // RewardMoneyDifficulty. A creature has no such field, so one column has to
    // be chosen and stated. Column 4 is the middle of the range and is complete
    // and non-decreasing across all eighty levels; only the *ratio* of two of
    // its entries is ever used, so the absolute generosity of the column does
    // not reach the player.
    constexpr uint32 MoneyCurveColumn = 4;

    // --- coin ---------------------------------------------------------------

    // One member's corrected credit, waiting for `ModifyMoney` to reach them.
    struct PendingCoin
    {
        uint32 Expected = 0;    // what the core is about to credit
        uint32 Target   = 0;    // what it should have been
    };

    // See "WHY THE PENDING MAP IS SAFE" above before touching this.
    thread_local std::unordered_map<ObjectGuid, PendingCoin> g_pendingCoin;

    // The coin multiplier for one player against one creature, or 1.0 when
    // nothing about this pairing should move.
    float CoinMultiplierFor(Creature* creature, Player const* player)
    {
        uint8 canonical = 0;
        Optional<uint8> const effective = ZoneScalingRewardLevelFor(creature, player, canonical);
        if (!effective)
            return 1.0f;

        Optional<float> const multiplier =
            sZoneScalingMgr->DescribeMoneyMultiplier(canonical, *effective);

        return multiplier ? *multiplier : 1.0f;
    }

    // The creature a `Loot` belongs to, or nullptr.
    //
    // `Creature::AddToWorld` stamps `sourceWorldObjectGUID` immediately before
    // `OnCreatureAddWorld` -- the very hook that tags creatures for this module
    // -- so any creature the module knows about carries it.
    Creature* LootSourceCreature(Player* player, Loot const* loot)
    {
        if (!player || !loot || !loot->sourceWorldObjectGUID.IsCreatureOrVehicle())
            return nullptr;

        Map* map = player->GetMap();
        if (!map)
            return nullptr;

        return ZoneScalingScaledCreature(map->GetCreature(loot->sourceWorldObjectGUID));
    }

    // --- gear ---------------------------------------------------------------

    struct GearEntry
    {
        uint32 ItemId        = 0;
        uint8  RequiredLevel = 0;
    };

    // (Class, SubClass, InventoryType, Quality) -> entries sorted by required
    // level. Built once from `item_template`; deliberately independent of every
    // config value, so `.reload config` can never leave it stale.
    std::unordered_map<uint32, std::vector<GearEntry>> g_gearPool;
    std::once_flag g_gearPoolOnce;

    constexpr uint32 GearKey(uint32 itemClass, uint32 subClass, uint32 invType, uint32 quality)
    {
        return (itemClass << 24) | (subClass << 16) | (invType << 8) | quality;
    }

    // ---------------------------------------------------------------------
    // Flag parity: the constraint that makes substitution safe at all.
    //
    // `Loot::AddItem` (LootMgr.cpp:481) runs during `tab->Process(...)`, which
    // is *inside* `Loot::FillLoot` at line 540 -- and the hook that lets this
    // module see the loot, `OnAfterLootTemplateProcess`, is not called until
    // line 563. By the time we substitute an item id, the core has already:
    //
    //   * derived `LootItem::freeforall` from ITEM_FLAG_MULTI_DROP and
    //     `LootItem::follow_loot_rules` from ITEM_FLAGS_CU_FOLLOW_LOOT_RULES,
    //     in `LootItem`'s constructor (LootMgr.cpp:393);
    //   * incremented `unlootedCount` -- but only for items that are not
    //     MULTI_DROP and that `AllowedForPlayer` accepted (LootMgr.cpp:520-535).
    //
    // So a substitution that changes any flag those two decisions read leaves
    // the bookkeeping describing an item that is no longer there. The visible
    // symptom is a corpse that never releases: `unlootedCount` never reaches
    // zero, so the loot window stays open with nothing in it.
    //
    // The fix is not to re-derive the fields after the swap -- `unlootedCount`
    // is a running total on the `Loot`, and we cannot know which increments
    // were ours. It is to make the flags unable to move: every item in the
    // pool is neutral on all of them, and any original that is not neutral is
    // left alone. Neutral-to-neutral is a no-op for every one of these
    // decisions, so nothing needs recomputing.
    //
    // `AllowedForPlayer` (LootMgr.cpp:417) also reads the faction flags, the
    // recipe flag, `StartQuest`, and DisableMgr, so those join the set.
    bool HasNeutralLootFlags(uint32 itemId, ItemTemplate const* proto)
    {
        return proto
            && !proto->HasFlag(ITEM_FLAG_MULTI_DROP)
            && !proto->HasFlagCu(ITEM_FLAGS_CU_FOLLOW_LOOT_RULES)
            && !proto->HasFlag(ITEM_FLAG_HIDE_UNUSABLE_RECIPE)
            && !proto->HasFlag2(ITEM_FLAG2_FACTION_HORDE)
            && !proto->HasFlag2(ITEM_FLAG2_FACTION_ALLIANCE)
            && proto->StartQuest == 0
            && !sDisableMgr->IsDisabledFor(DISABLE_TYPE_LOOT, itemId, nullptr);
    }

    // Whether an item may serve as a *replacement*.
    //
    // Stricter than the test for what may be replaced, and every clause is there
    // to keep something out of the world that has no business dropping off a
    // mob: a quest starter, a unique, a profession or reputation or PvP reward,
    // a heirloom, or a piece restricted to classes and races the looter may not
    // be. Restrictions expressed through subclass -- plate, mail, a weapon type
    // -- are preserved instead, because the key already carries them.
    bool IsUsableReplacement(uint32 itemId, ItemTemplate const* proto)
    {
        return HasNeutralLootFlags(itemId, proto)
            && (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
            && proto->InventoryType > 0
            && proto->Quality >= ITEM_QUALITY_UNCOMMON && proto->Quality <= ITEM_QUALITY_EPIC
            && proto->RequiredLevel >= 1 && proto->RequiredLevel <= 80
            && proto->MaxCount == 0
            && proto->Bonding != BIND_QUEST_ITEM
            && proto->RequiredSkill == 0
            && proto->RequiredSpell == 0
            && proto->RequiredReputationFaction == 0
            && proto->RequiredHonorRank == 0
            && proto->RequiredCityRank == 0
            && proto->ScalingStatDistribution == 0
            && proto->AllowableClass == uint32(-1)
            && proto->AllowableRace == uint32(-1);
    }

    void BuildGearPool()
    {
        ItemTemplateContainer const* store = sObjectMgr->GetItemTemplateStore();
        if (!store)
            return;

        for (auto const& [itemId, proto] : *store)
        {
            if (!IsUsableReplacement(itemId, &proto))
                continue;

            g_gearPool[GearKey(proto.Class, proto.SubClass, proto.InventoryType, proto.Quality)]
                .push_back({ itemId, uint8(proto.RequiredLevel) });
        }

        std::size_t total = 0;
        for (auto& [key, entries] : g_gearPool)
        {
            std::sort(entries.begin(), entries.end(),
                      [](GearEntry const& a, GearEntry const& b)
                      { return a.RequiredLevel < b.RequiredLevel; });
            total += entries.size();
        }

        LOG_INFO("server.loading",
                 "[zone-scaling] gear banding index: {} items across {} slot/quality buckets",
                 total, g_gearPool.size());
    }
}

// The coin curve. Defined here rather than in ZoneScalingMgr.cpp so that every
// piece of knowledge about how coin is priced lives in one file.
Optional<float> ZoneScalingMgr::DescribeMoneyMultiplier(uint8 canonical, uint8 effective) const
{
    uint32 const from = sObjectMgr->GetQuestMoneyReward(canonical, MoneyCurveColumn);
    uint32 const to   = sObjectMgr->GetQuestMoneyReward(effective, MoneyCurveColumn);

    // Either level outside the table's 1-80 range. Leave the coin alone rather
    // than guess: a zero here would otherwise read as "this creature is worth
    // nothing", which is a much larger claim than "no data".
    if (!from || !to)
        return std::nullopt;

    float const max = std::max(_lootMoneyMax, 1.0f);
    return std::clamp(float(to) / float(from), 1.0f / max, max);
}

// The whole of the gear decision, shared by the hook and by `.zs gear`. Shared
// rather than duplicated for the reason `ZoneScalingDescribeQuestXp` is: a
// diagnostic that reimplements what it diagnoses can agree with itself while
// disagreeing with the game.
bool ZoneScalingDescribeGearSwap(uint32 itemId, uint8 effectiveLevel, ZoneScalingGearSwap& out)
{
    out.FromItem = itemId;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
    {
        out.Declined = "no such item";
        return false;
    }

    out.FromLevel = uint8(std::min<uint32>(proto->RequiredLevel, 255));

    if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
    {
        out.Declined = "not a weapon or armour";
        return false;
    }

    if (!proto->InventoryType)
    {
        out.Declined = "not equippable";
        return false;
    }

    // A quest starter carries the quest; swapping it silently removes content.
    if (proto->StartQuest)
    {
        out.Declined = "starts a quest";
        return false;
    }

    // Flag parity -- see HasNeutralLootFlags above. `Loot::AddItem` has already
    // counted this item into `unlootedCount` using these flags by the time the
    // hook runs, so an item carrying any of them must not move.
    if (!HasNeutralLootFlags(itemId, proto))
    {
        out.Declined = "carries a loot flag the core has already counted on";
        return false;
    }

    if (proto->Quality < ITEM_QUALITY_UNCOMMON
        || proto->Quality > sZoneScalingMgr->GetLootGearMaxQuality())
    {
        out.Declined = "quality outside Loot.Gear.MaxQuality";
        return false;
    }

    std::call_once(g_gearPoolOnce, BuildGearPool);

    auto const bucket = g_gearPool.find(
        GearKey(proto->Class, proto->SubClass, proto->InventoryType, proto->Quality));

    if (bucket == g_gearPool.end())
    {
        out.Declined = "no items of this slot and quality in the index";
        return false;
    }

    uint8 const tolerance = sZoneScalingMgr->GetLootGearTolerance();
    uint8 const minRequiredLevel = uint8(effectiveLevel > tolerance ? effectiveLevel - tolerance : 1);

    // Gear already at the effective level or above it is not lowered. Gear up
    // to `tolerance` levels below it is also close enough to leave alone.
    if (out.FromLevel >= minRequiredLevel)
    {
        out.Declined = "already at the effective level or within Loot.Gear.Tolerance below it";
        return false;
    }

    // Replacement gear is never above the effective level, and never more than
    // `tolerance` levels below it.
    uint8 const lo = minRequiredLevel;
    uint8 const hi = effectiveLevel;

    auto const& entries = bucket->second;
    auto const first = std::lower_bound(entries.begin(), entries.end(), lo,
                                        [](GearEntry const& e, uint8 v)
                                        { return e.RequiredLevel < v; });
    auto const last = std::upper_bound(entries.begin(), entries.end(), hi,
                                       [](uint8 v, GearEntry const& e)
                                       { return v < e.RequiredLevel; });

    out.Candidates = uint32(std::distance(first, last));
    if (!out.Candidates)
    {
        out.Declined = "no candidate of this slot and quality at that level";
        return false;
    }

    GearEntry const& chosen = *(first + urand(0, out.Candidates - 1));

    if (chosen.ItemId == itemId)
    {
        out.Declined = "the pool offered the same item back";
        return false;
    }

    out.ToItem  = chosen.ItemId;
    out.ToLevel = chosen.RequiredLevel;
    return true;
}

// ---------------------------------------------------------------------------
// Coin
// ---------------------------------------------------------------------------

class ZoneScalingLootPlayerScript : public PlayerScript
{
public:
    ZoneScalingLootPlayerScript() : PlayerScript("ZoneScalingLootPlayerScript",
        {
            PLAYERHOOK_ON_BEFORE_LOOT_MONEY,
            PLAYERHOOK_ON_MONEY_CHANGED
        }) { }

    void OnPlayerBeforeLootMoney(Player* player, Loot* loot) override
    {
        g_pendingCoin.clear();

        if (!player || !loot || !loot->gold)
            return;

        if (!sZoneScalingMgr->IsLootMoneyScalingEnabled() || !sZoneScalingMgr->IsActive())
            return;

        Creature* creature = LootSourceCreature(player, loot);
        if (!creature)
            return;

        // Mirrors the handler's own `shareMoney`: it clears the flag for a
        // creature that is still alive, which is the pickpocket case, and
        // otherwise shares with the group. Getting this wrong would not merely
        // mis-split -- it would leave pending entries nobody ever collects.
        bool const share = !creature->IsAlive() && player->GetGroup() != nullptr;

        std::vector<Player*> recipients;

        if (share)
        {
            Group* group = player->GetGroup();
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                if (Player* member = itr->GetSource())
                    if (player->IsAtLootRewardDistance(member))
                        recipients.push_back(member);
        }
        else
        {
            recipients.push_back(player);
        }

        if (recipients.empty())
            return;

        uint32 const before = loot->gold;
        uint32 const count  = uint32(recipients.size());
        uint32 const share1 = before / count;

        std::vector<std::pair<Player*, uint32>> corrected;
        corrected.reserve(recipients.size());

        uint64 total = 0;
        bool   moved = false;

        for (Player* member : recipients)
        {
            float const multiplier = CoinMultiplierFor(creature, member);
            uint32 const target = uint32(std::lround(double(share1) * double(multiplier)));

            if (multiplier != 1.0f)
                moved = true;

            corrected.emplace_back(member, target);
            total += target;
        }

        if (!moved)
            return;

        // A pot that rounded to nothing still has to be lootable: a zero-copper
        // loot reads as a bug to the player and leaves the corpse in a state the
        // handler did not expect.
        uint32 const after = uint32(std::clamp<uint64>(total, 1, UINT32_MAX));

        loot->gold = after;
        sZoneScalingMgr->CountLootMoneyScaled(int64(after) - int64(before));

        // What the handler is about to hand each member. Only members whose own
        // share differs from it need correcting; in the solo case nobody does,
        // which is why that path never touches the pending map at all.
        uint32 const expected = share ? after / count : after;

        for (auto const& [member, target] : corrected)
            if (target != expected)
                g_pendingCoin[member->GetGUID()] = { expected, target };

        if (sZoneScalingMgr->IsDebug())
            LOG_DEBUG("module.zonescaling",
                      "[zone-scaling] loot coin from creature {} for {} recipient(s): {} -> {}"
                      " ({} per-member correction(s) pending)",
                      creature->GetEntry(), count, before, after, g_pendingCoin.size());
    }

    // Fires for every money change on the realm. The early return on an empty
    // map is the whole cost of that on any thread not currently splitting loot.
    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        if (g_pendingCoin.empty() || !player || amount <= 0)
            return;

        auto const itr = g_pendingCoin.find(player->GetGUID());
        if (itr == g_pendingCoin.end())
            return;

        // A credit that is not the one we predicted is somebody else's
        // transaction. Leave it, and leave the entry for the credit we want.
        if (uint32(amount) != itr->second.Expected)
            return;

        int32 const target = int32(std::min<uint32>(itr->second.Target, uint32(INT32_MAX)));

        if (sZoneScalingMgr->IsDebug())
            LOG_DEBUG("module.zonescaling",
                      "[zone-scaling] loot coin share for {} (level {}): {} -> {}",
                      player->GetName(), uint32(player->GetLevel()), amount, target);

        amount = target;
        g_pendingCoin.erase(itr);
    }
};

// The flush. `WorldSession::HandleLootMoneyOpcode` calls this unconditionally
// at the end of the block that does the splitting, so nothing this file staged
// can outlive the packet that caused it.
class ZoneScalingLootFlushScript : public LootScript
{
public:
    ZoneScalingLootFlushScript() : LootScript("ZoneScalingLootFlushScript",
        {
            LOOTHOOK_ON_LOOT_MONEY
        }) { }

    void OnLootMoney(Player* /*player*/, uint32 /*gold*/) override
    {
        g_pendingCoin.clear();
    }
};

// ---------------------------------------------------------------------------
// Gear
// ---------------------------------------------------------------------------

class ZoneScalingLootMiscScript : public MiscScript
{
public:
    ZoneScalingLootMiscScript() : MiscScript("ZoneScalingLootMiscScript",
        {
            MISCHOOK_ON_AFTER_LOOT_TEMPLATE_PROCESS
        }) { }

    void OnAfterLootTemplateProcess(Loot* loot, LootTemplate const* /*tab*/, LootStore const& store,
                                    Player* lootOwner, bool /*personal*/, bool /*noEmptyError*/,
                                    uint16 /*lootMode*/) override
    {
        if (!sZoneScalingMgr->IsLootGearBandingEnabled() || !sZoneScalingMgr->IsActive())
            return;

        // Creature drops only. Fishing, mining, containers and the rest have no
        // creature behind them and nothing to band against.
        if (&store != &LootTemplates_Creature)
            return;

        if (!loot || !lootOwner || loot->items.empty())
            return;

        Creature* creature = LootSourceCreature(lootOwner, loot);
        if (!creature)
            return;

        uint8 canonical = 0;
        Optional<uint8> const effective = ZoneScalingRewardLevelFor(creature, lootOwner, canonical);
        if (!effective)
            return;

        // `loot->quest_items` is deliberately not iterated. Swapping a quest
        // drop breaks the quest that needs it.
        for (LootItem& item : loot->items)
        {
            // Conditions, quest requirements and free-for-all are all statements
            // about *this particular item* that would not survive being someone
            // else. Leave them exactly as the loot template rolled them.
            if (item.needs_quest || item.freeforall || !item.conditions.empty())
                continue;

            ZoneScalingGearSwap swap;
            if (!ZoneScalingDescribeGearSwap(item.itemid, *effective, swap))
                continue;

            uint32 const from = item.itemid;

            item.itemid = swap.ToItem;

            // Both are rolled from the item id in `LootItem`'s constructor and
            // are meaningless against a different item -- a suffix factor for
            // the old entry applied to the new one produces an item with
            // nonsense stats rather than an obviously broken one.
            item.randomSuffix     = GenerateEnchSuffixFactor(item.itemid);
            item.randomPropertyId = Item::GenerateItemRandomPropertyId(item.itemid);

            sZoneScalingMgr->CountLootGearBanded();

            if (sZoneScalingMgr->IsDebug())
                LOG_DEBUG("module.zonescaling",
                          "[zone-scaling] loot gear from creature {} (level {} -> {}):"
                          " item {} (req {}) -> {} (req {}), {} candidate(s)",
                          creature->GetEntry(), canonical, uint32(*effective), from,
                          uint32(swap.FromLevel), swap.ToItem, uint32(swap.ToLevel),
                          swap.Candidates);
        }
    }
};

void AddZoneScalingLootScripts()
{
    new ZoneScalingLootPlayerScript();
    new ZoneScalingLootFlushScript();
    new ZoneScalingLootMiscScript();
}
