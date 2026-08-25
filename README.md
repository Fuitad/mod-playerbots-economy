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
cannot use. Trainer work, profession progression, crafting, recipe and reagent purchases, production assignments,
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
