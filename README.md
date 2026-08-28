> **Work in progress**

This project is not ready for installation or use. It provides no deployment or compatibility guarantee.

# Playerbots Economy

`mod-playerbots-economy` owns Playerbot careers, profession capability, gathering claims, market state,
economic policy, execution, persistence, and telemetry.

## Dependencies

The module requires `mod-playerbots-personality`, the Playerbot compatible AzerothCore repository, and the
generic extension seams from `Fuitad/mod-playerbots-upstream`.

## Configuration

Copy `conf/mod_playerbots_economy.conf.dist` to the server module configuration directory. Economy settings
are owned here and are not read from `playerbots.conf`.

## Database

The module stores its state in the Playerbots database. Its migrations are under
`data/sql/db_playerbot/updates`. The Playerbots module loads SQL contributed by enabled modules through its
database updater seam.

A profession progression that lacks a reagent records a material source path. The path is either
`same_actor_gathering` (the bot gathers the reagent with herbalism, mining or skinning) or
`same_actor_hunting` (the reagent is a creature drop, so the bot kills and loots creatures at or below
its level whose ordinary loot table carries it). Hunting paths carry gathering skill id 0.

## Managed supplies

When `AiPlayerbot.EconomyManagedSupplies` is enabled, the economy stocks ammunition, food, drink, health and
mana potions, class reagents, and bags through ordinary acquisition. An affordable Auction House listing is
preferred. If no legal affordable listing exists, the bot selects an ordinary unlimited gold vendor offer that
matches the need, travels to that vendor, and pays with its own gold. Vendor access applies the bot's faction,
level, reputation, item usability, map, and travel constraints.

The vendor budget always preserves the full current gear repair reserve. Class reagent quantities follow the
same class and level bands used by Playerbot initialization. Empty bag slots create demand when a usable bag is
affordable. An equipped bag also creates upgrade demand when an affordable replacement has at least four more
slots. A larger bag can satisfy a smaller capacity need.

Food, drink, and restoration potions remain stocked while the matching strategies are active. The economy only
uses one when health or mana is below that strategy's configured threshold. This leaves full health and full mana
bots stocked without consuming supplies on every economy cycle.

Managed supply consumption runs for every lifecycle safe random bot. The safety gate requires the lifecycle to be
enabled and the bot to be alive, outside combat and battlegrounds, free of a real player master, and not teleporting.
Career capability is a separate gate. A bot without eligible profession work still collects auction mail, buys and
uses finished goods from the Auction House or ordinary vendors, recovers obsolete purchases, and lists items it
cannot use. Unusable armor and weapons below uncommon quality are never listed, because no bot buys equipment
below uncommon, so such a listing could only expire and burn its deposit. The vendor visitor sells them instead. Trainer work, profession progression, crafting, recipe and reagent purchases, production assignments,
gathering, capability goals, and market making remain disabled until the career gate passes.

An idle consumer cycle reports `career_ineligible` when the career capability gate is closed and
`consumption_idle` when it is open. Both states use the ordinary cycle interval. An unmet need with no eligible
offer reports `no_finished_good_offer` separately.

## Coordinator performance and threading

`EconomyCycleAction` can run on many map workers, but every cycle shares one economy coordinator. An actor or
market refresh whose authoritative value facts are unchanged is a no op only when no time driven expiry or
retention mutation is due. That no op must not advance the coordinator generation or trigger whole fleet gap and
chain recomputation. A changed value must remain visible to the next snapshot and chain observation.

Gap totals and consumer identities are cached until an actor, market, or claim mutation invalidates them. Chain
synchronization runs once for that mutation state. Capability observations discovered by one bot cycle are
submitted as one batch so the cycle acquires the coordinator mutex once rather than once per demand gap.

