/*
 * Copyright (C) 2026+ mod-zone-scaling contributors, released under GNU AGPL v3
 * license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#ifndef MOD_ZONE_SCALING_H
#define MOD_ZONE_SCALING_H

#include "DataMap.h"
#include "Define.h"
#include "Optional.h"

#include <atomic>
#include <string>
#include <unordered_map>
#include <unordered_set>

class Creature;
class Map;
class Player;
class Quest;
class Unit;
struct CreatureTemplate;
struct ItemTemplate;
struct Loot;

// What to do with a zone that has no row in mod_zone_scaling_zone.
enum ZoneScalingDefault : uint8
{
    ZS_DEFAULT_OFF   = 0,   // unlisted zones are never scaled
    ZS_DEFAULT_CLAMP = 1    // unlisted zones use the configured default band
};

// Which way a creature is allowed to move from its canonical level.
//
// Orthogonal to ZoneScalingDefault: that decides *whether* a zone scales, this
// decides what scaling means once it does.
enum ZoneScalingDirection : uint8
{
    ZS_DIR_BOTH = 0,    // full scaling -- the creature presents at the player's
                        // level whether that is above or below its own
    ZS_DIR_UP   = 1,    // level-up only -- a creature below the player is
                        // raised to meet them; one already above them is left
                        // alone, so a zone keeps its dangerous corners

    ZS_DIR_INHERIT = 2  // per-zone bands only, never the global setting: use
                        // whatever ZoneScaling.Direction says. Imported content
                        // built for a higher level cap has to scale DOWN in its
                        // own zones while the open world stays level-up-only,
                        // and one global value cannot express both.
};

// Which kind of map a creature is standing on.
//
// Mirrors MapEntry::map_type, which is a clean five-way discriminator, rather
// than the Map:: predicates -- and that choice is the whole point. The
// predicates overlap: `Map::IsDungeon()` is true for raids as well, because
// MapEntry::IsDungeon() is `map_type == MAP_INSTANCE || map_type == MAP_RAID`.
// Gating dungeons and raids separately through `IsDungeon()` would silently
// give raids whatever dungeons were configured to do, which is precisely the
// mistake this enum exists to make unrepresentable.
enum ZoneScalingMapKind : uint8
{
    ZS_MAP_WORLD = 0,       // MAP_COMMON -- the open world
    ZS_MAP_DUNGEON,         // MAP_INSTANCE -- 5-man instances, normal and heroic
    ZS_MAP_RAID,            // MAP_RAID
    ZS_MAP_BATTLEGROUND,    // MAP_BATTLEGROUND
    ZS_MAP_ARENA,           // MAP_ARENA
    ZS_MAP_KIND_MAX
};

char const* ZoneScalingMapKindName(ZoneScalingMapKind kind);

// Why a creature template is not scaled. Ordered most to least specific: the
// first matching reason is the one reported, so `.zs excluded` explains the
// actual cause rather than the first cheap test that happened to pass.
enum ZoneScalingExclusion : uint8
{
    ZS_NOT_EXCLUDED = 0,
    ZS_EXCL_TABLE,          // listed in mod_zone_scaling_exclude
    ZS_EXCL_WORLDBOSS,
    ZS_EXCL_CRITTER,
    ZS_EXCL_TOTEM,
    ZS_EXCL_TRIGGER,
    ZS_EXCL_QUESTGIVER,
    ZS_EXCL_VENDOR,
    ZS_EXCL_TRAINER,
    ZS_EXCL_NO_XP,
    ZS_EXCL_UNATTACKABLE
};

char const* ZoneScalingExclusionName(ZoneScalingExclusion reason);

// A zone's scaling band. A creature in this zone presents to a player at
// clamp(playerLevel, MinLevel, MaxLevel).
struct ZoneScalingBand
{
    uint32 ZoneId   = 0;
    uint8  MinLevel = 0;
    uint8  MaxLevel = 0;
    bool   Enabled  = false;

    // Per-zone override of ZoneScaling.Direction. Imported content built for a
    // higher level cap needs to scale DOWN in its own zones while the open
    // world stays level-up-only, and one global setting cannot say both.
    ZoneScalingDirection Direction = ZS_DIR_INHERIT;

    // Whether this zone scales even for a player who has opted out of Chrono
    // Mode. Chrono is a preference about the OPEN WORLD, where fighting at
    // native levels is a legitimate way to play. Imported content authored for
    // a higher level cap has no such reading: opted out, its creatures are
    // simply unreachable, and the zone is not "unscaled" but broken. A zone
    // that cannot be played unscaled sets this.
    bool IgnoreChrono = false;

    [[nodiscard]] uint8 Clamp(uint8 playerLevel) const
    {
        if (playerLevel < MinLevel)
            return MinLevel;
        if (playerLevel > MaxLevel)
            return MaxLevel;
        return playerLevel;
    }
};

// The stat block a creature template would have at a given level: exactly what
// Creature::SelectLevel would produce, computed without touching a creature.
//
// Everything downstream rests on this. Z-04 derives its damage ratio from the
// damage figures here, so an error at this layer is an error everywhere, which
// is why Z-02 exists as its own milestone with its own command.
struct ZoneScalingStats
{
    uint32 Health            = 0;
    uint32 Mana              = 0;
    float  Armor             = 0.0f;
    float  MinDamage         = 0.0f;
    float  MaxDamage         = 0.0f;
    uint32 AttackPower       = 0;
    uint32 RangedAttackPower = 0;
};

// The two multipliers that make a fight feel like it is against a creature of
// the player's own level, while the creature's real numbers stay canonical.
//
// They are derived from *different* stat curves, and that is the single most
// important thing about this struct. The build plan said "scale inversely",
// which reads as one ratio used both ways; that is only correct if health and
// damage grow proportionally with level, and in creature_classlevelstats they
// do not. Deriving both from one curve produces a fight that is wrong in one
// direction by however much the two curves diverge.
struct ZoneScalingRatios
{
    // Creature -> player. The creature swings for damage appropriate to its
    // canonical level; the player should feel a creature of the effective
    // level. Damage curve: dmg(effective) / dmg(canonical).
    float Incoming = 1.0f;

    // Player -> creature. The creature's real health pool is canonical, but
    // the player should perceive the pool of an effective-level creature. What
    // is observable is the fraction removed per hit, so this is the *health*
    // curve, inverted: hp(canonical) / hp(effective).
    float Outgoing = 1.0f;

    [[nodiscard]] bool IsIdentity() const { return Incoming == 1.0f && Outgoing == 1.0f; }
};

// What to do about proximity aggro range.
//
// Read the header comment in ZoneScalingAggro.cpp before touching this. The
// short version: the *correct* answer -- a per-observer aggro radius -- is not
// reachable from a module, because AzerothCore exposes no hook on
// Creature::GetAggroRange and getLevelForTarget is not overridable. What is
// reachable is making the radius consistent across a scaled zone instead of
// tracking each creature's canonical level, which fixes the incoherence
// without fixing the level dependence.
enum ZoneScalingAggro : uint8
{
    ZS_AGGRO_STOCK     = 0, // leave detection range exactly as the template has it
    ZS_AGGRO_NORMALIZE = 1  // offset it so every creature in a band aggroes alike
};

// What a creature's own, unscaled level should be.
enum ZoneScalingCanonical : uint8
{
    ZS_CANON_NATIVE   = 0,  // whatever the core rolled from the template
    ZS_CANON_MIDPOINT = 1   // the midpoint of its zone's band
};

// Per-creature scaling state, cached once instead of recomputed per hit.
//
// Stored in Object::CustomData rather than in a manager-side map keyed by
// ObjectGuid, which is what the build plan originally called for. Two reasons,
// and the first is a correctness one:
//
//  - Maps are updated on a thread pool (MapMgr::Update -> schedule_update per
//    map), so creatures enter and leave the world on several threads at once.
//    A shared container would need a lock taken on every spawn, despawn, grid
//    load and grid unload. Here each creature's state is touched only by the
//    thread owning its map.
//  - Lifetime stops being our problem. The state is destroyed with the
//    creature, so a missed removal hook cannot leak or go stale -- which is
//    the failure the ObjectGuid key was chosen to avoid in the first place.
//
// The cost is that nothing can enumerate tagged creatures; that is what
// ZoneScalingMgr::GetTaggedCount and the debug log are for.
class ZoneScalingCreatureState : public DataMap::Base
{
public:
    static constexpr char const* Key = "ZoneScaling";

    uint32 ZoneId = 0;

    // Cached rather than re-read from the template on every damage event: the
    // elite offset needs it, and it is fixed for the life of an entry.
    uint8 Rank = 0;

    // What kind of map this creature stands on, resolved once at tag time. A
    // creature does not change map without leaving the world first, so this
    // cannot go stale the way the level can.
    ZoneScalingMapKind MapKind = ZS_MAP_WORLD;

    // The creature's own level, ignoring any player. Refreshed whenever the
    // core reselects it -- see ZoneScalingMgr::OnSelectLevel.
    uint8 CanonicalLevel = 0;

    ZoneScalingExclusion Exclusion = ZS_NOT_EXCLUDED;

    // In a scaled zone and not excluded. Says nothing about any particular
    // player: whether a given pairing actually moves is decided per observer.
    bool Scalable = false;

    // Stats at CanonicalLevel. Z-04 divides by these, so they are cached here
    // rather than recomputed per damage event. False if the level had no
    // usable creature_classlevelstats data.
    bool StatsValid = false;
    ZoneScalingStats CanonicalStats;
};

// Per-player Chrono Mode state, in Object::CustomData for the same reasons the
// creature state is: no manager-side map to lock, and lifetime tied to the
// object rather than to a hook firing.
//
// Two fields rather than one, because "opted out" and "never asked the
// database" are different answers and conflating them is how a login-order bug
// becomes a silent realm-wide opt-out. Loaded is false until the login hook has
// resolved the character's row; anything reading Enabled before that is asking
// a question the module cannot yet answer, and IsScalingActiveFor says so by
// falling back to the configured default rather than to `false`.
class ZoneScalingPlayerState : public DataMap::Base
{
public:
    static constexpr char const* Key = "ZoneScalingPlayer";

    bool Loaded  = false;
    bool Enabled = true;

    // Whether the character had a row of its own, as opposed to inheriting the
    // config default. Only used by `.chrono` to tell a player whether they have
    // ever made this choice -- which changes what the message should say when
    // the realm's default later moves under them.
    bool Explicit = false;
};

// Refreshes the level field of every creature the player can currently see, so
// that a change in what they *should* be shown reaches them within a tick.
//
// Lives in ZoneScalingDisplay.cpp next to the mechanism it depends on, but is
// declared here because Z-08 needs it too and for exactly the same reason
// Z-05 did: a creature only re-sends its level when something marks that field
// dirty, and neither levelling up nor toggling Chrono Mode changes anything
// about the creature. Without this, every mob already on screen keeps the
// number it was sent before the toggle, and the feature looks broken in the one
// moment the player is watching for it to work.
void ZoneScalingRefreshNearbyLevels(Player* player, char const* why);

// What the quest XP correction (Z-09) would do to one quest for one player
// level, and why it would decline if it declines.
//
// This exists because the quest path is the one reward path that cannot be
// exercised without a client -- a soak of AI-driven characters generates kills
// endlessly and quest turn-ins never. `.zs questxp` calls the same function the
// hook calls, so the arithmetic and every guard are tested even though the hook
// firing is not.
struct ZoneScalingQuestXp
{
    int32       QuestLevel  = 0;
    int32       ZoneOrSort  = 0;
    uint8       Effective   = 0;
    uint32      CanonicalXp = 0;
    uint32      EffectiveXp = 0;
    char const* Declined    = nullptr;  // non-null when nothing would move
};

bool ZoneScalingDescribeQuestXp(Quest const* quest, Player const* player, uint8 playerLevel,
                                ZoneScalingQuestXp& out);

// A creature this module is actually scaling, or nullptr. Rejects anything a
// player owns, anything untagged, and anything whose stats could not be built
// at its canonical level.
//
// `StatsValid` is required even where no arithmetic divides by a stat table.
// Without it a creature with no `creature_classlevelstats` row at its level
// would fight at canonical difficulty and pay an effective-level reward -- an
// easy mob worth a hard mob's loot, which is an exploit rather than a rounding
// error.
Creature* ZoneScalingScaledCreature(Unit* unit);

// The level this creature is being fought at by this player, or nullopt when
// nothing about the reward should move. Fills `canonical` with the creature's
// own level either way.
//
// Deliberately not `ZoneScalingMgr::GetEffectiveLevelFor`, which is otherwise
// identical: that one feeds the Chrono-suppressed and elite-offset counters,
// and those count *fights*. Routing a reward through it would count a second
// elite offset for every kill and report suppressed "fights" that were really
// XP or coin awards, making both diagnostics lie.
Optional<uint8> ZoneScalingRewardLevelFor(Creature* creature, Player const* player,
                                          uint8& canonical);

// Warband: the level creatures present at for this player.
//
// The module is per-viewer by construction - every player sees its own level.
// The Warband rule is that the *group* is the unit: every member sees the world
// at the highest member's level, so a party of five fights the same world.
// With ZoneScaling.GroupAnchor = 1 (the Warband default) this returns the
// highest member level of the player's group, or the player's own level when
// solo; with it off the plugin behaves exactly as shipped (per-viewer).
uint8 ZoneScalingAnchorLevel(Player const* player);

// What the gear banding (Z-10) would do to one item at one level, and why it
// would decline if it declines.
//
// Exists for the same reason `ZoneScalingQuestXp` does: the substitution runs
// inside loot generation, which cannot be stepped through from a console, and
// its decision depends on an index built from `item_template` at runtime. The
// command calls the same function the hook calls, so every guard and the pool
// lookup are exercised whether or not anyone is standing on a corpse.
struct ZoneScalingGearSwap
{
    uint32      FromItem     = 0;
    uint32      ToItem       = 0;
    uint8       FromLevel    = 0;   // the original's RequiredLevel
    uint8       ToLevel      = 0;   // the replacement's RequiredLevel
    uint32      Candidates   = 0;   // how many the pool offered within tolerance
    char const* Declined     = nullptr;
};

bool ZoneScalingDescribeGearSwap(uint32 itemId, uint8 effectiveLevel, ZoneScalingGearSwap& out);

class ZoneScalingMgr
{
public:
    static ZoneScalingMgr* instance();

    // Re-read mod_zone_scaling.conf. Called from OnAfterConfigLoad, so it must
    // tolerate being called both at startup and on a live `.reload config`.
    void LoadConfig();

    // Re-read the world-DB band and exclusion tables. Separate from LoadConfig
    // because `.zs reload` should be able to pick up table edits without
    // disturbing config, and vice versa.
    void LoadFromDB();

    [[nodiscard]] bool IsEnabled() const { return _enable; }
    [[nodiscard]] bool IsDebug() const { return _debug; }
    [[nodiscard]] bool IsActive() const { return _enable && _tablesPresent; }

    [[nodiscard]] std::size_t GetBandCount() const { return _bands.size(); }
    [[nodiscard]] std::size_t GetExclusionCount() const { return _excluded.size(); }
    [[nodiscard]] std::size_t GetForcedCount() const { return _forced.size(); }

    // Resolve a zone's band. Returns nullptr when the zone is not scaled --
    // either it has no row and DefaultBehaviour is off, or its row is
    // disabled. Callers must treat nullptr as "leave everything stock".
    // `map`, when given, suppresses the DefaultBehaviour fallthrough on
    // instanced maps -- see the comment in the definition. Diagnostic callers
    // that only have a zone id may omit it.
    [[nodiscard]] ZoneScalingBand const* GetBand(uint32 zoneId, Map const* map = nullptr) const;

    // The level a creature in this zone presents at, for a player of this
    // level. std::nullopt means "this zone is not scaled".
    //
    // The player's level is first lowered by ZoneScaling.TargetOffset (see
    // TargetLevelFor), then clamped to the band, then the elite offset and the
    // direction rule are applied. Every consumer -- damage, the hit table, the
    // level display and the reward corrections -- reads the result of this one
    // function, so the offset moves all of them together.
    //
    // `canonicalLevel` is the creature's own unscaled level, and it is only
    // consulted under ZS_DIR_UP, where the answer is the higher of it and the
    // banded player level. Pass 0 when there is no creature in hand -- the
    // question is then hypothetical and the banded level is returned as-is.
    //
    // A useful property falls out of this: under ZS_DIR_UP a creature above the
    // player returns its own canonical level, so effective == canonical and
    // every downstream stage (the Z-04 damage ratio, the Z-05 level field) is
    // a no-op for that pair without needing a special case.
    // `rank` is the template's CreatureEliteType. It is only consulted when
    // PreserveEliteOffset is on, and defaults to CREATURE_ELITE_NORMAL (0) so
    // that a caller with no template in hand asks the plain question.
    [[nodiscard]] Optional<uint8> GetEffectiveLevel(uint32 zoneId, uint8 playerLevel,
                                                    uint8 canonicalLevel = 0,
                                                    uint8 rank = 0) const;

    [[nodiscard]] ZoneScalingDirection GetDirection() const { return _direction; }

    // --- breadth: where scaling applies, and what stays special (Z-07) -------

    // Five-way, from MapEntry::map_type. Never from the overlapping Map::
    // predicates -- see the ZoneScalingMapKind comment for why that matters.
    // A null map answers ZS_MAP_WORLD; callers that care check IsMapAllowed,
    // which refuses a null map outright.
    [[nodiscard]] static ZoneScalingMapKind ResolveMapKind(Map const* map);

    [[nodiscard]] bool IsMapKindEnabled(ZoneScalingMapKind kind) const
    { return kind < ZS_MAP_KIND_MAX && _mapKindEnabled[kind]; }

    // The gate. Instanced content is off by default, and every path that can
    // tag or scale a creature runs through this -- a zone band alone is not
    // enough to scale anything, because a band is keyed on zone id and a zone
    // id says nothing about whether you are standing in a raid.
    [[nodiscard]] bool IsMapAllowed(Map const* map) const;

    [[nodiscard]] bool IsElitePreserved() const { return _preserveEliteOffset; }
    [[nodiscard]] uint8 GetEliteOffsetMax() const { return _eliteOffsetMax; }

    // ZoneScaling.TargetOffset: how many levels *below* the player a scaled
    // creature is brought to. 0 presents everything at the player's own level;
    // 3 or 4 keeps a scaled zone green rather than yellow, so it still pays XP
    // but is a poorer place to grind than content at your level. Applied to the
    // player's level before the band clamp, so the band floor still wins.
    [[nodiscard]] uint8 GetTargetOffset() const { return _targetOffset; }
    [[nodiscard]] uint8 TargetLevelFor(uint8 playerLevel) const
    { return playerLevel > _targetOffset ? uint8(playerLevel - _targetOffset) : uint8(1); }

    // How far above the band an elite or rare should present, so that a
    // creature which cons dangerous before scaling still cons dangerous after.
    // 0 for normal ranks, for world bosses (never scaled anyway), and whenever
    // the creature is not already above its band's midpoint.
    [[nodiscard]] uint8 EliteOffsetFor(uint32 zoneId, uint8 canonicalLevel, uint8 rank) const;

    // Counted for the same reason as the aggro adjustments: `.zs ratio` proves
    // the arithmetic, but not that the live path reaches it with the right
    // inputs. The rank travels from the template through the cached state, and
    // a rank that silently stayed 0 would leave every number correct and the
    // feature inert -- indistinguishable from working, from outside.
    void CountEliteOffset() const { _eliteOffsets.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] uint64 GetEliteOffsetCount() const
    { return _eliteOffsets.load(std::memory_order_relaxed); }

    // --- Chrono Mode, the per-player opt-in (Z-08) --------------------------

    // Whether the opt-in exists on this realm at all. False means every player
    // is scaled, which is precisely the behaviour of Z-01 through Z-07 -- so
    // the default keeps this milestone from changing anything by itself.
    //
    // Also false when the characters-DB table is missing. That failure has to
    // resolve to "the module behaves as it did before Chrono existed" rather
    // than to "nobody scales", because the second silently discards every zone
    // band the administrator configured.
    [[nodiscard]] bool IsChronoActive() const { return _chronoEnable && _chronoTablePresent; }
    [[nodiscard]] bool IsChronoEnabled() const { return _chronoEnable; }
    [[nodiscard]] bool IsChronoTablePresent() const { return _chronoTablePresent; }
    [[nodiscard]] bool GetChronoDefault() const { return _chronoDefault; }

    // ZoneScaling.Chrono.BotDefault: -1 follows Chrono.Default, 0/1 give
    // playerbots their own default. A bot never runs `.chrono`, so for bots the
    // default *is* the choice. Reported by `.zs status`.
    [[nodiscard]] int8 GetChronoBotDefault() const { return _chronoBotDefault; }

    // A session with no socket has no real remote address: upstream leaves it
    // empty, the mod-playerbots fork stamps it "bot". Checked this way rather
    // than through the fork-only `WorldSession::IsBot()`, so the module still
    // builds on a stock core, where the answer is simply always false.
    [[nodiscard]] static bool IsBotPlayer(Player const* player);

    // What a character with no stored choice does: Chrono.BotDefault for a
    // bot when it is set, Chrono.Default otherwise.
    [[nodiscard]] bool ChronoDefaultFor(Player const* player) const;

    // The one question the rest of the module asks about a player. True when
    // Chrono is inactive, so every caller can be written as though the opt-in
    // did not exist.
    [[nodiscard]] bool IsScalingActiveFor(Player const* player) const;

    // Zone-aware form. Identical to the above except that a band flagged
    // IgnoreChrono answers true regardless of the player's opt-out. Callers in
    // the damage, kill-XP and quest-XP paths use this one; the Chrono XP BONUS
    // deliberately does not, because that bonus is the reward for choosing to
    // be scaled and must not be paid to someone who did not choose it.
    [[nodiscard]] bool IsScalingActiveFor(Player const* player, uint32 zoneId) const;

    [[nodiscard]] static ZoneScalingPlayerState* GetPlayerState(Player const* player);

    // Resolve a character's stored flag and attach state. Called on login.
    void OnPlayerLogin(Player* player) const;

    // Persist a change and refresh what the player can see. Returns false if
    // the write could not be attempted, which at this point means only that the
    // table is missing.
    bool SetChrono(Player* player, bool enabled) const;

    // Whether a player is somewhere they are allowed to toggle: out of combat
    // and in a rested area. `why` is filled with a player-facing reason when
    // the answer is false.
    [[nodiscard]] static bool CanToggleChrono(Player const* player, std::string& why);

    // Counts fights that *would* have been scaled and were not, because the
    // player had opted out. Same instrument as the aggro and elite counters,
    // and added for the same reason: the opt-in path returns nothing and
    // changes nothing, so from outside, "the flag is respected" and "the flag
    // never reaches the hot path" produce identical realms.
    void CountChronoSuppressed() const { _chronoSuppressed.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] uint64 GetChronoSuppressedCount() const
    { return _chronoSuppressed.load(std::memory_order_relaxed); }

    void CountChronoToggled() const { _chronoToggled.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] uint64 GetChronoToggledCount() const
    { return _chronoToggled.load(std::memory_order_relaxed); }

    // --- rewards: paying for a scaled kill or quest (Z-09) ------------------
    //
    // Scaling a creature up without scaling its reward is worse than not
    // scaling it: the zone becomes harder *and* pointless. These three switches
    // are separate because they carry very different risk. The kill correction
    // rides an in-tree hook and is arithmetic on a value the core hands us; the
    // grey top-up reimplements part of the core's reward pipeline, for reasons
    // set out at length in ZoneScalingRewards.cpp; the quest correction touches
    // a reward players tend to notice being wrong.
    [[nodiscard]] bool IsXpScalingEnabled() const { return _xpEnable; }
    [[nodiscard]] bool IsGreyKillXpEnabled() const { return _xpEnable && _xpGreyKills; }
    [[nodiscard]] bool IsQuestXpScalingEnabled() const { return _xpEnable && _xpQuests; }

    // Percent added to every XP award a Chrono opt-in receives, as the reward
    // for taking the harder realm. 0 disables it. Only ever applied when
    // Chrono is active *and* the player has actually opted in -- a realm with
    // no opt-in has no one to pay a bonus to.
    [[nodiscard]] uint32 GetChronoXpBonusPct() const { return _chronoXpBonus; }
    [[nodiscard]] bool IsChronoXpBonusActive() const
    { return _chronoXpBonus != 0 && IsChronoActive(); }

    // Kill XP corrected by the ratio of base gains, grey top-ups paid, quest
    // awards moved, and Chrono bonuses added. Same instrument, same reason as
    // every other counter here: all four paths mutate a value in place and
    // return nothing, so from outside "working" and "never reached" look alike.
    void CountXpKillAdjusted() const { _xpKillAdjusted.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] uint64 GetXpKillAdjustedCount() const
    { return _xpKillAdjusted.load(std::memory_order_relaxed); }

    void CountXpGreyPaid(uint32 amount) const
    {
        _xpGreyPaid.fetch_add(1, std::memory_order_relaxed);
        _xpGreyAmount.fetch_add(amount, std::memory_order_relaxed);
    }
    [[nodiscard]] uint64 GetXpGreyPaidCount() const
    { return _xpGreyPaid.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64 GetXpGreyAmount() const
    { return _xpGreyAmount.load(std::memory_order_relaxed); }

    void CountXpQuestAdjusted() const { _xpQuestAdjusted.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] uint64 GetXpQuestAdjustedCount() const
    { return _xpQuestAdjusted.load(std::memory_order_relaxed); }

    void CountXpChronoBonus() const { _xpChronoBonus.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] uint64 GetXpChronoBonusCount() const
    { return _xpChronoBonus.load(std::memory_order_relaxed); }

    // --- loot: coin and gear for a fight the module changed (Z-10) ----------
    //
    // Split from the XP switches rather than folded into them because the two
    // halves of this milestone carry very different risk. Coin is arithmetic on
    // a number the core hands us and is on by default; gear *rewrites which
    // items the world drops*, which is a far larger claim on an administrator's
    // realm, so it is off by default and stays off unless asked for.
    [[nodiscard]] bool IsLootScalingEnabled() const { return _lootEnable; }
    [[nodiscard]] bool IsLootMoneyScalingEnabled() const { return _lootEnable && _lootMoney; }
    [[nodiscard]] bool IsLootGearBandingEnabled() const { return _lootEnable && _lootGear; }

    [[nodiscard]] float GetLootMoneyMax() const { return _lootMoneyMax; }
    [[nodiscard]] uint8 GetLootGearTolerance() const { return _lootGearTolerance; }
    [[nodiscard]] uint8 GetLootGearMaxQuality() const { return _lootGearMaxQuality; }

    // Coin multiplier for a creature moving from `canonical` to `effective`.
    //
    // Read from `quest_money_reward`, which is Blizzard's own per-level
    // statement of what a copper reward at each level is worth -- complete and
    // non-decreasing for levels 1-80 on a stock world DB. This module invents
    // no coin curve of its own, for exactly the reason Z-09 took a ratio of
    // `BaseGain` rather than recomputing an award: a curve fitted here would be
    // a constant nobody could check.
    //
    // std::nullopt when either level has no row, which is the only case the
    // caller must treat as "leave the coin alone".
    [[nodiscard]] Optional<float> DescribeMoneyMultiplier(uint8 canonical, uint8 effective) const;

    // Coin scaled and gear rebanded. Same instrument, same reason as every
    // other counter here: both paths mutate something and return nothing, so
    // from outside "working" and "never reached" look alike. The coin counter
    // also carries the signed copper delta, because a realm running
    // `Direction = both` will scale some loot down, and a count alone cannot
    // distinguish "this paid out sensibly" from "this quietly halved the
    // economy".
    void CountLootMoneyScaled(int64 delta) const
    {
        _lootMoneyScaled.fetch_add(1, std::memory_order_relaxed);
        _lootMoneyDelta.fetch_add(delta, std::memory_order_relaxed);
    }
    [[nodiscard]] uint64 GetLootMoneyScaledCount() const
    { return _lootMoneyScaled.load(std::memory_order_relaxed); }
    [[nodiscard]] int64 GetLootMoneyDelta() const
    { return _lootMoneyDelta.load(std::memory_order_relaxed); }

    void CountLootGearBanded() const { _lootGearBanded.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] uint64 GetLootGearBandedCount() const
    { return _lootGearBanded.load(std::memory_order_relaxed); }

    // --- per-creature lifecycle (Z-03) -------------------------------------
    //
    // Called from the AllCreatureScript hooks. Kept on the manager rather than
    // in the script so the commands can reuse the accessors.

    // Attach state on entering the world. Idempotent.
    void OnAddWorld(Creature* creature);

    // Release state on leaving the world. Safe on an untagged creature.
    void OnRemoveWorld(Creature* creature);

    // Recompute the canonical level after the core reselects it.
    //
    // This hook is not redundant with OnAddWorld, and finding out why cost
    // some reading: Creature::Respawn calls SelectLevel() on a creature that
    // never left the world, and SelectLevel rolls urand(minlevel, maxlevel).
    // A creature with a level range therefore changes level on every respawn
    // while OnAddWorld stays silent. Without this the cache goes quietly stale
    // and every downstream ratio is computed against a level the creature no
    // longer has.
    void OnSelectLevel(Creature* creature);

    // Optionally override the level the core is about to assign. Runs during
    // creature creation, before the creature is in the world.
    void OnBeforeSelectLevel(CreatureTemplate const* cinfo, Creature* creature, uint8& level) const;

    // Whether this creature is worth carrying state for at all: module active,
    // not owned by a player, not excluded, and standing in a scaled zone.
    [[nodiscard]] bool IsCandidate(Creature* creature) const;

    [[nodiscard]] static ZoneScalingCreatureState* GetState(Creature* creature);

    // Live count of creatures holding state. The leak metric: it should track
    // the population of loaded grids and return to its floor when they unload.
    [[nodiscard]] uint32 GetTaggedCount() const { return _taggedCount.load(std::memory_order_relaxed); }

    [[nodiscard]] ZoneScalingCanonical GetCanonicalMode() const { return _canonical; }

    // --- proximity aggro (Z-06) ---------------------------------------------

    [[nodiscard]] ZoneScalingAggro GetAggroMode() const { return _aggroMode; }

    // The midpoint of a zone's band, or std::nullopt when the zone is unscaled.
    // Shared by the midpoint canonical mode and the aggro offset so the two
    // cannot drift apart -- under CanonicalLevel = midpoint the offset is
    // exactly zero, and that has to remain true by construction rather than by
    // two functions happening to round the same way.
    [[nodiscard]] Optional<uint8> GetBandMidpoint(uint32 zoneId) const;

    // The detection distance a creature should carry so that proximity aggro is
    // the same for every creature in a scaled zone. std::nullopt means leave
    // the template's value alone. Answers regardless of the configured mode --
    // it is the predictor `.zs aggro` uses to show what normalization would do
    // before it is switched on; only ApplyAggroRange consults the mode.
    [[nodiscard]] Optional<float> NormalizedDetectionRange(CreatureTemplate const* cinfo,
                                                           uint32 zoneId,
                                                           uint8 canonicalLevel) const;

    // Applied at tag time and refreshed whenever the core reselects a level --
    // which is also when Creature::InitEntry has just overwritten the detection
    // range with the template's, so this must run after it and does.
    void ApplyAggroRange(Creature* creature, ZoneScalingCreatureState const& state) const;

    // Puts the template's own value back, so nothing this module did outlives
    // the creature's time in a scaled zone.
    void RestoreAggroRange(Creature* creature) const;

    // Counted because ApplyAggroRange writes to a creature and returns nothing,
    // so without this there is no way to tell "normalization is working" from
    // "normalization never fired" without selecting a creature in-game. Counts
    // writes, not creatures: a creature re-entering the world or changing entry
    // is adjusted again and counted again.
    void CountAggroAdjusted() const { _aggroAdjusted.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] uint64 GetAggroAdjustedCount() const
    { return _aggroAdjusted.load(std::memory_order_relaxed); }

    // Creature::GetAggroRange reimplemented for the `.zs aggro` diagnostic,
    // minus the two aura terms it cannot know about. Exists so the size of the
    // gap this milestone could not close is a measured number rather than an
    // argument.
    [[nodiscard]] float PredictAggroRadius(float detectionRange, uint8 playerLevel,
                                           uint8 creatureLevel) const;

    // --- combat normalization (Z-04) ---------------------------------------

    [[nodiscard]] bool IsDamageScalingEnabled() const { return _scaleDamage; }
    [[nodiscard]] bool IsHitChanceScalingEnabled() const { return _scaleHitChance; }
    [[nodiscard]] bool IsLevelDisplayEnabled() const { return _scaleLevelDisplay; }

    // --- the core view (Z-14) --------------------------------------------
    //
    // With the Z-14 core patch applied, `Creature::getLevelForTarget` and
    // armor mitigation ask the module for a viewer's version of a creature
    // (see ZoneScalingView.cpp). `CoreView` is the config switch; `Installed`
    // records whether this build's core had the header at all.
    [[nodiscard]] bool IsCoreViewEnabled() const { return _coreView; }
    [[nodiscard]] bool IsCoreViewInstalled() const { return _coreViewInstalled; }
    void SetCoreViewInstalled(bool installed) { _coreViewInstalled = installed; }
    [[nodiscard]] bool IsPoolDisplayEnabled() const { return _scalePoolDisplay; }

    // GetEffectiveLevelFor without the diagnostics. The core asks this from
    // inside getLevelForTarget -- per aggro check, per roll, per XP award --
    // and routing that through the counting form would make "scaled fights"
    // and "Chrono-suppressed fights" count queries instead of fights.
    [[nodiscard]] Optional<uint8> GetViewLevelFor(Creature const* creature,
                                                  Player const* player) const;

    // The armor of the viewer's version, or 0 when there is no view.
    [[nodiscard]] uint32 GetViewArmorFor(Creature const* creature, Player const* player) const;

    // The health and mana bars of the viewer's version: the real pool scaled
    // by the health (and mana) curve between canonical and effective level,
    // with the current values carried as the same share. False when the
    // pairing is not scaled or the stats are unusable.
    struct ViewPool
    {
        uint32 MaxHealth = 0;
        uint32 Health    = 0;
        uint32 MaxMana   = 0;   // 0 = leave the mana fields alone
        uint32 Mana      = 0;
    };
    [[nodiscard]] bool GetViewPool(Creature const* creature, uint8 effective, ViewPool& out) const;

    // The level this creature presents at to this player, or std::nullopt when
    // the pairing is not scaled at all. Shared by the damage ratios and the
    // melee hit table, which must agree or the two corrections fight.
    [[nodiscard]] Optional<uint8> GetEffectiveLevelFor(Creature* creature,
                                                       Player const* player) const;

    // The ratios for a live pairing. Returns identity for anything that must
    // not move -- module off, creature untagged or unscalable, no player on the
    // other side, or stats unavailable at either level. Callers can therefore
    // apply the result unconditionally.
    [[nodiscard]] ZoneScalingRatios GetRatios(Creature* creature, Player const* player) const;

    // The same arithmetic with no creature in hand, for `.zs ratio`. Reports
    // the effective level it resolved through, so the command can show its
    // working. std::nullopt means the zone is not scaled or the stats are
    // unusable -- the two cases the hot path silently treats as identity.
    [[nodiscard]] Optional<ZoneScalingRatios> DescribeRatios(CreatureTemplate const* cinfo,
                                                            uint32 zoneId, uint8 canonicalLevel,
                                                            uint8 playerLevel,
                                                            uint8& effectiveLevel) const;

    // Applies a ratio to a damage amount, rounding away from zero so that a
    // heavily scaled-down hit lands for 1 rather than 0. A hit that does
    // nothing at all reads as immunity, breaks proc and threat expectations,
    // and makes a fight unwinnable rather than merely slow.
    [[nodiscard]] static uint32 ApplyRatio(uint32 amount, float ratio);

    // Counters, not logs. Z-03 was debugged largely by grepping a debug log,
    // which cost more time than any real bug in it -- a per-swing log line is
    // both too noisy to leave on and too easy to mis-grep. These answer "is
    // this actually firing in the wild" from `.zs status`, with no logging
    // enabled and no cost beyond a relaxed increment.
    void CountDamageScaled() { _damageScaled.fetch_add(1, std::memory_order_relaxed); }
    void CountHitTableFixed() { _hitTableFixed.fetch_add(1, std::memory_order_relaxed); }

    [[nodiscard]] uint64 GetDamageScaledCount() const { return _damageScaled.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64 GetHitTableFixedCount() const { return _hitTableFixed.load(std::memory_order_relaxed); }

    // --- per-observer level display (Z-05) ----------------------------------
    //
    // Three counters, because two were not enough to answer the milestone's
    // "confirm the values-update cache still hits" acceptance without a
    // profiler -- and the first attempt at two got the wrong answer in a way
    // worth recording.
    //
    // The core caches one built SMSG_UPDATE_OBJECT field buffer per (visibility
    // flags, update type) and reuses it for every observer sharing that key,
    // patching per-target afterwards. So:
    //
    //  - Tracked counts *buffer builds*: ShouldTrackValuesUpdatePosByIndex is
    //    consulted only while a fresh buffer is assembled, never on a cache hit.
    //  - Served counts *observers*: every target the patch hook was handed a
    //    tracked level field for, hit path included.
    //  - Patched counts observers whose number actually changed.
    //
    // The cache hit rate is 1 - Tracked / Served. It is emphatically *not*
    // 1 - Tracked / Patched, which is what this module measured first: the two
    // hooks do not apply the same test. ShouldTrack knows only the creature, so
    // it fires for every scalable one; the patch hook additionally knows the
    // observer and drops the pairing when the effective level equals the
    // canonical one. Tracked therefore exceeds Patched whenever players happen
    // to be near their own level -- which read as a negative hit rate, an
    // impossible number that pointed straight at the conflated denominator.
    void CountLevelTracked() { _levelTracked.fetch_add(1, std::memory_order_relaxed); }
    void CountLevelServed() { _levelServed.fetch_add(1, std::memory_order_relaxed); }
    void CountLevelPatched() { _levelPatched.fetch_add(1, std::memory_order_relaxed); }

    [[nodiscard]] uint64 GetLevelTrackedCount() const { return _levelTracked.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64 GetLevelServedCount() const { return _levelServed.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64 GetLevelPatchedCount() const { return _levelPatched.load(std::memory_order_relaxed); }

    // Whether this template may ever be scaled. Evaluated from the template
    // alone -- no spawned Creature is needed -- so the answer is stable and
    // can be cached per entry by later milestones.
    [[nodiscard]] ZoneScalingExclusion GetExclusion(CreatureTemplate const* cinfo) const;

    // The stats this template would have at `level`, mirroring
    // Creature::SelectLevel. False means the answer would have been nonsense
    // and `out` is untouched -- see HasBaseStats for why that is possible.
    [[nodiscard]] bool ComputeStats(CreatureTemplate const* cinfo, uint8 level,
                                    ZoneScalingStats& out) const;

    // Whether creature_classlevelstats holds *usable* data for the exact
    // (class, level, expansion) this template will read. Not "a row exists".
    //
    // This matters more than it looks, and in three ways that compound:
    //
    //  - ObjectMgr::GetCreatureBaseStats does not fail for a missing pair. It
    //    returns a static default of BaseHealth 1 and BaseDamage 0.
    //  - Rows above the real data ceiling are present but are *placeholders*
    //    carrying those same values, and there are holes below the ceiling.
    //  - Placeholders are per *expansion column*, not per row. Measured on a
    //    stock world DB: class 2 level 85 has basehp0 = 1 but basehp2 = 15498.
    //
    // The last point is why this takes a template rather than a class: which
    // column is read depends on cinfo->expansion. It is also why the test
    // cannot be applied to the finished number -- ModHealth multiplies the
    // placeholder 1 up into a plausible-looking value. Bolvar at level 85 came
    // out as 300 HP that way, which looks like data rather than a bug.
    [[nodiscard]] static bool HasBaseStats(CreatureTemplate const* cinfo, uint8 level);

    // The usable ceiling that holds for every class -- the one a zone band must
    // respect, since a zone holds creatures of all four unit classes.
    [[nodiscard]] uint8 LowestMaxUsableLevel() const;

    // Human-readable explanation of GetBand's answer, for `.zs zone`.
    [[nodiscard]] std::string DescribeBand(uint32 zoneId) const;

    // One-line summary of module state, for `.zs status`.
    [[nodiscard]] std::string DescribeStatus() const;

private:
    ZoneScalingMgr() = default;

    static bool TableExists(char const* table);
    static bool CharacterTableExists(char const* table);
    bool LoadBands();
    bool LoadExclusions();
    bool LoadForced();
    void LoadBaseStatsCoverage();

    bool _enable         = false;
    bool _debug          = false;
    bool _scaleDamage       = true;
    bool _scaleHitChance    = true;
    bool _scaleLevelDisplay = true;
    bool _scalePoolDisplay  = true;
    bool _coreView          = true;
    bool _coreViewInstalled = false;

    // Ratios are clamped into this range before use. Nothing legitimate gets
    // near either end -- the widest band this core can express, level 1 against
    // level 73, stays well inside -- so hitting a bound means a stat lookup has
    // returned something absurd, and the clamp is there so that a bad number
    // produces a strange fight rather than an instant kill or an invulnerable
    // mob while the cause is found.
    static constexpr float MinRatio = 0.01f;
    static constexpr float MaxRatio = 100.0f;

    static ZoneScalingRatios RatiosFrom(ZoneScalingStats const& canonical,
                                        ZoneScalingStats const& effective);

    ZoneScalingDefault   _defaultBehaviour = ZS_DEFAULT_OFF;
    ZoneScalingDirection _direction        = ZS_DIR_BOTH;

    // Whether an instanced map needs an explicit mod_zone_scaling_zone row
    // before it will scale, regardless of DefaultBehaviour.
    bool _instancedRequireExplicit = true;
    ZoneScalingCanonical _canonical        = ZS_CANON_NATIVE;
    ZoneScalingAggro     _aggroMode        = ZS_AGGRO_STOCK;

    // Open world on, everything instanced off. Dungeons and raids are tuned
    // encounters with their own difficulty contract, and a band keyed on zone
    // id will happily match a raid's zone -- so the safe default has to be an
    // explicit opt-in per kind rather than "whatever the zone table says".
    bool _mapKindEnabled[ZS_MAP_KIND_MAX] = { true, false, false, false, false };

    bool  _preserveEliteOffset = true;
    uint8 _eliteOffsetMax      = 3;
    uint8 _targetOffset        = 0;

    // Off by default, so installing this milestone changes nothing. When it is
    // switched on, _chronoDefault decides what every existing character does on
    // their next login -- and at the shipped value that is "stop being scaled",
    // which is a realm-wide behaviour change from one config line. LoadConfig
    // warns about it every startup rather than only the first.
    bool _chronoEnable  = false;
    bool _chronoDefault = false;
    int8 _chronoBotDefault = -1;
    bool _chronoTablePresent = false;

    // On by default, unlike Chrono: a realm that has switched scaling on has
    // already accepted that fights change, and leaving the rewards uncorrected
    // is not a neutral position -- it is the specific failure this milestone
    // exists to prevent. The two sub-switches exist so an administrator can
    // keep the safe half if the reimplemented half misbehaves.
    bool   _xpEnable      = true;
    bool   _xpGreyKills   = true;
    bool   _xpQuests      = true;
    uint32 _chronoXpBonus = 10;

    // Coin on, gear off. See IsLootGearBandingEnabled for why the two defaults
    // differ. The clamp is generous rather than tight: the shipped money curve
    // really does say a level-80 kill is worth ~770x a level-5 one, and a
    // number that large is a judgement about the realm's economy, not an error
    // to be silently corrected -- so the clamp is there to stop an absurd
    // number, and the conf tells an administrator to lower it if the honest
    // one is still too much.
    bool   _lootEnable         = true;
    bool   _lootMoney          = true;
    bool   _lootGear           = false;
    float  _lootMoneyMax       = 50.0f;
    uint8  _lootGearTolerance  = 5;
    uint8  _lootGearMaxQuality = 3;

    // Written from every map thread, so atomic. relaxed ordering is enough:
    // nothing is published through it, it is only ever read for display.
    std::atomic<uint32> _taggedCount{0};

    // Mutable because ApplyAggroRange is const -- it changes the creature, not
    // the manager, and that distinction is worth keeping in the signature.
    mutable std::atomic<uint64> _aggroAdjusted{0};
    mutable std::atomic<uint64> _eliteOffsets{0};
    mutable std::atomic<uint64> _chronoSuppressed{0};
    mutable std::atomic<uint64> _chronoToggled{0};

    mutable std::atomic<uint64> _xpKillAdjusted{0};
    mutable std::atomic<uint64> _xpGreyPaid{0};
    mutable std::atomic<uint64> _xpGreyAmount{0};
    mutable std::atomic<uint64> _xpQuestAdjusted{0};
    mutable std::atomic<uint64> _xpChronoBonus{0};

    mutable std::atomic<uint64> _lootMoneyScaled{0};
    mutable std::atomic<int64>  _lootMoneyDelta{0};
    mutable std::atomic<uint64> _lootGearBanded{0};

    // 64-bit: a busy realm generates damage events fast enough to wrap a 32-bit
    // counter in a long uptime, and a wrapped diagnostic is worse than none.
    std::atomic<uint64> _damageScaled{0};
    std::atomic<uint64> _hitTableFixed{0};
    std::atomic<uint64> _levelTracked{0};
    std::atomic<uint64> _levelServed{0};
    std::atomic<uint64> _levelPatched{0};

    // Fills a state block from a creature already in the world.
    void BuildState(Creature* creature, ZoneScalingCreatureState& state) const;

    // The band applied to unlisted zones when _defaultBehaviour is CLAMP.
    ZoneScalingBand _defaultBand;

    // True once LoadFromDB has run and found its tables. While false the
    // module refuses to do anything, so a missing SQL install degrades to
    // "does nothing" rather than to a crash.
    bool _tablesPresent = false;

    std::unordered_map<uint32, ZoneScalingBand> _bands;
    std::unordered_set<uint32> _excluded;

    // World bosses opted back into scaling. Read only by GetExclusion, and
    // only for the ZS_EXCL_WORLDBOSS rule -- see mod_zone_scaling_force.
    std::unordered_set<uint32> _forced;

    // (unitClass << 8 | expansion) -> highest level with usable data, for the
    // startup diagnostic and the band-ceiling warning only. HasBaseStats does
    // not consult this; it reads the core's own stat block, which is exact.
    std::unordered_map<uint32, uint8> _maxUsableLevel;

    static constexpr uint32 CoverageKey(uint8 unitClass, uint8 expansion)
    {
        return (uint32(unitClass) << 8) | uint32(expansion);
    }
};

#define sZoneScalingMgr ZoneScalingMgr::instance()

#endif // MOD_ZONE_SCALING_H
