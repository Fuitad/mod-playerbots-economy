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

## Verification

Run the standalone checks from this repository root.

```bash
python3 -m unittest tests/python/test_check_repository.py
python3 tools/check_repository.py
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

The manual AzerothCore integration workflow builds the module with its public dependencies.