Global aggregation is protected by the coordinator mutex. The deterministic 11 gap regression requires one domain
operation lock acquisition, one gap rebuild, and one chain synchronization when claim expiry dirties the batch.
Counter reads are excluded from the lock total so before and after samples measure only the operation. This is a
work amplification bound, not live wait duration proof. Player, Map, inventory, mail, and Auction House reads and
mutations stay on their authoritative AzerothCore threads. Do not move those objects to a background worker.

If a future implementation adds a worker queue, the queue may consume only immutable value snapshots captured on
the authoritative thread. It must have bounded capacity and bounded backpressure. Its telemetry must expose input
freshness, queue age, queue depth, rejected work, and overflow so stale or discarded economy state remains visible.

The focused coordinator regressions cover a 200 actor fleet, an 11 gap capability batch, bridge reconciliation,
idempotent outcomes, production renewal, claim expiry, lease admission, and speculation release. They prove that
equivalent actor and market facts cause no gap or chain rebuild, that changed facts remain immediate, that one
capability batch advances every gap under one lock scope, and that the covered outcome, invalidation, renewal,
expiry, and lease paths change generation only when their observable state changes.

```bash
/Users/pierre/Workspace/azerothcore-wotlk/build/src/test/unit_tests \
  --gtest_filter='PlayerbotEconomyCoordinatorTest.*'
```

Before accepting a live deployment, run a supervised profile with exactly 200 active bots and no activity lease
changes during the capture. Confirm the MCP status and complete roster both report 200 active bots before and after
the sample. Capture 30 seconds from the deployed process.

```bash
PID="$(pgrep -x worldserver)"
PROFILE=/tmp/playerbots-economy-200-bot.sample.txt
/usr/bin/sample "$PID" 30 -file "$PROFILE"
/opt/homebrew/bin/rg -n \
  -e 'SyncChainsLocked|CalculateGapsLocked|PlayerbotEconomyCoordinator.*mutex' \
  "$PROFILE"
```

Accept the profile only when the roster remains at 200, the MCP queue remains available with depth zero, and the
search shows no coordinator aggregation or mutex wait as a repeating map worker stack. During the same supervised
window, fresh world update timing must remain within the server's preserved known good baseline. Preserve the
sample, timing window, roster evidence, process identity, source revisions, and binary hash with the report.

## Managed equipment enhancements

When `AiPlayerbot.EconomyManagedSupplies` is enabled, random bots acquire usable enchant scrolls, weapon oils,
sharpening stones, glyphs, and gems through the ordinary Auction House path. The final use step consumes the real
item through the same item use and socket packets sent by a game client.

Weapon oils and sharpening stones target the main hand weapon. Permanent enchant scrolls target equipped gear
allowed by the scroll spell. An enhancement is applied only when the relevant permanent or temporary enchantment
slot is empty, or when the new enchantment has a higher stat weight than the existing one.

Glyph demand follows the configured premade glyph list for the bot's class and active specialization. Only empty
glyph slots unlocked at the bot's level generate demand. The glyph item is used with the matching glyph slot index,
so the core validates the major or minor slot class and applies the glyph normally.

Gem demand comes from empty sockets on equipped gear. Candidate gems must match the socket colour. When several
listed gems fit, the economy prefers the gem with the highest stat weight for the bot before comparing price. The
selected gem is installed through the core socket handler, and an occupied socket is never replaced automatically.

## Riding ranks

A random bot buys the riding rank its level entitles it to, from a Mount trainer, with its own gold.
The level bands and the skill each one requires are the ones `mod-playerbots` already uses for mounts
(`AiPlayerbot.UseGroundMountAtMinLevel` and its three siblings, read through `RequiredMountTier` and
`RequiredRidingSkill`), so there is one set of tier rules and not two.

Riding reuses the whole trainer objective machinery: a `Riding` objective, a travel destination, an
affordability test and a lesson selection. Two things are different.

