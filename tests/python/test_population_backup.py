from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path

MODULE_PATH = Path(__file__).resolve().parents[2] / "tools" / "population_backup.py"
sys.path.insert(0, str(MODULE_PATH.parent))
SPEC = importlib.util.spec_from_file_location("population_backup", MODULE_PATH)
assert SPEC and SPEC.loader
POPULATION_BACKUP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POPULATION_BACKUP)


class PopulationBackupTest(unittest.TestCase):
    @staticmethod
    def protected_report(status: str = "NOT_READY") -> dict[str, object]:
        return {
            "status": status,
            "digest": "abc",
            "counts": {"target_accounts": 2, "target_characters": 4},
            "refusal_reasons": [],
            "refusal_surfaces": [],
            "manifest": {
                "unknown_edges": [],
                "unavailable_sources": [],
                "protected_overlap_surfaces": [],
                "protected_baseline": {
                    "accounts": [{"account_id": 157}],
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
            },
        }

    def test_dump_command_locks_every_authoritative_schema_together(self) -> None:
        command = POPULATION_BACKUP.mysql_dump_command("root")

        self.assertIn("--lock-all-tables", command)
        self.assertIn("--set-gtid-purged=OFF", command)
        self.assertNotIn("--single-transaction", command)
        databases = command.index("--databases")
        self.assertEqual(
            command[databases + 1 :],
            ["acore_auth", "acore_characters", "acore_playerbots", "medivh"],
        )

    def test_protected_baseline_requires_the_exact_deszy_binding(self) -> None:
        report = self.protected_report()

        POPULATION_BACKUP.validate_protected_baseline(report)
        report["manifest"]["protected_baseline"]["characters"][0]["name"] = "Other"

        with self.assertRaisesRegex(POPULATION_BACKUP.SafetyRefusal, "protected_deszy"):
            POPULATION_BACKUP.validate_protected_baseline(report)

    def test_protected_online_state_requires_both_account_and_character_offline(
        self,
    ) -> None:
        rows = [
            {
                "account_id": 157,
                "account_online": 0,
                "character_guid": 661,
                "character_online": 0,
                "delete_account_id": None,
                "name": "Deszy",
            }
        ]

        POPULATION_BACKUP.validate_protected_online_state(rows)
        rows[0]["character_online"] = 1

        with self.assertRaisesRegex(
            POPULATION_BACKUP.SafetyRefusal, "protected_deszy:online_state"
        ):
            POPULATION_BACKUP.validate_protected_online_state(rows)

    def test_isolated_runtime_paths_cannot_escape_the_operation_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            operation = Path(directory).resolve()
            POPULATION_BACKUP.require_isolated_path(
                operation, operation / "rehearsal/mysql"
            )

            with self.assertRaisesRegex(
                POPULATION_BACKUP.SafetyRefusal, "outside_operation"
            ):
                POPULATION_BACKUP.require_isolated_path(
                    operation, operation.parent / "live"
                )

    def test_service_partition_rejects_a_required_writer(self) -> None:
        operation = object.__new__(POPULATION_BACKUP.PopulationBackupOperation)
        operation.arguments = Namespace(
            writer_service=[("writer", Path("/writer.plist"))],
            required_running_service=[("writer", Path("/required.plist"))],
        )

        with self.assertRaisesRegex(
            POPULATION_BACKUP.SafetyRefusal, "service_partition_overlap"
        ):
            operation._validate_service_partition()

    def test_unstable_audit_retries_but_refusal_stops_immediately(self) -> None:
        reports = iter(
            [
                {"status": "UNSTABLE", "refusal_reasons": [], "refusal_surfaces": []},
                self.protected_report(),
            ]
        )
        attempts: list[int] = []

        report = POPULATION_BACKUP.audit_until_stable(
            lambda attempt: attempts.append(attempt) or next(reports), 3
        )

        self.assertEqual(report["status"], "NOT_READY")
        self.assertEqual(attempts, [1, 2])

        with self.assertRaisesRegex(POPULATION_BACKUP.SafetyRefusal, "audit_refused"):
            POPULATION_BACKUP.audit_until_stable(
                lambda attempt: {
                    "status": "REFUSED",
                    "refusal_reasons": ["unknown_edge"],
                    "refusal_surfaces": ["surface"],
                },
                3,
            )

    def test_restored_report_must_match_digest_counts_and_protected_state(self) -> None:
        frozen = self.protected_report()

        POPULATION_BACKUP.validate_restored_report(frozen, dict(frozen))
        changed = dict(frozen)
        changed["digest"] = "def"

        with self.assertRaisesRegex(
            POPULATION_BACKUP.SafetyRefusal, "restore_digest_mismatch"
        ):
            POPULATION_BACKUP.validate_restored_report(frozen, changed)

        changed = dict(frozen)
        changed["counts"] = {"target_accounts": 3, "target_characters": 4}

        with self.assertRaisesRegex(
            POPULATION_BACKUP.SafetyRefusal, "restore_counts_mismatch"
        ):
            POPULATION_BACKUP.validate_restored_report(frozen, changed)


if __name__ == "__main__":
    unittest.main()
