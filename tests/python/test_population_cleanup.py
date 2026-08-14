from __future__ import annotations

import copy
import importlib.util
import sys
import unittest
from pathlib import Path
from unittest import mock

MODULE_PATH = (
    Path(__file__).resolve().parents[2] / "tools" / "population_cleanup_support.py"
)
sys.path.insert(0, str(MODULE_PATH.parent))
SPEC = importlib.util.spec_from_file_location("population_cleanup_support", MODULE_PATH)
assert SPEC and SPEC.loader
POPULATION_CLEANUP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POPULATION_CLEANUP)

RUNTIME_PATH = (
    Path(__file__).resolve().parents[2] / "tools" / "population_cleanup_runtime.py"
)
RUNTIME_SPEC = importlib.util.spec_from_file_location(
    "population_cleanup_runtime", RUNTIME_PATH
)
assert RUNTIME_SPEC and RUNTIME_SPEC.loader
POPULATION_CLEANUP_RUNTIME = importlib.util.module_from_spec(RUNTIME_SPEC)
RUNTIME_SPEC.loader.exec_module(POPULATION_CLEANUP_RUNTIME)


def frozen_report() -> dict[str, object]:
    manifest = {
        "identities": {
            "account_ids": [364, 365],
            "accounts": [
                {"account_id": 364, "username": "RNDBOT0"},
                {"account_id": 365, "username": "RNDBOT1"},
            ],
            "auction_ids": [],
            "auctions": [
                {
                    "auction_id": 71,
                    "bidder_account_id": None,
                    "bidder_guid": 0,
                    "item_guid": 501,
                    "owner_account_id": 364,
                    "owner_guid": 1001,
                }
            ],
            "character_guids": [1001, 1002],
            "characters": [
                {
                    "account_id": 364,
                    "character_guid": 1001,
                    "delete_account_id": None,
                    "name": "Alpha",
                },
                {
                    "account_id": 365,
                    "character_guid": 1002,
                    "delete_account_id": None,
                    "name": "Beta",
                },
            ],
            "derived": {
                "careers": [],
                "economy_positions": [],
                "llm_actors": [],
                "medivh_observed_bots": [
                    {"character_guid": 1001, "id": 91, "public_id": "observed_1"}
                ],
                "personalities": [{"character_guid": 1001}],
                "playerbots": [{"character_guid": 1001}],
                "redis_pending_entries": [],
                "redis_social_entries": [],
                "redis_telemetry_entries": [],
                "social_actors": [
                    {
                        "actor_id": 81,
                        "character_guid": 1001,
                        "public_id": "actor_1",
                    }
                ],
            },
            "item_guids": [502, 501],
            "item_locations": [],
            "item_owners": [],
            "mail": [
                {
                    "has_items": 1,
                    "item_guids": [502],
                    "mail_id": 61,
                    "message_type": 2,
                    "money": 0,
                    "receiver_account_id": 365,
                    "receiver_guid": 1002,
                    "sender_account_id": None,
                    "sender_guid": 7,
                }
            ],
        },
        "protected_baseline": {
            "accounts": [{"account_id": 157, "username": "FUITAD"}],
            "characters": [
                {
                    "account_id": 157,
                    "character_guid": 661,
                    "delete_account_id": None,
                    "name": "Deszy",
                }
            ],
            "ownership": [],
            "surfaces": {},
        },
        "protected_overlap_surfaces": [],
        "provenance": {
            "inventory_sha256": "inventory-hash",
            "schemas": {
                "acore_auth": "auth-schema",
                "acore_characters": "characters-schema",
                "acore_playerbots": "playerbots-schema",
                "medivh": "medivh-schema",
            },
            "sources": {"economy": {"git_revision": "e517f153"}},
        },
        "reference_surfaces": {},
        "shared_reference_surfaces": {},
        "surface_policies": {},
        "surfaces": {
            "characters.auctionhouse": {
                "count": 1,
                "identity_digest": "auction-surface",
            },
            "characters.characters": {
                "count": 2,
                "identity_digest": "characters-surface",
            },
        },
        "unavailable_sources": [],
        "unknown_edges": [],
        "zero_predicates": {"target_accounts": False},
    }
    digest = POPULATION_CLEANUP.canonical_digest(manifest)
    return {
        "capture_status": "STABLE",
        "digest": digest,
        "exit_code": 0,
        "first_digest": digest,
        "format_version": 2,
        "manifest": manifest,
        "refusal_reasons": [],
        "refusal_surfaces": [],
        "second_digest": digest,
        "status": "NOT_READY",
    }