The travel catalog carries Mount trainers alongside Tradeskill trainers, in one map, and each
destination records which type it is. A riding objective is only ever offered a Mount trainer and a
profession objective only ever a Tradeskill trainer, so the pools cannot mix. This is what the
maintenance mount action in `mod-playerbots` cannot do: it searches spawns on the bot's current map
only, so a bot whose racial riding trainer sits on another continent never reaches one. The economy
routes through `TravelNodeMap`, so it does.

The rank purchase is confirmed by the riding skill CAP rising, never by the bot holding the riding
skill. A bot buying Journeyman riding already held riding when it walked in, so a skill test would
report success before any gold changed hands.

Riding does not wait behind the career capability gate, because it is not profession work: a bot with
no eligible career still runs errands and would otherwise walk every one of them.

It outranks profession trainer work, with two yields, both of which exist to keep a bot arriving
somewhere. Riding never takes the stage from a trainer trip already under way, because cancelling one
restarts it and a riding gap that nothing can close would restart it on every cycle. And when no mount
trainer can serve the bot at all, the profession objective takes the stage for that cycle instead.
`PlayerbotEconomyPolicy::ChooseTrainerStageObjective` is the whole rule and is covered by
`PlayerbotEconomyPolicyTest.RidingNeverCancelsATrainerTripAlreadyInFlight`.

"In flight" means the travel target this runtime owns, not the trainer destination it selected.
Those are different facts. Travel declines on any cycle where another system holds the forced travel
target, and the selection outlives the decline, so reading the selection as liveness would keep the
stage serving an objective the career plan had already reassigned with no cycle that ever re-selects.
`PlayerbotEconomyPolicy::TrainerTripInFlight` is that distinction.

A trip in flight is also bounded. Upstream keeps a forced travel target alive indefinitely while it
reports travelling: `TravelTarget::isActive` short circuits its expiry check for a forced travelling
target, and `TravelTarget::isTraveling` skips the fall back to cooldown when a forced target's
destination becomes invalid. A gathering trip whose node despawns or is taken mid route therefore
never ends on its own. Because `ReleaseIdleCycleState` keeps a trip that is still under way, that
stranded target was never released, and while a forced target is held
`ChooseTravelTargetAction` never calls `getNewTarget`, so quest travel is not outranked, it is
skipped entirely. The observed result was bots parked in their starting zone with quest objectives
that had not advanced in hours.

`EvaluateEconomyTrip` is the whole rule. A trip stops counting as in flight when its destination
reports invalid, or when it overruns `max(estimated travel seconds x 3, 120s)`. The multiplier
absorbs detours, combat and resting on the way; the floor keeps a short leg from being abandoned
just after it starts. Either outcome falls through to `Reset`, which already clears the forced
target, and the next cycle belongs to quest travel. An abandoned trip logs a warning naming the
destination, the elapsed time and the cause, because upstream reports the target as healthily
travelling right up to the moment it is dropped.

The bound covers walking trips only. A taxi flight (`activeEconomyFlight->taxiActive`) is still
treated as in flight for as long as the server keeps the bot on the taxi.

The rank spends from its own budget: free money for anything, minus what the profession and consumable
lanes want. Those two lanes are reserved by nothing else, so a one off durable purchase would otherwise
eat the reagents and food the bot needs on every later cycle. A bot that qualifies for a rank it cannot
yet afford reports `insufficient_protected_money` and keeps earning.

The trainer stage only runs for bots at level 6 or above
(`PlayerbotEconomyPolicy::ProfessionPipelineOpen`). Below that, nothing can succeed (Apprentice
teach spells require level 5 and the stay home rule confines a level 5 bot to its own zone), so the
stage selects nothing rather than scanning the realm and logging a refusal every cycle. Riding has
its own level band and does not wait behind this gate. A trainer refusal line carries the blocker
plus the bot's level, zone, purse and computed budget, and `no_route` (viable trainers exist, no
travel path reaches any) is reported separately from `unsafe_route` (a zone gate refused the
destination).

