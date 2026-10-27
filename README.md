# mod-zone-scaling

Open-world level scaling for AzerothCore 3.3.5a. Mobs, quest rewards and loot
present themselves to each player at that player's own level, within a
configurable per-zone band — retail-Legion style, on a WotLK core.

> **Status: pre-release (Z-10).** Per-player damage normalization, the melee
> hit-table correction, the per-observer level display, optional aggro-range
> normalization, per-map-kind gating, elite/rare offset preservation, the
> per-player Chrono Mode opt-in, experience-reward correction and loot
> correction — coin, and optional level-banded gear — are implemented. The
> arithmetic is verified against an independent code path, and the display path
> is verified to fire in the wild. **Nothing has been verified through a game
> client**: nobody has fought a scaled mob with two accounts at different levels
> and measured time-to-kill, and nobody has seen two nameplates disagree. Treat
> the numbers as sound and the experience as untested.

## What makes this different

Existing scaling modules are instance-only:

| Module | Scope |
|---|---|
| `mod-autobalance` | instances, by player count |
| `mod-dungeon-master` | instances, repopulation |
| `mod-zone-difficulty` | per-zone difficulty modifiers, not level scaling |

This one targets the open world, and it scales **per player rather than per
mob**. Two players thirty levels apart can fight the same mob at the same time,
each at their own difficulty, seeing their own nameplate level. That works
because AzerothCore builds `SMSG_UPDATE_OBJECT` per receiver and exposes the
per-observer field patch path to modules
(`UnitScript::ShouldTrackValuesUpdatePosByIndex` and
`UnitScript::OnPatchValuesUpdate`).

**No core modifications are required.** A stock client works unchanged.

## Requirements

- AzerothCore, any recent master. Verified against `ceeb311`.
- No other module is required. `mod-playerbots` is supported where present but
  is not a dependency.