def backup_record() -> dict[str, object]:
    return {
        "backup": {
            "mysql": {"path": "/backup/mysql.sql.gz", "sha256": "mysql-hash"},
            "redis": {"path": "/backup/redis.rdb", "sha256": "redis-hash"},
        },
        "final_services": {
            "medivh_maintenance": True,
            "running": ["homebrew.mxcl.mysql", "homebrew.mxcl.redis"],
            "stopped": ["com.azeroth.worldserver"],
        },
        "format_version": 1,
        "operation": "/backup",
        "protected_online_state": [
            {
                "account_id": 157,
                "account_online": 0,
                "character_guid": 661,
                "character_online": 0,
                "delete_account_id": None,
                "name": "Deszy",
            }
        ],
        "status": "VERIFIED_WRITERS_STOPPED",
    }


class PopulationCleanupTest(unittest.TestCase):
    def test_frozen_authority_requires_exact_digest_and_deszy_controls(self) -> None:
        report = frozen_report()

        POPULATION_CLEANUP.validate_frozen_authority(report, report["digest"])
        report["manifest"]["identities"]["character_guids"].append(661)

        with self.assertRaisesRegex(
            POPULATION_CLEANUP.CleanupRefusal, "frozen_digest_mismatch"
        ):
            POPULATION_CLEANUP.validate_frozen_authority(
                report, frozen_report()["digest"]
            )

    def test_external_auction_or_mail_entitlement_refuses_cleanup(self) -> None:
        report = frozen_report()

        POPULATION_CLEANUP.validate_settlement_boundary(report)
        report["manifest"]["identities"]["auctions"][0].update(
            {"bidder_account_id": 157, "bidder_guid": 661}
        )

        with self.assertRaisesRegex(
            POPULATION_CLEANUP.CleanupRefusal, "external_auction_bidder"
        ):
            POPULATION_CLEANUP.validate_settlement_boundary(report)

        report = frozen_report()
        report["manifest"]["identities"]["mail"][0].update(
            {"message_type": 0, "sender_account_id": 157, "sender_guid": 661}
        )

        with self.assertRaisesRegex(
            POPULATION_CLEANUP.CleanupRefusal, "external_mail_participant"
        ):
            POPULATION_CLEANUP.validate_settlement_boundary(report)

    def test_plan_contains_exact_targets_archive_and_deterministic_digest(self) -> None:
        report = frozen_report()
        record = backup_record()
        effects = [
            {
                "engine": "InnoDB",
                "expected_rows": 1,
                "phase": "economy",
                "predicate": "`id` IN (71)",
                "schema": "acore_characters",
                "source": "character_queries",
                "surface": "characters.auctionhouse",
                "table": "auctionhouse",
            }
        ]

        first = POPULATION_CLEANUP.build_cleanup_plan(
            report, record, effects, expected_frozen_digest=report["digest"]
        )
        second = POPULATION_CLEANUP.build_cleanup_plan(
            report, record, effects, expected_frozen_digest=report["digest"]
        )

        self.assertEqual(first, second)
        self.assertEqual(first["plan_digest"], POPULATION_CLEANUP.plan_digest(first))
        self.assertEqual(first["targets"]["account_ids"], [364, 365])
        self.assertEqual(first["targets"]["character_guids"], [1001, 1002])
        self.assertEqual(first["archive"]["mysql_sha256"], "mysql-hash")
        self.assertEqual(first["archive"]["redis_sha256"], "redis-hash")
        self.assertEqual(first["effects"][0]["expected_rows"], 1)

    def test_apply_refuses_any_change_to_the_exact_plan(self) -> None:
        report = frozen_report()
        plan = POPULATION_CLEANUP.build_cleanup_plan(
            report,
            backup_record(),
            [],
            expected_frozen_digest=report["digest"],
        )

        POPULATION_CLEANUP.validate_exact_plan(plan)
        changed = copy.deepcopy(plan)
        changed["targets"]["character_guids"].append(1003)

        with self.assertRaisesRegex(
            POPULATION_CLEANUP.CleanupRefusal, "plan_digest_mismatch"
        ):
            POPULATION_CLEANUP.validate_exact_plan(changed)

    def test_row_count_mismatch_is_an_apply_failure(self) -> None:
        POPULATION_CLEANUP.require_affected_rows("characters.auctionhouse", 1, 1)

        with self.assertRaisesRegex(
            POPULATION_CLEANUP.CleanupApplyFailure,
            "affected_rows_mismatch:characters.auctionhouse:expected=1:actual=0",
        ):
            POPULATION_CLEANUP.require_affected_rows("characters.auctionhouse", 1, 0)

    def test_shared_deszy_events_are_deleted_with_the_legacy_epoch(self) -> None:
        self.assertEqual(
            POPULATION_CLEANUP.legacy_social_event_deletion_count(578640, 1719),
            578640,
        )

        with self.assertRaisesRegex(
            POPULATION_CLEANUP.CleanupRefusal,
            "invalid_shared_legacy_social_event_count",
        ):
            POPULATION_CLEANUP.legacy_social_event_deletion_count(10, 11)

    def test_protected_reconciliation_ignores_only_legacy_reference_roles(self) -> None:
        frozen = {
            "protected_baseline": {
                "accounts": [{"account_id": 157, "username": "FUITAD"}],
                "characters": [{"character_guid": 661, "name": "Deszy"}],
                "ownership": [],
                "surfaces": {
                    "characters.characters": {"count": 1, "identity_digest": "char"},
                    "characters.item_instance": {
                        "count": 5,
                        "identity_digest": "mixed",
                    },
                    "playerbots.social_actor_rows.playerbot_social_event": {
                        "count": 1719,
                        "identity_digest": "legacy",
                    },
                },
            },
            "reference_surfaces": {
                "characters.item_instance": {
                    "protected": {
                        "owned": {"count": 4, "identity_digest": "owned"},
                        "provenance": {"count": 1, "identity_digest": "legacy"},
                    }
                },
                "playerbots.social_actor_rows.playerbot_social_event": {
                    "protected": {
                        "participant": {"count": 1719, "identity_digest": "legacy"}
                    }
                },
            },
        }
        post = copy.deepcopy(frozen)
        post["protected_baseline"]["surfaces"]["characters.item_instance"] = {
            "count": 4,
            "identity_digest": "owned",
        }
        post["protected_baseline"]["surfaces"][
            "playerbots.social_actor_rows.playerbot_social_event"
        ] = {"count": 0, "identity_digest": "empty"}
        post["reference_surfaces"]["characters.item_instance"]["protected"][
            "provenance"
        ] = {"count": 0, "identity_digest": "empty"}
        post["reference_surfaces"][
            "playerbots.social_actor_rows.playerbot_social_event"
        ]["protected"]["participant"] = {"count": 0, "identity_digest": "empty"}

        POPULATION_CLEANUP.validate_protected_reconciliation(frozen, post)
        post["protected_baseline"]["surfaces"]["characters.characters"]["count"] = 0

        with self.assertRaisesRegex(
            POPULATION_CLEANUP.CleanupApplyFailure,
            "protected_surface_changed:characters.characters",
        ):
            POPULATION_CLEANUP.validate_protected_reconciliation(frozen, post)

    def test_mysql_apply_checks_exact_counts_and_rolls_back_mismatch(self) -> None:
        effect = {
            "action": "delete",
            "engine": "InnoDB",
            "expected_rows": 1,
            "predicate": "t.`id` IN (71)",
            "schema": "acore_characters",
            "surfaces": ["characters.auctionhouse"],
            "table": "auctionhouse",
        }

        class FakeProcess:
            @staticmethod
            def poll() -> None:
                return None

        class FakeSession:
            def __init__(self, affected: int):
                self.affected = affected
                self.commands: list[str] = []
                self.process = FakeProcess()
                self.closed = False

            def command(self, sql: str, marker: str) -> None:
                self.commands.append(f"{sql}:{marker}")

            def scalar(self, sql: str, marker: str) -> int:
                return 1

            def mutate(self, sql: str, marker: str) -> int:
                return self.affected

            def close(self) -> None:
                self.closed = True

        success = FakeSession(1)
        with mock.patch.object(
            POPULATION_CLEANUP_RUNTIME,
            "MysqlMutationSession",
            return_value=success,
        ):
            results = POPULATION_CLEANUP_RUNTIME.apply_mysql("root", [effect])

        self.assertEqual(results[0]["affected_rows"], 1)
        self.assertTrue(
            any(command.startswith("COMMIT:") for command in success.commands)
        )
        self.assertTrue(success.closed)

        mismatch = FakeSession(0)
        with (
            mock.patch.object(
                POPULATION_CLEANUP_RUNTIME,
                "MysqlMutationSession",
                return_value=mismatch,
            ),
            self.assertRaisesRegex(
                POPULATION_CLEANUP_RUNTIME.CleanupApplyFailure,
                "affected_rows_mismatch:characters.auctionhouse",
            ),
        ):
            POPULATION_CLEANUP_RUNTIME.apply_mysql("root", [effect])

        self.assertTrue(
            any(command.startswith("ROLLBACK:") for command in mismatch.commands)
        )
        self.assertTrue(mismatch.closed)


if __name__ == "__main__":
    unittest.main()