Profession lessons spend from the tradeskill lane with a small floor,
`PlayerbotEconomyPolicy::ProfessionTrainingBudget`. The bare lane holds back every standing reserve,
including a flat travel floor larger than a fresh bot's whole purse, so after a population wipe no bot
could ever afford a first lesson: the 2026-08-27 census found 6,011 `insufficient_protected_money`
refusals in one night against 208 bots holding 8 silver on average. Lessons are one off durable
purchases costing coppers at the early ranks, so up to 10 silver of the purse, minus the repair need,
is always spendable on them. A bot whose ordinary tradeskill lane already clears the floor keeps the
larger of the two, which leaves wealthy bots exactly as before.

Learning the rank is not the same as owning a mount. The mount item itself is bought and used by
`RandomBotMountAction` in `mod-playerbots`, which takes over as soon as the riding skill is high
enough for the tier. Nothing here buys a mount.

That action also trains riding, from the bot's one racial trainer, on the bot's current map. The two
paths overlap on purpose and neither is removed: the maintenance action is what put apprentice riding
on 157 of the 183 level 20 and above characters on the live realm, and the economy adds the reach it
does not have. Whichever arrives first wins cleanly. The rank appears, the other side stops wanting it,
and the economy releases its objective on the next cycle.

## Jewelcrafting trainers in the old-world capitals

Every Jewelcrafting trainer Blizzard shipped stands on map 530 (Silvermoon, Exodar, Shattrath and the
Outland outposts) or 571 (Dalaran). Blood elf and draenei bots reach theirs as same-map capitals, but an
old-world race that planned Jewelcrafting had no reachable trainer at all and re-booked a dead objective
every cycle, logging `unsafe_route` forever. The module therefore ships
`data/sql/db-world/updates/playerbot_economy_jc_capital_trainers.sql`, which adds a Jewelcrafting trainer
and a supplies vendor to each of the six old-world capitals (entries 980000 through 980011). Each pair
stands beside the city's enchanting trainer, clones the canonical Silvermoon or Exodar NPC (trainer list
113 and the standard supplies stock), and carries the city's own faction so cross-faction bots keep
refusing it. The worldserver's updater applies the file at startup; the spawns appear after a restart.

## Population aware career selection

A bot's professions are drawn from its own affinity weighted pool. Left alone that draw looks at one bot
at a time, so coverage is luck: on the live realm it produced 87 Mining and 76 Herbalism against 9
Jewelcrafting, and a gathering share of 53 percent rather than the 60 the economy wants.

Selection now sees the population. Three parts, and nothing else about the draw changes.

**The census.** `PlayerbotEconomyTelemetry` already receives every career plan the module resolves, keyed
by character, so it keeps the primary profession counts alongside them and hands out a snapshot through
`SnapshotProfessionCensus`. It is maintained incrementally as careers are published, so reading it costs
one small copy and never a database query. Its denominator is every bot whose career this process has
observed, not every bot that exists: a freshly started process sees a partial population and biases
toward whatever is scarce among the bots it has actually seen. A career already assigned is persisted and
is never reassigned, so a restart repopulates the census from the plans it loads rather than reshuffling
them.

**The decision.** `PlayerbotCareerPopulation::CandidateBiasPermille` takes the census, the targets and the
candidate's primary professions and returns a weight multiplier. It is a pure function of those facts and
needs no `Player`. Two terms, both of which only ever add weight:

1. A per profession floor. A profession below its configured minimum share owes a deficit, and the boost
   is proportional to how much of that deficit is still open, so it fades to nothing exactly at the floor
   and cannot overshoot past it.
2. A gathering share target. Whichever side of the gathering and crafting split is short gets a boost
   proportional to the size of the miss. The long side is left at its affinity weight, never penalised.

A career is scored by the mean of the professions it occupies, so a pair short on both sides outranks one
short on a single side and a mixed pair sits between them. Affinity stays the tiebreak: the bias rewrites
weights inside a pool that class legality, race legality, affinity gates and reachability have already
decided, so a high crafting affinity bot still prefers crafting, and no legal career can be weighted out
of its own pool.

