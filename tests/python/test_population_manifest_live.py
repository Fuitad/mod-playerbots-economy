from __future__ import annotations

import copy
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

MODULE_PATH = (
    Path(__file__).resolve().parents[2] / "tools" / "population_manifest_live.py"
)
sys.path.insert(0, str(MODULE_PATH.parent))
SPEC = importlib.util.spec_from_file_location("population_manifest_live", MODULE_PATH)
assert SPEC and SPEC.loader
POPULATION_MANIFEST_LIVE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POPULATION_MANIFEST_LIVE)


def fixture_inventory() -> tuple[dict[str, object], list[dict[str, str]]]:
    schema_rows = [
        {
            "schema": "acore_auth",
            "table": "account",
            "column": "id",
            "column_type": "int unsigned",
            "nullable": "NO",
            "key": "PRI",
            "extra": "auto_increment",
        }
    ]
    inventory = {
        "version": 1,
        "schema_hashes": {
            "acore_auth": POPULATION_MANIFEST_LIVE._canonical_digest(schema_rows),
            "acore_characters": POPULATION_MANIFEST_LIVE._canonical_digest([]),
            "acore_playerbots": POPULATION_MANIFEST_LIVE._canonical_digest([]),
            "medivh": POPULATION_MANIFEST_LIVE._canonical_digest([]),
        },
        "confirmed_edge_keys": ["auth.account.id"],
        "source_artifacts": {"auth_cleanup": "fake:account.cpp"},
        "edges": [
            {
                "key": "auth.account.id",
                "schema": "acore_auth",
                "table": "account",
                "identity_columns": ["id"],
                "selectors": [{"kind": "account", "columns": ["id"]}],
                "source": "auth_cleanup",
            }
        ],
    }
    return inventory, schema_rows


class PopulationManifestLiveTest(unittest.TestCase):
    def test_confirmed_edge_removal_is_reported_as_unknown(self) -> None:
        inventory, schema_rows = fixture_inventory()
        inventory["edges"] = []

        unknown, _ = POPULATION_MANIFEST_LIVE.validate_inventory(inventory, schema_rows)

        self.assertEqual(unknown, ["inventory.missing_edge.auth.account.id"])

    def test_schema_or_column_change_is_reported_as_unknown(self) -> None:
        inventory, schema_rows = fixture_inventory()
        changed = copy.deepcopy(schema_rows)
        changed[0]["column_type"] = "bigint unsigned"

        unknown, _ = POPULATION_MANIFEST_LIVE.validate_inventory(inventory, changed)

        self.assertEqual(unknown, ["schema.acore_auth"])

    def test_unconfirmed_edge_is_reported_as_unknown(self) -> None:
        inventory, schema_rows = fixture_inventory()
        inventory["confirmed_edge_keys"] = []

        unknown, _ = POPULATION_MANIFEST_LIVE.validate_inventory(inventory, schema_rows)

        self.assertEqual(unknown, ["inventory.unconfirmed_edge.auth.account.id"])

    def test_authoritative_source_hash_drift_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "account.cpp").write_text("changed\n", encoding="utf-8")
            inventory = {
                "source_artifacts": {"auth_cleanup": "fake:account.cpp"},
                "source_hashes": {"auth_cleanup": "expected"},
            }
            arguments = type(
                "Arguments",
                (),
                {"playerbots_config": "/fixture/playerbots.conf"},
            )()
            with (
                patch.object(
                    POPULATION_MANIFEST_LIVE,
                    "_source_roots",
                    return_value={"fake": root},
                ),
                patch.object(
                    POPULATION_MANIFEST_LIVE,
                    "_run",
                    return_value="revision\n",
                ),
            ):
                _, unavailable, drift = POPULATION_MANIFEST_LIVE.source_provenance(
                    arguments, inventory, "config-hash"
                )

        self.assertEqual(unavailable, [])
        self.assertEqual(drift, ["source.auth_cleanup"])

    def test_deployed_projection_override_is_reported_as_unknown(self) -> None:
        inventory = POPULATION_MANIFEST_LIVE.load_inventory(
            MODULE_PATH.with_name("population_manifest_inventory.json")
        )
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / ".env").write_text(
                "REDIS_HOST=127.0.0.1\n"
                "REDIS_PORT=6379\n"
                "MEDIVH_SOCIAL_REDIS_STREAM=unexpected:social\n",
                encoding="utf-8",
            )
            arguments = type(
                "Arguments",
                (),
                {
                    "medivh_root": directory,
                    "redis_host": "127.0.0.1",
                    "redis_port": 6379,
                },
            )()
            _, drift, unavailable = POPULATION_MANIFEST_LIVE.projection_provenance(
                arguments, inventory
            )

        self.assertEqual(unavailable, [])
        self.assertEqual(
            drift,
            [
                "projection.redis_stream.medivh:social",
                "projection.redis_stream.unexpected:social",
                "projection.redis_string.medivh:social:cursor",
                "projection.redis_string.unexpected:social:cursor",
            ],
        )

    def test_capture_sql_uses_only_read_only_statements(self) -> None:
        inventory, schema_rows = fixture_inventory()
        _, primary_keys = POPULATION_MANIFEST_LIVE.validate_inventory(
            inventory, schema_rows
        )

        sql = POPULATION_MANIFEST_LIVE.build_capture_sql(
            inventory, primary_keys, 157, 661
        )

        statements = [
            statement.strip().upper()
            for statement in sql.split(";")
            if statement.strip()
        ]
        self.assertEqual(
            statements[0],
            "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ",
        )
        self.assertEqual(statements[1], "SET SESSION TRANSACTION READ ONLY")
        self.assertEqual(statements[2], "START TRANSACTION WITH CONSISTENT SNAPSHOT")
        self.assertEqual(statements[-1], "COMMIT")
        self.assertTrue(
            all(
                statement.startswith(("SET ", "START ", "WITH ", "SELECT ", "COMMIT"))
                for statement in statements
            )
        )
        self.assertNotRegex(
            sql.upper(),
            r"\b(DELETE|INSERT|UPDATE|REPLACE|CREATE|ALTER|DROP|TRUNCATE)\b",
        )
        self.assertIn("protected_surface.auth.account.id", sql)
        self.assertIn("WHERE id=157", sql)
        self.assertIn("guid=661 AND account=157", sql)

    def test_account_prefix_requires_one_exact_deployed_setting(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "playerbots.conf"
            config.write_text(
                'AiPlayerbot.RandomBotAccountPrefix = "rndbot"\n', encoding="utf-8"
            )
            prefix, digest = POPULATION_MANIFEST_LIVE._read_account_prefix(config)
            self.assertEqual(prefix, "rndbot")
            self.assertEqual(len(digest), 64)

            config.write_text(
                'AiPlayerbot.RandomBotAccountPrefix = "a"\n'
                'AiPlayerbot.RandomBotAccountPrefix = "b"\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                POPULATION_MANIFEST_LIVE.CaptureRefusal,
                "account_prefix_missing_or_ambiguous",
            ):
                POPULATION_MANIFEST_LIVE._read_account_prefix(config)


if __name__ == "__main__":
    unittest.main()