## Install

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/firstoni-dev/mod-zone-scaling.git
cd ../build && cmake .. && make -j$(nproc) && make install
```

Apply to your **world** database:

| File | What it does |
|---|---|
| `data/sql/db-world/base/mod_zone_scaling_base.sql` | the module's own tables — required |
| `data/sql/db-world/base/mod_zone_scaling_commands.sql` | help text for `.help zs`, `.help zs ratio` and the rest — required |
| `data/sql/db-world/updates/*.sql` | column additions for installs made before the per-zone `direction` and `ignore_chrono` columns existed — a fresh install gets them from the base file |

And, only if you intend to use Chrono Mode,
`data/sql/db-characters/base/mod_zone_scaling_character.sql` to your
**characters** database. AzerothCore's `db_assembler` picks all of these up
automatically if you run with `USE_SQL_AUTO_UPDATE`.

`mod_zone_scaling_commands.sql` is the only file here that writes to a core
table. It is scoped to the fifteen command names this module owns, and it uses
`DELETE` + `INSERT` deliberately — help text is reference data that should track
the code, so being re-applied when the file changes is the wanted behaviour. The
module's *own* tables use `CREATE TABLE IF NOT EXISTS` for the opposite reason:
they hold hand-authored bands that must survive an update.

There is also `optional-sql/mod_zone_scaling_chrono_npc.sql`, which creates an
example NPC. It lives **outside `data/sql/`** so that the updater will not apply
it — AzerothCore walks a module's `data/sql/db-world` recursively and has no
notion of an "optional" subdirectory, so a file placed in
`data/sql/db-world/optional/` is installed on the next startup just like one in
`base/`. Run it yourself if you want it. See "A gossip NPC, if you want one".

**Optional core patch.** One documented limitation — glancing and crushing
blows deciding on the creature's real level — needs a two-line change in the
core that a module cannot make. It is in `patches/`, it is a no-op on a core
without this module, and the reasoning is under "Glancing and crushing blows:
the optional core patch" below. Skip it and everything else works.

Then edit `mod_zone_scaling.conf` in your config directory. The module ships
disabled; nothing changes until you set `ZoneScaling.Enable = 1` **and** add at
least one row to `mod_zone_scaling_zone`.

## Configuration

Every option is documented inline in `conf/mod_zone_scaling.conf.dist`. The two
that matter on first run:

| Option | Default | Effect |
|---|---|---|
| `ZoneScaling.Enable` | `0` | master switch |
| `ZoneScaling.DefaultBehaviour` | `off` | whether zones absent from the table scale at all |
| `ZoneScaling.Direction` | `both` | whether scaling may lower a creature, or only raise it |
| `ZoneScaling.TargetOffset` | `0` | how many levels below the player a scaled creature lands (3-4 for an Ascension feel) |
| `ZoneScaling.CanonicalLevel` | `native` | what a creature's own level is, before any player is considered |
| `ZoneScaling.ScaleDamage` | `1` | normalize damage between players and scaled creatures |
| `ZoneScaling.ScaleHitChance` | `1` | make the melee hit table use the effective level too |
| `ZoneScaling.ScaleLevelDisplay` | `1` | show each player the creature's effective level |
| `ZoneScaling.AggroRange` | `stock` | whether proximity aggro is normalized across a scaled zone |
| `ZoneScaling.Maps.World` | `1` | scale the open world |
| `ZoneScaling.Maps.Dungeon` | `0` | scale 5-man instances, normal and heroic |
| `ZoneScaling.Maps.Raid` | `0` | scale raids |
| `ZoneScaling.Maps.Battleground` | `0` | scale battlegrounds |
| `ZoneScaling.Maps.Arena` | `0` | scale arenas |
| `ZoneScaling.PreserveEliteOffset` | `1` | keep elites and rares above the mobs around them |
| `ZoneScaling.PreserveEliteOffset.Max` | `3` | how far above, in levels |
| `ZoneScaling.Chrono.Enable` | `0` | make scaling a per-player choice rather than a realm state |
| `ZoneScaling.Chrono.Default` | `0` | what a character who has never chosen does |
| `ZoneScaling.Chrono.BotDefault` | `-1` | the same question for playerbots; `-1` follows `Chrono.Default` |
| `ZoneScaling.Chrono.XpBonus` | `10` | percent added to every XP award a Chrono opt-in receives |
| `ZoneScaling.Xp.Enable` | `1` | correct XP to match the level content was fought at |
| `ZoneScaling.Xp.GreyKills` | `1` | pay for a creature that is grey at its own level but is being fought at yours |
| `ZoneScaling.Xp.Quests` | `1` | price a quest at the level its zone presents at |
| `ZoneScaling.Loot.Enable` | `1` | master switch for the loot corrections |
| `ZoneScaling.Loot.Money` | `1` | scale a scaled creature's coin drop to its effective level |
| `ZoneScaling.Loot.MoneyMax` | `50.0` | cap on the coin multiplier, in either direction |
| `ZoneScaling.Loot.Gear` | `0` | replace eligible equipment drops with same-slot items at the effective level or up to three levels below |
| `ZoneScaling.Loot.Gear.Tolerance` | `3` | maximum levels below the effective level for replacement gear; replacements are never higher than the effective level |
| `ZoneScaling.Loot.Gear.MaxQuality` | `3` | highest quality rebanded (2 uncommon, 3 rare, 4 epic) |

## Where scaling applies

A dungeon has a zone id like anywhere else, so `DefaultBehaviour = clamp`
would otherwise sweep instanced content in without anyone asking. The five
`Maps.*` switches are independent and **only the open world defaults on**.

Instanced content is designed around a fixed encounter difficulty and a group
that meets it; scaling it is a different feature with different failure modes,
not the same one applied more widely. If you enable dungeons or raids the
module logs a warning at startup saying so. Nothing stops you — but the
warning is there because it is a decision, not a default.

`.zs sweep <mapId> [zoneId]` answers "what would actually scale here" without
flying anywhere: it walks every spawn on a map, resolves its zone, runs it
through the real predicates, and reports a breakdown plus an explicit
PASS/FAIL. Use it after any band change, and on an instance map to confirm the
gate holds.

## Chrono Mode — scaling as a player's choice

With `Chrono.Enable = 1`, scaling stops being a property of the realm and
becomes a per-character flag, the way retail war mode is. A player with it off
fights every creature at its own level, sees its real level on the nameplate,
and is entirely unaffected by your zone bands. A player with it on gets the
whole module.

```
.chrono            what your flag is, and whether you can change it here
.chrono on         opt in
.chrono off        opt out
```

**You can only toggle out of combat and in an inn, a capital city or a faction
rest zone.** That is not ceremony. Without it, a player losing a scaled fight
could flip the creature trivial mid-pull, and the reverse — tag something weak,
opt in, kill it for the scaled reward — is the same exploit read backwards. One
rule closes both, and it is a rule players already know from retail. A GM may
toggle anywhere, which is also how you rescue a player stuck somewhere the
core never sets a rest flag.

Choices persist immediately, not at logout, so a crash cannot silently discard
one.

### Choosing a default

`Chrono.Default` decides what a character with no stored choice does, and the
decision is more consequential than it looks:

- **`0` — true opt-in.** On the day you enable Chrono, nobody has a stored
  choice, so *every* character stops being scaled and your zone bands have no
  visible effect until players run `.chrono on`. The module warns about this at
  every startup.
- **`1` — opt-out.** Scaling is the realm's normal state and the flag is an
  escape hatch. Nobody's experience changes on the day you enable Chrono.

Changing this later moves everyone who has never chosen, live, without a
restart or a relog. A stored choice always wins.

**Playerbots** get their own answer through `Chrono.BotDefault`. A bot never
runs `.chrono`, so for a bot the default is the choice, and the useful pairing
is `Chrono.Default = 0` with `Chrono.BotDefault = 1`: people opt in, the bots
that populate the world are always scaled. A bot is recognised by its session
having no remote address, which is how mod-playerbots creates every bot
session; the module does not link against the playerbots module and builds
unchanged on a core without it. Bots are never paid the Chrono XP bonus, since
they did not choose anything.

Requires `mod_zone_scaling_character` in your **characters** database. If it is
missing the module logs an error and behaves as though Chrono were off — so a
forgotten SQL file costs you the opt-in, not the scaling.

### A gossip NPC, if you want one

`.chrono` is complete on its own. The NPC exists because the player who most
needs the opt-in is the least likely to know a slash command exists — someone
who logs in, finds a level 7 boar hitting like a level 60 one, and has no idea
that was a choice somebody made.

The script registers as `npc_zone_scaling_chrono` and then waits. It drives
**any** creature whose `creature_template.ScriptName` is set to that string, and
this module's automatic SQL creates none, so by default nothing in your world
changes. Two ways to get one:

```sql
-- 1. the example NPC: entry 190000, "Keeper of Hours"
--    (not applied automatically; run it yourself)
mysql -u acore -p acore_world < optional-sql/mod_zone_scaling_chrono_npc.sql
-- then, in-game, at an inn:  .npc add 190000

-- 2. or attach it to an innkeeper you already have
UPDATE creature_template SET ScriptName = 'npc_zone_scaling_chrono'
 WHERE entry = <your innkeeper>;
```

The second is arguably better placement, since the toggle is only permitted in
a rested area anyway. It is not done for you because it overwrites whatever
`ScriptName` that creature had.

⚠ **Entry 190000 is a guess at a free id, not a reservation** — AzerothCore has
no registry for module creature entries. Check it against your own world
database before applying.

Every branch of the NPC delegates to the same functions `.chrono` uses, so the
two cannot reach different conclusions: a realm that changes the toggle rules
changes both at once. When the toggle is refused, the NPC shows the same reason
the command would have — "not while in combat" teaches a rule; "no" does not.

## Elites stay elite

Scaling flattens levels, and an elite scaled to the same level as the trash
around it loses the one signal that says *this one is different*.
`PreserveEliteOffset` raises elite, rare-elite and rare creatures above their
banded level by however far they stood above the band midpoint originally, up
to `.Max` levels.

The offset is measured against the band **midpoint** rather than its ceiling:
against the ceiling almost every creature reads as unremarkable and the offset
comes out zero exactly where it is wanted. It is also **only ever upward** — a
band whose maximum exceeds the `creature_classlevelstats` usable ceiling (see
below) would otherwise clamp the elite *below* its own trash, so at the top of
such a band the offset is simply suppressed and the elite presents at the
banded level like everything else.

## Full scaling, or level-up only

`DefaultBehaviour` decides *whether* a zone scales. `Direction` decides what
scaling means once it does, and it is the setting that most changes how the
realm feels:

| `Direction` | A creature below you | A creature above you |
|---|---|---|
| `both` | raised to your level | lowered to your level |
| `up` | raised to your level | **left exactly as it is** |

`both` is retail-Legion behaviour: the whole world stays relevant and the whole
world stays survivable. `up` is the conservative choice — outlevelled zones
still give XP and usable quests, but nothing that was meant to be dangerous
stops being dangerous, and walking into something too big for you still means
something.

The band still applies in both modes. Its maximum caps how far a creature is
raised; under `up` it simply loses the power to *lower* a creature that was
already past it. Its minimum is likewise unaffected — in a 10-60 zone a level-3
critter presents at 10 even to a level-5 player, under either direction,
because that is the floor doing its documented job.

### A little below you, not exactly at you

`TargetOffset` shifts the whole thing down by a fixed number of levels. At `3`,
a creature that would have presented at your level presents three below it
instead: green rather than yellow, still worth XP and still questable, but a
worse place to farm than content at your own level — the behaviour Project
Ascension shipped for its outlevelled zones. The elite offset stacks on top, so
a rare elite in that zone presents at or just above your level while the trash
beside it sits under you, which is the signal `PreserveEliteOffset` exists to
keep.

The offset is applied to your level before the band clamp and before the
direction rule, and it is capped at 10: past the grey threshold a scaled
creature pays no XP, which defeats the purpose. Damage, hit tables, the
nameplate and the reward corrections all read the same offset number, so
nothing has to be tuned twice.

## What a creature's own level is

Everything above is measured relative to a creature's **canonical level** — its
own level, before any player is considered. `ZoneScaling.CanonicalLevel` decides
where that comes from:

- **`native`** (default) — whatever the core rolled from the creature's
  template. The world keeps the shape it was designed with, and this module is
  purely a lens over it.
- **`midpoint`** — every eligible creature in a banded zone is placed at the
  midpoint of that band. In a 10-60 zone everything becomes level 35.

`midpoint` narrows how far anything ever has to be scaled in either direction,
which keeps the arithmetic better behaved at the extremes. The cost is that it
**rewrites the actual world for everyone**, not just the scaled view — this is
the one setting in the module that changes state rather than presentation.

> ⚠️ Consider `midpoint` and `Direction = up` together before enabling both.
> Under `up` nothing is ever lowered, so raising every creature to a band
> midpoint is a large and permanent difficulty increase for anyone below it.

Excluded creatures are never moved, and no creature is moved onto a level with
no usable `creature_classlevelstats` data — that would produce a 1 HP mob, so
the override is refused and the native roll stands.

Canonical level is tracked per creature and refreshed whenever the core
reselects it. That refresh is not optional bookkeeping: `Creature::Respawn`
calls `SelectLevel()` on a creature that never left the world, and `SelectLevel`
re-rolls `urand(minlevel, maxlevel)`. A creature with a level range therefore
changes level on every respawn without any add-to-world hook firing, and a cache
built only on entry would go quietly stale — taking every ratio computed from it
along with it.

## How a fight is actually normalized

The creature's real numbers never move. It keeps one health pool and one stat
block, shared by everyone fighting it. What changes is the damage crossing
between it and each player:

| Direction | Multiplier | Curve |
|---|---|---|
| creature → player | `damage(effective) / damage(canonical)` | damage |
| player → creature | `health(canonical) / health(effective)` | health |

**The two curves are different on purpose.** Incoming damage should feel like an
effective-level creature's swing, which is the damage curve. Outgoing damage is
judged by how much of the health bar it removes — the real pool is
`health(canonical)`, the pool the player should perceive is `health(effective)`
— which is the health curve, inverted. Health and damage do not grow together in
`creature_classlevelstats`, so one ratio used both ways is wrong in one
direction.

Heals are deliberately **not** scaled. A creature's self-heal is denominated in
the same canonical HP as its real pool, so the fraction of the bar it restores is
already correct for every observer; scaling it would introduce an error rather
than remove one.

Pets, guardians, totems and charmed creatures resolve to their owner, so they
fight at the owner's ratio — neither exempt nor double-scaled.

### The hit table matters as much as the damage

Melee skill in this core is `level * 5`, taken from the creature's **canonical**
level. Without `ScaleHitChance`, a level-8 creature presenting at 60 swings with
skill 40 into a player's defence 300: it misses most of the time, and however
large its damage multiplier is, that damage rarely lands. Scaling the damage
number alone does not produce a fair fight.

`ScaleHitChance` corrects miss, dodge, parry and block. Glancing and crushing
blows need one small core change on top — see the next section.

### Glancing and crushing blows: the optional core patch

The core decides *whether* a glancing or crushing blow may happen with a raw
level comparison, and only then works out *how likely* it is from the four
skill values the hook lets a module correct. So with the module alone the odds
are right but the gate is wrong: a level-60 player fighting a level-8 creature
presented at 63 (an elite with the offset) should glance a quarter of the time
and never does, because the gate still sees level 8. Under `Direction = both`
the mismatch runs the other way and a lowered creature hands out glancing
blows that should not exist.

`patches/azerothcore-melee-outcome-gates.patch` rewrites those two gates in
terms of the values the hook already receives. `GetMaxSkillValueForLevel` is
defined as `getLevelForTarget * 5`, so with no module loaded the patch changes
nothing at all — the world-boss rule included — and with this module loaded the
gates follow the effective level like everything else. It is two expressions
in `Unit::RollMeleeOutcomeAgainst`:

```bash
cd azerothcore-wotlk
git apply modules/mod-zone-scaling/patches/azerothcore-melee-outcome-gates.patch
```

It applies cleanly to upstream `master` and to the mod-playerbots fork. The
patch is optional; the module builds and runs without it and simply keeps the
limitation described below. What the patch does not touch is the glancing
*damage* reduction, which the core still computes from the raw level gap: with
the gate fixed that is at worst a 30% reduction where the offset would have
asked for 10-30%, a rounding matter rather than a fairness one.

`.zs ratio` shows the exact multipliers for any pairing, without a fight or even
a creature in the zone.

## What each player sees

The nameplate, the tooltip and the con colour follow the same effective level
the fight does. Two players looking at one creature at the same moment see two
different numbers, and neither is being told a rounded-off approximation of the
other's — each is sent the level that creature actually presents at *for them*.

This works because the core assembles `SMSG_UPDATE_OBJECT` per receiver: it
builds one field buffer, caches it per visibility class, and then hands the
assembled bytes to a per-target patch pass. The module marks where the level
field landed and rewrites those four bytes for each observer. A stock client
needs no addon and no patch; it is simply told a different number.

One consequence is easy to miss and looks like a bug when you meet it: a
creature only re-sends its level when something marks that field dirty, and a
player levelling up changes nothing about the creature. Every mob already on
screen would keep the number it was sent when it came into view. The module
therefore refreshes nearby creatures on level-up, so the nameplates follow
within a tick rather than at the next time you walk away and back.

Turning `ScaleDamage` on and `ScaleLevelDisplay` off is legal but rarely what
anyone wants: the nameplate then says "Level 8" for something swinging like a
60, and the con colour tells the player it is safe.

## Experience follows the fight

Scaling a creature up without scaling what it pays is worse than not scaling it
at all: the zone gets harder *and* stays worthless. So the module corrects
experience to match the level content was actually fought at. Kills, quests and
the Chrono bonus each have their own switch; all three ride
`ZoneScaling.Xp.Enable`.

**Kills** are corrected by a ratio. `Acore::XP::Gain` has exactly one term that
depends on the victim's level, so scaling the award by

```
BaseGain(playerLevel, effectiveLevel) / BaseGain(playerLevel, canonicalLevel)
```

lands on the right number while preserving the elite doubling,
`ModExperience`, `Rate.XP.Kill`, the group share, the damage-requirement taper
and any XP-percent aura — without the module needing to know that any of them
exist.

**Grey kills are the case that matters, and the hard one.** Stock AzerothCore
pays nothing for a creature at or below your grey level. At level 40 that is
everything below 31, so a scaled starting zone would be a fight for no reward.
Worse, the core never even offers the award to a module in that case: it skips
the hook entirely when the amount is zero. Those kills are therefore paid as a
top-up after the fact, which means reproducing part of the core's reward
pipeline. That is why `ZoneScaling.Xp.GreyKills` is a separate switch — it is
the one piece of this module that can be broken by an unrelated core change,
and it can be turned off on its own.

**Quests** are priced at the level their zone presents at. A quest whose level
is `-1` already tracks the player and is left alone; so is one whose
`ZoneOrSort` is a class or profession bucket rather than a zone.

**Expect large numbers.** This is by design and is worth seeing before you
enable it. "Bounty on Garrick Padfoot" is a level-5 quest paying 35 XP to a
level-40 player; in a scaled Elwynn it pays 3,446, because that is what a
level-40 quest of its difficulty pays. Run `.zs questxp <id> <level>` against
your own bands before turning this loose — it prints the before, the after and
the multiplier. If the result is too generous, narrow the band; do not leave
the correction off, because then the zone is only harder.

**Quest gold is not touched and does not need to be.** AzerothCore already
looks the reward up by the player's level rather than the quest's.

## Loot follows the fight too

Coin comes from `creature_template.mingold/maxgold` and is fixed per entry —
`Loot::generateMoneyLoot` never looks at a level. So without correction a Defias
thug scaled from 8 to 40 still drops a level-8 thug's copper.

**The coin curve is not invented here.** Unlike experience there is no core
*formula* to take a ratio of; coin is data. But there is core *data*:
`quest_money_reward` is Blizzard's own per-level statement of what a copper
reward is worth, it is already loaded, and it is complete and non-decreasing for
levels 1–80 on a stock world DB. The multiplier is the ratio of that curve at
the two levels, and this module ships no coin constant of its own.

**It overshoots at the top, and that is stated rather than fudged.** Measured
against the mean coin of non-elite fixed-level creatures in the shipped
`creature_template`:

| levels | creature data | `quest_money_reward` |
|---|---|---|
| 10 → 20 | 3.9× | 3.6× |
| 10 → 40 | 20.7× | 14.0× |
| 10 → 60 | 22.8× | 28.0× |
| 10 → 80 | 116.0× | 232.0× |

Close through the classic range, roughly twice as generous at 80, because the
WotLK end of the table inflates harder than creature coin does. Run
`.zs coin <from> <to>` against your own bands before switching this on; cap it
with `Loot.MoneyMax` if the honest number is too rich for your realm. A fitted
fudge factor would be a number nobody could check.

**Solo looting is exact. Grouped looting is exact in the money and not in the
message.** `Loot::gold` is one number on the corpse, the before-loot hook fires
once for whoever clicked, and the group branch sends the same `uint32` to every
member with no hook between the split and the packet. So the module corrects
each member's *credit* individually as `ModifyMoney` reaches them — everyone's
balance moves by their own level-appropriate amount — but the "Your share is…"
line shows the group average rather than each member's own number. Closing that
needs an edit to `LootHandler.cpp`, and this module does not edit the core.

**Gear banding is off by default, deliberately.** With `Loot.Gear = 1`, an
eligible equipment drop from a scaled creature is replaced by an item of the
same class, subclass, inventory type and quality at the effective level or up to
`Loot.Gear.Tolerance` levels below. Replacements are never above the effective
level. If that slot, subclass and quality have no eligible item in the allowed
range, the original drop is left unchanged. The index is built over
`item_template` - 10,430 items survive the eligibility filter on a stock world
DB. No new tables; nothing is written to your world DB.

It ships off because it is a much larger claim than coin: it rewrites *which
items the world drops*. A realm that switches it on stops being one where a
particular mob drops a particular sword. Coin is arithmetic; this is content.
Never substituted: quest drops, quest starters, items with loot conditions,
free-for-all items, and anything below uncommon. Never used as a replacement:
uniques, heirlooms, and profession, reputation or PvP rewards. `.zs gear` shows
what it would do to any item at any level.

## Zone bands

One row per zone in `mod_zone_scaling_zone`. A creature in that zone presents
to a player at `clamp(playerLevel, minLevel, maxLevel)`.

```sql
INSERT INTO mod_zone_scaling_zone (zoneId, minLevel, maxLevel, enabled, comment)
    VALUES (40, 10, 60, 1, 'Westfall');
```

Below the floor the mob stays at the floor and remains dangerous. Above the
ceiling it caps, so a zone can still be outgrown.

> ⚠️ **`maxLevel` has a real ceiling, and it is lower than 80.** Creature stats
> come from `creature_classlevelstats`, whose rows run to level 100 but whose
> *usable* data stops between 73 and 85 depending on `unit_class` **and**
> `expansion` — the rows above are placeholders of 1 HP and no damage. On a
> stock world DB the binding limit is **level 73** (class 4, expansion 1).
>
> The module refuses to scale a creature onto an unusable level rather than
> producing a 1 HP mob, and logs the real ceiling for your database at startup.
> Check that log before choosing band maxima; raising them further means
> extending `creature_classlevelstats` yourself.

A row with `enabled = 0` means *specifically not this zone*. It overrides
`DefaultBehaviour = clamp` rather than falling through to it, so you can opt
the whole world in and carve out exceptions.

## Which creatures are skipped

Some are skipped by rule, with no configuration: world bosses, critters,
totems, trigger creatures, anything a player cannot attack (`NON_ATTACKABLE`
or `IMMUNE_TO_PC` — spirit healers and most friendly scenery), quest givers,
vendors, trainers, and anything flagged to grant no XP. Add further entries to
`mod_zone_scaling_exclude`.

`.zs excluded <entry>` reports which rule caught a given creature, so you never
have to guess why something did not scale.

## Commands

| Command | Level | Does |
|---|---|---|
| `.zs status` | GM | module state: enabled, tables loaded, band and exclusion counts |
| `.zs zone [id]` | GM | the resolved band for a zone, defaulting to yours |
| `.zs level <zone> <playerLevel> [canonicalLevel]` | GM | what a creature of that level would present at — the direction rule, made falsifiable without playing |
| `.zs ratio <entry> <zone> <playerLevel> [canonicalLevel]` | GM | the exact combat multipliers for a hypothetical pairing, and the health pool the player would perceive |
| `.zs questxp <questId> <playerLevel>` | GM | what a quest would pay before and after correction, or which rule declined it |
| `.zs coin <canonicalLevel> <effectiveLevel>` | GM | the coin multiplier between two levels, with worked examples — see it before switching coin scaling on |
| `.zs gear <itemId> <effectiveLevel>` | GM | what an item would be replaced with at that level, or which rule declined it |
| `.zs aggro <entry> <zone> <playerLevel> [canonicalLevel]` | GM | the aggro radius three ways — stock, normalized and ideal — plus the residual, so the trade can be measured on your own bands before it is taken |
| `.zs info` | GM | everything the module is tracking about your selected creature — or, if it is tracking nothing, exactly which test it failed |
| `.zs sweep <mapId> [zone]` | GM | walks every spawn on a map and reports what would scale, with a PASS/FAIL verdict — the way to check a band without flying it |
| `.zs excluded [entry]` | GM | whether a creature scales, and if not, which rule stopped it |
| `.zs stat [entry] [min] [max]` | GM | what a creature's HP/mana/melee/AP would be at each level in a range |
| `.zs reload` | Admin | re-read both tables without restarting |
| `.chrono [on\|off]` | Player | read or set your own Chrono Mode flag |

All work from the server console too, except that `.zs zone` needs an explicit
zone id there, `.zs excluded` / `.zs stat` need an explicit entry, and `.zs info`
is in-game only because it has no argument form — it reads your selection and
there is no selection on a console. `.zs sweep` costs a synchronous pass over
every spawn on the map on the world thread, and takes one map at a time
because resolving a spawn's zone instantiates that map — run it deliberately,
not in a loop. `.zs level`
is pure arithmetic against loaded config — it touches no creature and the zone
need not contain one, so any hypothetical can be asked.

`.zs stat` takes its arguments positionally, and **if any argument is given the
first is always the entry** — so `.zs stat` alone reads your selection, while
any explicit form starts with an entry. With a creature selected it also prints
that creature's live stats, which should equal the computed row at its own
level; a mismatch means the module has drifted from the core.

## Limitations

Stated up front, because some of them are structural on 3.3.5 and no amount of
module work removes them:

- **Item stats are fixed in `item_template`.** There is no per-drop item-level
  scaling without cloning items into the database. Loot scaling is limited to
  coin amounts and substituting whole items of the right level.
- **A group's loot coin message shows the average, not each member's share.**
  The money is right; the chat line is not. AzerothCore computes one
  `goldPerPlayer` and sends the same number to every member, with no hook
  between the split and `SMSG_LOOT_MONEY_NOTIFY`. Solo looting is exact in both.
- **Gear bands to the round-robin looter, not to each looter.** Loot is rolled
  once per corpse, at the moment of death, for one player. Per-looter gear is
  not expressible on AzerothCore's loot model at all — it would need the loot
  itself to be per-observer, which is a core change, not a module one.
- **Some drops are never rebanded, by design.** Anything carrying a flag the
  core has already used to build its loot bookkeeping — free-for-all drops,
  items following custom loot rules, faction-restricted items, profession
  recipes, quest starters, and anything disabled for loot — is left exactly as
  it dropped, and so is anything the replacement pool cannot match neutrally.
  `.zs gear <itemId> <level>` names the reason for any particular item.
- **The coin curve is borrowed from quests and is about twice as generous as
  creature data past level 70.** It is the only per-level money curve the core
  ships, it is honest about being one, and `Loot.MoneyMax` caps it. See the
  table above.
- **Combat log numbers differ between players** fighting the same mob. This is
  inherent to per-player normalization; retail scaling behaves the same way.
- **Health bars are percentages**, so views stay coherent, but a mob's absolute
  HP is a single canonical number, not a per-player one.
- **Playerbots evaluate threat against real unit fields**, which are canonical
  rather than per-bot. The interaction is not yet characterised. Bots are
  ordinary players as far as Chrono Mode is concerned, so they take
  `Chrono.Default`; they do not inherit their owner's flag, because doing so
  would mean depending on the playerbots API.
- **Two things do not follow the Chrono flag**, because they are not
  per-player and cannot be made so. `AggroRange = normalize` writes a detection
  distance onto the creature, and `CanonicalLevel = midpoint` changes the
  creature's real level at spawn. An opted-out player still meets both. Neither
  changes a fight's difficulty the way damage does, but "opted out means
  nothing about you is scaled" would be a slightly false promise.
- **Glancing and crushing blows are gated on the real level unless the core
  patch is applied.** The core decides whether either outcome may happen from
  a raw level comparison made after the melee-outcome hook. Without
  `patches/azerothcore-melee-outcome-gates.patch`, an elite presented above
  the player never glances and a creature lowered under `Direction = both`
  glances when it should not. With `Direction = up` and no elite offset the
  gates happen to agree with stock, so the loss is confined to elites. The
  probabilities themselves are already correct either way. See "Glancing and
  crushing blows" above.
- **Only melee hit chance is corrected — unless the Z-14 core patch is
  applied.** With `patches/zs_per_viewer_level_apply.py` (or the `.patch`)
  on the core and `CoreView = 1`, `Creature::getLevelForTarget` and armor
  mitigation ask this module for the viewer's version, and spell hit and
  resistance, weapon and defence skill, the glancing and crushing gates,
  stealth detection, aggro radius, kill XP, reputation and the "grants XP or
  honor" gate all follow the effective level from that one definition. The
  melee-outcome-gates patch above is then redundant (harmless). Without the
  patch, spell hit and resistance still use canonical levels.
- **The grey-kill top-up reimplements core logic.** It has to: when the stock
  award is zero the core never calls the hook a module would use to change it,
  and there is no hook anywhere in `Unit::Kill` before the reward runs, so a
  module cannot even record which victim is about to be priced. The top-up
  reproduces the eligibility rules, the group share and the static multipliers
  from `KillRewarder`. It is correct against the current core and it is the
  first thing to suspect after a core upgrade. `.zs status` counts corrected
  and topped-up kills separately for that reason.
- **In a mixed-level group the kill ratio uses your level, not the group's
  reference member.** The core prices a group kill once, from the highest
  member the victim is not grey to, and shares it out; the ratio here answers
  "what is this creature worth to *me* at the level I am fighting it". Both are
  defensible and the difference is small next to the correction itself, but
  they are not identical.
- **Battleground kills are not topped up.** Battlegrounds are off by default
  and run their own rate branch, which this module does not reproduce.
- **The quest correction is only lightly exercised.** It has fired in the wild
  exactly once, correctly. The arithmetic and all six guards are exercised
  through `.zs questxp`, which calls the same function the hook calls, and the
  two agreed on that one live case — but one sample is one sample.
- **The elite offset disappears at the top of a wide band.** It cannot push a
  creature past the `creature_classlevelstats` usable ceiling, so in a band
  whose maximum already reaches or exceeds that ceiling — a 1-80 band on a
  stock world DB, where the ceiling is 73 — elites present at exactly the same
  level as the trash beside them. Narrower bands, or an extended
  `creature_classlevelstats`, are the only fixes.
- **Only the level, health and mana fields are shown per observer.** Damage
  and everything else in the unit's fields remain canonical for every viewer;
  the scaling is a relabelling plus a damage multiplier, not a per-player
  creature. (`ScalePoolDisplay` adds the bars, Z-14.)
- **Aggro radius cannot be made per-player.** The core computes it as
  `detectionRange - (playerLevel - creatureLevel)` and exposes no hook on
  `Creature::GetAggroRange`, `GetAttackDistance` or `CanStartAttack`, while
  `getLevelForTarget` cannot be overridden from a module. `AggroRange =
  normalize` removes the *creature's* level from the answer, so every creature in
  a zone pulls a given player at the same distance instead of a level-8 boar
  reaching 5 yards while the level-25 ogre beside it reaches 22. It cannot remove
  the *player's* level; the residual is `|effective level - band midpoint|`
  yards, the same for every creature in the zone and zero at the midpoint. Run
  `.zs aggro` to see the size of the gap on your own bands. **The Z-14 core
  patch is that hook**: with it the radius follows each viewer exactly, and
  `AggroRange` should be left at `stock`.

## Credits

The per-viewer level and armor resolvers in the core (`CreatureView.h`,
`patches/zs_per_viewer_level_apply.py`, `src/ZoneScalingView.cpp`) and the
per-observer health and mana bars (`ScalePoolDisplay`) are adopted from
[jealous-sound/azerothcore-wotlk-coa#4406](https://github.com/jealous-sound/azerothcore-wotlk-coa/pull/4406),
"feat(scaling): Level scaling and the Destiny Weavers" by **bozo-1**
(commit `1c1235abc`, 2026-09-20). That PR's `LocalLevelScaling.h` resolver
slots and its `destiny_weaver_scaling.cpp` are the design; this module
reimplements them over its own effective-level and stat tables. Its realm-wide
lift, quest keep-shares, group-leader override and Weaver NPCs are not carried.

## License

AGPL-3.0, matching AzerothCore. See `LICENSE`.