**The pool.** `PlayerbotCareerSeeds::Build` assembles the candidate seeds from facts alone, the bot's
class, the primary professions it already learned, the slot limit and the configured class matching share,
so the distribution a population would produce can be measured without a running world.
`PlayerbotCareerPopulationTest.AFreshPopulationClearsTheJewelcraftingFloorAndReachesTheGatheringTarget`
does exactly that, simulating a fresh 200 bot population twice, once with the population terms disabled,
and reports both distributions.

Secondary skills (Cooking, First Aid, Fishing) are not primary professions and take no part in the share
or the floor. Nothing here spends money, so no `NeedMoneyFor` lane competes with it.

**The career provider.** `mod-playerbots-llm` registers a `PlayerbotCareerPlanProvider` and, when it
answers, its choice is the plan. A provider is told the candidate tokens, summaries, engagement and
spending styles, and nothing at all about the population, so a provider answer cannot honour a floor.
While any primary profession sits below its floor, the economy therefore does not consult the provider:
`PlayerbotCareerPopulation::PopulationNeedsCoverage` decides that, and `ResolvePlan` drops any request
already in flight rather than letting an answer land behind the bypass. Once every profession clears its
floor the provider decides again, exactly as it did before.

This bypass is a design decision taken alongside the population terms, not a reviewed preference, and it
changes the behaviour of an enabled paid integration: it silences the LLM career lane for as long as a
floor is unmet, which on a fresh population is most of the creation ramp. It has not been reviewed by the
repository owner. Note also that the floor and the bypass are one switch today: setting
`ProfessionFloorPermille` to 0 disables both, so keeping the floor while letting the provider decide would
need a separate key.

**What the census cannot see.** A career is published only once it resolves, so bots whose assignment is
still in flight are absent from the snapshot the next bot reads. On the weighted draw the whole sequence,
snapshot, select, publish, runs synchronously inside one `EnsurePersistentPlan` call, so the window is
only as wide as the map workers running concurrently. It is much wider on the provider path, which waits
on a network round trip, and that is the second reason the provider is bypassed while coverage is short:
population steering happens exactly where the window is narrow. The simulation assigns one bot at a time,
so it is more strongly serialised than a live creation wave and its distribution is an expectation rather
than a guarantee.

### Configuration

All four keys carry the `PlayerbotsEconomy.Careers.` prefix.

| Key | Default | Meaning |
|---|---|---|
| `GatheringSharePercent` | 60 | Share of primary slots that should be gathering |
| `ProfessionFloorPermille` | 40 | Minimum share of primary slots per profession. 0 disables the floor |
| `FloorBoostPercent` | 400 | Extra weight for a profession at zero against the floor. 0 disables it |
| `ShareBoostPercent` | 600 | Extra weight for the short side of the gathering share. 0 disables it |

All four are re-read by `ReloadPlayerbotEconomyConfig` on `OnAfterConfigLoad`, so `reload_config` applies
them without a restart. What they cannot do is respec a bot: a career plan is persisted once
(`PLAYERBOT_CAREER_PLAN_VERSION`) and a primary profession slot cannot be reclaimed, so a change here
reshapes careers assigned after the reload and leaves every existing bot as it is.

## Population manifest audit

`tools/population_manifest.py` is the supported read only population audit command. It can be run before a
cleanup, after a cleanup, or before a later population remediation. It discovers generated accounts from the
deployed account prefix and the authoritative Playerbots account type rows on every run. Those two authorities
must identify the same accounts. No configured or observed population count is accepted as authority.

Run the production audit from this repository root. The protected arguments are mandatory and this deployment
requires the exact Deszy binding shown here.

