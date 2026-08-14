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
   unknown edge, incomplete item location expansion, or capture error prevented a safe result.

A valid stable report contains the canonical SHA256 `digest`, practical target counts, all exact zero predicates,
the protected baseline, source file revisions and hashes, schema hashes, the inventory hash, and read only access
attribution. Large row surfaces retain an exact row count and canonical identity digest. Explicit target and
derived actor identities remain in the manifest. A separate protected sweep covers every declared database edge,
Medivh identity, cache value, Redis stream entry, and pending entry for account 157 and GUID 661. Any row shared by
the target and protected sweeps is an exact refusal surface.

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

## Verification

Run the standalone checks from this repository root.

```bash
python3 -m unittest tests/python/test_check_repository.py
python3 -m unittest tests/python/test_population_manifest.py tests/python/test_population_manifest_live.py tests/python/test_population_manifest_projections.py
python3 tools/check_repository.py
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

The manual AzerothCore integration workflow builds the module with its public dependencies.