```bash
python3 tools/population_manifest.py live \
  --playerbots-config /Users/pierre/azeroth-server/etc/modules/playerbots.conf \
  --protected-account-id 157 \
  --protected-character-guid 661 \
  --protected-character-name Deszy \
  --medivh-root /Users/pierre/Workspace/medivh \
  --output /path/to/population-manifest.json
```

The command performs two ordinary MySQL transactions at `REPEATABLE READ`, declared `READ ONLY`, with consistent
snapshots. Redis
access uses only `GET`, `XRANGE`, `XINFO GROUPS`, and `XPENDING`. It writes no database or Redis state. The
optional output path preserves the same JSON report written to standard output.

The JSON `status` and process exit behavior are stable automation contracts.

1. `READY` exits 0. The two captures match and every required target surface is exactly zero.
2. `NOT_READY` exits 0. The two captures match, but at least one exact zero predicate is false. This is a valid
   diagnostic result, including before cleanup.
3. `UNSTABLE` exits 3. The captures differ. `changed_surfaces`, `first_digest`, and `second_digest` identify the
   refusal. No stable `digest` is emitted.
4. `REFUSED` exits 4. A protected identity mismatch, authority disagreement, schema drift, missing source,
   unknown edge, ambiguous cleanup ownership, incomplete item location expansion, protected owned mutation, or
   capture error prevented a safe result.

A valid stable report contains the canonical SHA256 `digest`, practical target counts, all exact zero predicates,
the protected baseline, source file revisions and hashes, schema hashes, the inventory hash, and read only access
attribution. Large row surfaces retain an exact row count and canonical identity digest. Explicit target and
derived actor identities remain in the manifest. A separate protected sweep covers every declared database edge,
Medivh identity, cache value, Redis stream entry, and pending entry for account 157 and GUID 661. Unclassified
surfaces remain fail closed: any row shared by the target and protected sweeps is an exact refusal surface.

Manifest format version 2 adds source backed cleanup roles for surfaces whose ownership is authoritative.
`reference_surfaces` reports target and protected counts and identity digests by `owned`, `provenance`, or
`participant` role. `shared_reference_surfaces` reports the exact intersections between those roles. These rows stay
visible even when they are not refusal surfaces.

The current classified interpretation is intentionally narrow. An `item_instance` row is deleted by `owner_guid`.
`creatorGuid` and `giftCreatorGuid` are provenance, so a bot owned item created or gifted by Deszy is reported but
does not claim that Deszy owns the row. A social relationship is directional and owned by `bot_actor_id`.
`subject_actor_id` is participation, so deleting a bot owned relationship toward Deszy does not delete a
Deszy owned relationship toward that bot. Ordinary cohort cleanup retains Social events. An explicitly authorized
population epoch transition may instead bind the frozen backup as the immutable archive, then remove the exact
frozen cohort event set from the live store. Participation in an archived legacy event is not row ownership.

For a classified surface, `protected_overlap_surfaces` contains only a row that the proposed target cleanup selects
as owned and the protected sweep also selects as owned. That condition remains an exact `REFUSED` result. A
provenance or participant intersection is diagnostic. It does not weaken the exact account 157, character GUID 661,
and `Deszy` negative controls, and it does not authorize any cleanup action.

Compare two preserved stable reports with the supported comparison command.

```bash
python3 tools/population_manifest.py compare \
  /path/to/earlier-population-manifest.json \
  /path/to/later-population-manifest.json
```

An identical comparison exits 0 with `comparison_status` set to `IDENTICAL`. A changed comparison exits 5 with
`comparison_status` set to `CHANGED` and exact `changed_surfaces`. Invalid, tampered, refused, or unstable inputs
exit 4 with `comparison_status` set to `REFUSED`.

The authoritative classification lives in `tools/population_manifest_inventory.json`. Every run validates its
source artifact hashes, exact table and column references, projection keys, and full schema hashes. A source or
schema change therefore refuses the audit until the changed surface is independently classified and the checked in
inventory is updated from current authoritative source and schema evidence. The deployed Medivh environment is
also checked against the classified Redis host, port, stream, and cursor identities on each run.

## Population backup rehearsal

`tools/population_backup.py` is the supported safety command for the stopped realm backup that must precede a
population cleanup. It has no cleanup path. It first records and verifies exact repository, artifact, launchd,
MySQL, Redis, schema, protected Deszy, and population identities. It then enables Medivh maintenance and stops
only the supplied launchd writer services through their exact plists.

The current writer boundary consists of worldserver, authserver, the Playerbots LLM sidecar, the Playerbots
collector, the Social collector, the Medivh telemetry consumer, and the Medivh scheduler. MySQL and Redis are
required running services. Both must be supplied as launchd label and absolute plist pairs, just like every
writer. The command refuses duplicate labels, overlap between stopped and required services, a plist mismatch,
an unexpected runtime identity, a protected state difference, an unknown audit edge, or a scoped MySQL event.

Inspect the exact arguments before an operation.

```bash
python3 tools/population_backup.py --help
```

Use the existing host backup root. Choose a new run name because the command refuses to overwrite a directory.
Pass each service as `LABEL=/absolute/path.plist`. Pass every deployed source repository with `--repository` and
every installed binary, configuration, plist, wrapper, audit source, and inventory file with `--artifact`.

After writer quiescence, the command requires two identical population snapshots. It captures all four
authoritative MySQL schemas in one `mysqldump --lock-all-tables` operation. This global lock is required because
the deployed schemas contain both InnoDB and MyISAM tables. It captures Redis through the RDB transfer command
while the same writers remain stopped. The dump disables GTID purging so it can be restored into the existing
server during a later rollback without attempting to replace the server's executed GTID state.

The rehearsal MySQL instance uses a new data directory, an operation local socket, `--no-defaults`, and
`--skip-networking`. The rehearsal Redis instance uses an operation local socket and port zero. The command
restores both backups there, reruns the full population audit, and requires the restored canonical digest,
surface counts, ownership, protected baseline, and direct offline Deszy tuple to match the frozen source.

Successful evidence includes the frozen and restored reports, exact comparison, MySQL dump, Redis RDB, service
and configuration identities, commands, file hashes, and operation record. Only rehearsal data directories are
removed. The evidence and backup remain, Medivh stays in maintenance, all writer services stay stopped, and
MySQL and Redis stay running. A failure restores every service that this command stopped and removes Medivh
maintenance before returning a refusal.

## Population scope: what this tooling does and does not do

This module owns OFFLINE population work only: audit, backup, and the cleanup executor, all under
`tools/population_*.py`. Two other things people come here looking for live elsewhere.

| Job | Where it actually lives |
|---|---|
| Audit, back up, or plan a population change offline | Here, `tools/population_*.py` |
| Wipe the cohort from a running server | **`mod-playerbots-lifecycle`**, config `PlayerbotsLifecycle.CleanupRequested` |
| Recreate the cohort afterwards | **`mod-playerbots`**, automatic, `RandomPlayerbotFactory::CreateRandomBots` |

Read `modules/mod-playerbots-lifecycle/README.md` before running a wipe. Nothing in this module is
required for one, and the words "wipe", "reset", and "recreate" appear nowhere in either module, which
is why searching for them finds nothing.

Recreation needs no tooling and never did. `CreateRandomBots` is called unconditionally from
`PlayerbotAIConfig.cpp` at startup, is idempotent (it skips existing accounts and accounts already at ten
characters), and rebuilds the population up to `AiPlayerbot.MinRandomBots` with `RandomPlayerbotFactionBalance`
keeping the factions even. The `admit-*.json` and `option-b-recreation` artifacts under
`~/azeroth-server/backups/2026-08-14*` came from a deterministic cohort builder that was never merged; it is
preserved as the tag `archive/option-b-recreation` (chain `f96e03d..d464f66`). Its config keys
(`PlayerbotsEconomy.FrozenPopulationEnabled`, `PopulationOperationMode`, `PopulationOperationCohort`,
`PopulationOperationGuard`) may still sit in a deployed `.conf`. They are dead: nothing reads them, and the
strings are absent from the built worldserver.

The cleanup executor here orders auctions and mail before `item_instance`, which goes before `characters`,
because `Player::DeleteFromDB` cleans guild membership and mail but never touches `auctionhouse`. The
in-server wipe has the same gap, and this module closes it through the extension hooks: `PrepareBotPurge`
refuses when a character outside the cohort holds a live bid on a doomed listing, and `OnBotPurge` deletes
the auction rows and any auctioned item rows once the accounts are gone. See
`src/Bot/Economy/PlayerbotEconomyPurge.h`.

One property that cannot be preserved across a recreation: `PlayerbotPersonalityMgr::Generate`
draws crafting affinity, gathering affinity, exploration affinity, sociability, voice, fictional
age, fictional country and roleplay affinity from `urand()`, which AzerothCore seeds from
`std::random_device` with no supported seed boundary. Only economy affinity is derived from the
character GUID. A recreated population therefore has a fresh, unreproducible affinity distribution,
so any before-and-after measurement that depends on affinity is not comparable across a wipe.

## Population cleanup executor

`tools/population_cleanup.py` plans and applies one exact frozen population cleanup. Plan mode is read only for
MySQL and Redis. It verifies the retained operation evidence, the authoritative MySQL backup and isolated restore,
the preserved audit runner, every recorded source and runtime artifact, the stopped writer fence, the running
stores, Medivh maintenance, and the protected Deszy identity before it creates a deterministic plan. Redis is a
disposable projection store. Its historical RDB and backup epoch are not cleanup authority.

The plan contains every frozen account, character, item, auction, mail, Social actor, and derived target identifier.
It also records the ordered table effects, expected row counts, storage engines, retained survivor owned Social
rows, Redis projection keys, immutable MySQL archive identity, validated Redis wipe location, and a canonical
SHA256 plan digest. The plan never records a Redis backup as authoritative recovery data.

```bash
python3 tools/population_cleanup.py plan \
  --operation-record /path/to/frozen-operation/operation-record.json \
  --audit-root /path/to/preserved-audit-checkout \
  --expected-frozen-digest FROZEN_SHA256 \
  --output-dir /new/evidence/directory
```

Apply mode accepts only an unchanged exact plan. It repeats the complete gate before mutation. Every MySQL effect
must still have its planned row count. The live Social event set is cleared only after the archive descriptor is
durably written. The Redis projections are then removed, and two post cleanup manifests plus literal exact target
queries must reconcile. Deszy account, character, owned reference rows, economic entitlements, and online and
deletion state must remain unchanged.

```bash
python3 tools/population_cleanup.py apply \
  --plan /path/to/cleanup-plan.json \
  --operation-record /path/to/frozen-operation/operation-record.json \
  --audit-root /path/to/preserved-audit-checkout \
  --expected-frozen-digest FROZEN_SHA256 \
  --output-dir /new/apply-evidence/directory
```

Any failure after mutation begins restores all four MySQL schemas from the verified MySQL backup. Redis recovery
stops only the retained local Redis service, removes its validated snapshot, restarts Redis, flushes all databases,
saves the empty state, and proves `PONG`, database size zero, and exact projection key absence. It never installs or
compares a Redis backup. Writers remain stopped and Medivh remains in maintenance after either successful cleanup
or recovery. The executor never recreates bots and never starts a writer service. Redis is the only service it may
restart.

## Verification

Run the standalone checks from this repository root.

```bash
python3 -m unittest tests/python/test_check_repository.py
python3 -m unittest discover -s tests/python
python3 tools/check_repository.py
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

The manual AzerothCore integration workflow builds the module with its public dependencies.
