from __future__ import annotations

import contextlib
import copy
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

MODULE_PATH = Path(__file__).resolve().parents[2] / "tools" / "population_manifest.py"
SPEC = importlib.util.spec_from_file_location("population_manifest", MODULE_PATH)
assert SPEC and SPEC.loader
POPULATION_MANIFEST = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POPULATION_MANIFEST)


def fixture_capture() -> dict[str, object]:
    return {
        "captured_at": "2026-08-14T10:00:00Z",
        "provenance": {
            "schemas": {
                "acore_auth": "auth-hash",
                "acore_characters": "characters-hash",
            },
            "sources": {"azerothcore": "source-sha"},
        },
        "account_prefix": "rndbot",
        "protected_identity": {
            "account_id": 157,
            "character_guid": 661,
            "character_name": "Deszy",
        },
        "account_rows": [
            {"account_id": 364, "username": "RNDBOT0"},
            {"account_id": 365, "username": "RNDBOT1"},
            {"account_id": 157, "username": "FUITAD"},
        ],
        "ownership_rows": [
            {"account_id": 364, "account_type": 1},
            {"account_id": 365, "account_type": 2},
        ],
        "character_rows": [
            {
                "character_guid": 1001,
                "account_id": 364,
                "delete_account_id": None,
                "name": "Alpha",
            },
            {
                "character_guid": 1002,
                "account_id": 0,
                "delete_account_id": 365,
                "name": "Beta",
            },
            {
                "character_guid": 661,
                "account_id": 157,
                "delete_account_id": None,
                "name": "Deszy",
            },
        ],
        "item_owner_rows": [
            {"item_guid": 5001, "owner_guid": 1001},
            {"item_guid": 5002, "owner_guid": 1002},
            {"item_guid": 5003, "owner_guid": 1001},
            {"item_guid": 5004, "owner_guid": 1002},
        ],
        "item_location_rows": [
            {"surface": "inventory", "item_guid": 5001, "holder_id": 1001},
            {"surface": "mail", "item_guid": 5002, "holder_id": 7001},
            {"surface": "auction", "item_guid": 5003, "holder_id": 8001},
            {"surface": "guild_bank", "item_guid": 5004, "holder_id": 9001},
        ],
        "auction_rows": [
            {
                "auction_id": 8001,
                "item_guid": 5003,
                "owner_guid": 1001,
                "bidder_guid": 0,
            }
        ],
        "mail_rows": [
            {
                "mail_id": 7001,
                "sender_guid": 0,
                "receiver_guid": 1002,
                "item_guids": [5002],
            }
        ],
        "derived_identities": {
            "economy_positions": [{"public_id": "pos_1", "character_guid": 1001}],
            "llm_actors": [{"bot_guid": 1001}],
            "medivh_observed_bots": [
                {"id": 91, "public_id": "obs_1", "character_guid": 1001}
            ],
            "redis_social_entries": [{"entry_id": "2-0", "actor_public_id": "act_1"}],
            "redis_telemetry_entries": [{"entry_id": "1-0", "character_guid": 1001}],
            "social_actors": [
                {"actor_id": 81, "public_id": "act_1", "character_guid": 1001}
            ],
        },
        "surface_rows": {
            "acore_characters.character_inventory.guid": [{"guid": 1001, "item": 5001}],
            "acore_playerbots.playerbot_personality.character_guid": [
                {"character_guid": 1001}
            ],
        },
        "surface_role_rows": {},
        "protected_surface_rows": {},
        "protected_surface_role_rows": {},
        "unknown_edges": [],
        "unavailable_sources": [],
        "diagnostics": {"configured_target_count": 310},
    }


class PopulationManifestTest(unittest.TestCase):
    def test_canonical_hash_is_independent_of_input_order(self) -> None:
        first = fixture_capture()
        second = copy.deepcopy(first)
        second["account_rows"].reverse()
        second["ownership_rows"].reverse()
        second["character_rows"].reverse()
        second["item_location_rows"].reverse()
        second["derived_identities"]["social_actors"].reverse()
        second["surface_rows"] = dict(reversed(list(second["surface_rows"].items())))

        result = POPULATION_MANIFEST.evaluate_captures(first, second)

        self.assertEqual(result["capture_status"], "STABLE")
        self.assertEqual(result["first_digest"], result["second_digest"])
        self.assertEqual(result["changed_surfaces"], [])

    def test_expands_items_from_every_declared_location(self) -> None:
        snapshot = POPULATION_MANIFEST.build_snapshot(fixture_capture())

        identities = snapshot["stable_payload"]["identities"]
        self.assertEqual(identities["item_guids"], [5001, 5002, 5003, 5004])
        self.assertEqual(
            identities["item_locations"],
            [
                {"holder_id": 1001, "item_guid": 5001, "surface": "inventory"},
                {"holder_id": 7001, "item_guid": 5002, "surface": "mail"},
                {"holder_id": 8001, "item_guid": 5003, "surface": "auction"},
                {"holder_id": 9001, "item_guid": 5004, "surface": "guild_bank"},
            ],
        )

    def test_unknown_edge_forces_hard_refusal(self) -> None:
        first = fixture_capture()
        first["unknown_edges"] = ["acore_characters.future_item_location.item_guid"]

        result = POPULATION_MANIFEST.evaluate_captures(first, copy.deepcopy(first))

        self.assertEqual(result["capture_status"], "REFUSED")
        self.assertEqual(result["readiness"], "REFUSED")
        self.assertIn("unknown_edges", result["refusal_reasons"])
        self.assertIn(
            "unknown:acore_characters.future_item_location.item_guid",
            result["refusal_surfaces"],
        )

    def test_changed_identity_refuses_stability_with_exact_surface(self) -> None:
        first = fixture_capture()
        second = copy.deepcopy(first)
        second["item_location_rows"].append(
            {"surface": "inventory", "item_guid": 5005, "holder_id": 1001}
        )
        second["item_owner_rows"].append({"item_guid": 5005, "owner_guid": 1001})

        result = POPULATION_MANIFEST.evaluate_captures(first, second)

        self.assertEqual(result["capture_status"], "UNSTABLE")
        self.assertEqual(result["readiness"], "UNKNOWN")
        self.assertIn("identities.item_guids", result["changed_surfaces"])

    def test_exact_deszy_negative_controls(self) -> None:
        mutations = {
            "missing protected binding": lambda capture: capture.pop(
                "protected_identity"
            ),
            "wrong protected binding": lambda capture: capture[
                "protected_identity"
            ].update({"character_guid": 662}),
            "missing protected row": lambda capture: capture["character_rows"].pop(),
            "wrong protected name": lambda capture: capture["character_rows"][
                -1
            ].update({"name": "NotDeszy"}),
            "protected account targeted": lambda capture: capture[
                "ownership_rows"
            ].append({"account_id": 157, "account_type": 1}),
            "protected character targeted": lambda capture: capture["character_rows"][
                -1
            ].update({"account_id": 364}),
            "protected mail overlap": lambda capture: capture["mail_rows"].append(
                {
                    "mail_id": 7002,
                    "sender_guid": 1001,
                    "receiver_guid": 661,
                    "item_guids": [],
                }
            ),
            "protected derived actor overlap": lambda capture: capture[
                "derived_identities"
            ]["llm_actors"].append({"bot_guid": 661}),
            "protected inventory edge overlap": lambda capture: capture[
                "protected_surface_rows"
            ].update(
                {
                    "acore_characters.character_inventory.guid": [
                        {"guid": 1001, "item": 5001}
                    ]
                }
            ),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                first = fixture_capture()
                mutate(first)
                result = POPULATION_MANIFEST.evaluate_captures(
                    first, copy.deepcopy(first)
                )
                self.assertEqual(result["capture_status"], "REFUSED")
                self.assertIn("protected_deszy", result["refusal_reasons"])

    def test_classified_shared_references_do_not_refuse_bot_owned_cleanup(self) -> None:
        first = fixture_capture()
        item = {"guid": 5001}
        target_relationship = {"id": 9001}
        protected_relationship = {"id": 9002}
        event = {"id": 10001}
        first["surface_rows"].update(
            {
                "characters.item_instance": [item],
                "playerbots.social_event": [event],
                "playerbots.social_relationship": [
                    target_relationship,
                    protected_relationship,
                ],
            }
        )
        first["protected_surface_rows"].update(
            {
                "characters.item_instance": [item],
                "playerbots.social_event": [event],
                "playerbots.social_relationship": [
                    target_relationship,
                    protected_relationship,
                ],
            }
        )
        first["surface_role_rows"] = {
            "characters.item_instance": {"owned": [item], "provenance": []},
            "playerbots.social_event": {"participant": [event]},
            "playerbots.social_relationship": {
                "owned": [target_relationship],
                "participant": [protected_relationship],
            },
        }
        first["protected_surface_role_rows"] = {
            "characters.item_instance": {"owned": [], "provenance": [item]},
            "playerbots.social_event": {"participant": [event]},
            "playerbots.social_relationship": {
                "owned": [protected_relationship],
                "participant": [target_relationship],
            },
        }
        first["surface_policies"] = {
            "characters.item_instance": {
                "cleanup_behavior": "delete_owned",
                "closure_required": True,
            },
            "playerbots.social_event": {
                "cleanup_behavior": "retain",
                "closure_required": False,
            },
            "playerbots.social_relationship": {
                "cleanup_behavior": "delete_owned",
                "closure_required": True,
            },
        }

        result = POPULATION_MANIFEST.evaluate_captures(first, copy.deepcopy(first))

        self.assertEqual(result["capture_status"], "STABLE")
        self.assertEqual(result["refusal_reasons"], [])
        manifest = result["manifest"]
        self.assertEqual(manifest["protected_overlap_surfaces"], [])
        shared = manifest["shared_reference_surfaces"]
        self.assertEqual(
            shared["characters.item_instance"]["owned_to_provenance"]["count"], 1
        )
        self.assertEqual(
            shared["playerbots.social_event"]["participant_to_participant"]["count"],
            1,
        )
        self.assertEqual(
            shared["playerbots.social_relationship"]["owned_to_participant"]["count"],
            1,
        )
        self.assertEqual(
            shared["playerbots.social_relationship"]["participant_to_owned"]["count"],
            1,
        )
        self.assertNotIn("surface.playerbots.social_event", result["zero_predicates"])

    def test_classified_target_owned_protected_owned_overlap_still_refuses(
        self,
    ) -> None:
        first = fixture_capture()
        row = {"id": 9001}
        first["surface_rows"]["playerbots.social_relationship"] = [row]
        first["protected_surface_rows"]["playerbots.social_relationship"] = [row]
        first["surface_role_rows"] = {
            "playerbots.social_relationship": {"owned": [row], "participant": []}
        }
        first["protected_surface_role_rows"] = {
            "playerbots.social_relationship": {"owned": [row], "participant": []}
        }
        first["surface_policies"] = {
            "playerbots.social_relationship": {
                "cleanup_behavior": "delete_owned",
                "closure_required": True,
            }
        }

        result = POPULATION_MANIFEST.evaluate_captures(first, copy.deepcopy(first))

        self.assertEqual(result["capture_status"], "REFUSED")
        self.assertIn("protected_deszy", result["refusal_reasons"])
        self.assertIn(
            "protected_overlap:playerbots.social_relationship",
            result["refusal_surfaces"],
        )

    def test_retained_references_do_not_hold_cleanup_zero_predicates_open(self) -> None:
        first = fixture_capture()
        item = {"guid": 5001}
        relationship = {"id": 9001}
        first["surface_rows"].update(
            {
                "characters.item_instance": [item],
                "playerbots.social_relationship": [relationship],
            }
        )
        first["protected_surface_rows"].update(
            {
                "characters.item_instance": [item],
                "playerbots.social_relationship": [relationship],
            }
        )
        first["surface_role_rows"] = {
            "characters.item_instance": {"owned": [], "provenance": [item]},
            "playerbots.social_relationship": {
                "owned": [],
                "participant": [relationship],
            },
        }
        first["protected_surface_role_rows"] = {
            "characters.item_instance": {"owned": [item], "provenance": []},
            "playerbots.social_relationship": {
                "owned": [relationship],
                "participant": [],
            },
        }
        first["surface_policies"] = {
            "characters.item_instance": {
                "cleanup_behavior": "delete_owned",
                "closure_required": True,
            },
            "playerbots.social_relationship": {
                "cleanup_behavior": "delete_owned",
                "closure_required": True,
            },
        }

        result = POPULATION_MANIFEST.evaluate_captures(first, copy.deepcopy(first))

        self.assertEqual(result["capture_status"], "STABLE")
        self.assertTrue(result["zero_predicates"]["surface.characters.item_instance"])
        self.assertTrue(
            result["zero_predicates"]["surface.playerbots.social_relationship"]
        )
        self.assertEqual(
            result["manifest"]["surfaces"]["characters.item_instance"]["count"],
            1,
        )
        self.assertEqual(
            result["manifest"]["surfaces"]["playerbots.social_relationship"]["count"],
            1,
        )

    def test_target_count_is_diagnostic_not_authority(self) -> None:
        first = fixture_capture()
        second = copy.deepcopy(first)
        second["diagnostics"]["configured_target_count"] = 160

        result = POPULATION_MANIFEST.evaluate_captures(first, second)

        self.assertEqual(result["capture_status"], "STABLE")
        self.assertEqual(result["first_digest"], result["second_digest"])
        self.assertEqual(result["manifest"]["identities"]["account_ids"], [364, 365])

    def test_account_name_and_ownership_authorities_must_be_equal(self) -> None:
        first = fixture_capture()
        first["account_rows"].append({"account_id": 366, "username": "RNDBOT2"})

        result = POPULATION_MANIFEST.evaluate_captures(first, copy.deepcopy(first))

        self.assertEqual(result["capture_status"], "REFUSED")
        self.assertIn("account_authority_mismatch", result["refusal_reasons"])

    def test_known_unassigned_account_type_is_not_a_target(self) -> None:
        first = fixture_capture()
        first["ownership_rows"].append({"account_id": 400, "account_type": 0})

        result = POPULATION_MANIFEST.evaluate_captures(first, copy.deepcopy(first))

        self.assertEqual(result["capture_status"], "STABLE")
        self.assertEqual(result["manifest"]["identities"]["account_ids"], [364, 365])

    def test_stable_nonzero_manifest_is_not_ready(self) -> None:
        first = fixture_capture()

        result = POPULATION_MANIFEST.evaluate_captures(first, copy.deepcopy(first))

        self.assertEqual(result["capture_status"], "STABLE")
        self.assertEqual(result["readiness"], "NOT_READY")
        self.assertFalse(result["zero_predicates"]["target_accounts"])
        self.assertFalse(result["zero_predicates"]["target_items"])

    def test_live_cli_preserves_attributable_not_ready_report(self) -> None:
        first = fixture_capture()
        second = copy.deepcopy(first)
        second["captured_at"] = "2026-08-14T10:00:02Z"
        with tempfile.TemporaryDirectory() as directory:
            output_path = Path(directory) / "manifest.json"
            stdout = io.StringIO()
            with (
                patch.object(
                    POPULATION_MANIFEST,
                    "capture_live_pair",
                    return_value=(first, second),
                ),
                contextlib.redirect_stdout(stdout),
            ):
                exit_code = POPULATION_MANIFEST.main(
                    [
                        "live",
                        "--playerbots-config",
                        "/fixture/playerbots.conf",
                        "--protected-account-id",
                        "157",
                        "--protected-character-guid",
                        "661",
                        "--protected-character-name",
                        "Deszy",
                        "--output",
                        str(output_path),
                    ]
                )

            report = json.loads(stdout.getvalue())
            preserved = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(exit_code, 0)
            self.assertEqual(report, preserved)
            self.assertEqual(report["status"], "NOT_READY")
            self.assertEqual(report["counts"]["target_accounts"], 2)
            self.assertEqual(report["counts"]["target_characters"], 2)
            self.assertEqual(report["counts"]["target_items"], 4)
            self.assertEqual(report["digest"], report["first_digest"])

    def test_cli_exit_semantics_fail_closed(self) -> None:
        refusal = fixture_capture()
        refusal["unknown_edges"] = ["acore_characters.future_owner.guid"]
        unstable = fixture_capture()
        changed = copy.deepcopy(unstable)
        changed["surface_rows"]["new.surface"] = [{"id": 1}]

        cases = [
            (refusal, copy.deepcopy(refusal), 4, "REFUSED"),
            (unstable, changed, 3, "UNSTABLE"),
        ]
        for first, second, expected_exit, expected_status in cases:
            with self.subTest(expected_status=expected_status):
                stdout = io.StringIO()
                with (
                    patch.object(
                        POPULATION_MANIFEST,
                        "capture_live_pair",
                        return_value=(first, second),
                    ),
                    contextlib.redirect_stdout(stdout),
                ):
                    exit_code = POPULATION_MANIFEST.main(
                        [
                            "live",
                            "--playerbots-config",
                            "/fixture/playerbots.conf",
                            "--protected-account-id",
                            "157",
                            "--protected-character-guid",
                            "661",
                            "--protected-character-name",
                            "Deszy",
                        ]
                    )
                self.assertEqual(exit_code, expected_exit)
                self.assertEqual(
                    json.loads(stdout.getvalue())["status"], expected_status
                )

        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            exit_code = POPULATION_MANIFEST.main(
                [
                    "live",
                    "--playerbots-config",
                    "/fixture/playerbots.conf",
                    "--protected-account-id",
                    "157",
                    "--protected-character-guid",
                    "662",
                    "--protected-character-name",
                    "Deszy",
                ]
            )
        self.assertEqual(exit_code, 4)
        self.assertEqual(
            json.loads(stdout.getvalue())["refusal_reasons"],
            ["protected_binding_inexact"],
        )

    def test_compare_cli_reports_exact_manifest_changes(self) -> None:
        first = POPULATION_MANIFEST.operational_report(
            POPULATION_MANIFEST.evaluate_captures(fixture_capture(), fixture_capture())
        )
        changed_capture = fixture_capture()
        changed_capture["character_rows"][0]["name"] = "Changed"
        second = POPULATION_MANIFEST.operational_report(
            POPULATION_MANIFEST.evaluate_captures(changed_capture, changed_capture)
        )
        with tempfile.TemporaryDirectory() as directory:
            first_path = Path(directory) / "first.json"
            second_path = Path(directory) / "second.json"
            first_path.write_text(json.dumps(first), encoding="utf-8")
            second_path.write_text(json.dumps(second), encoding="utf-8")
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                exit_code = POPULATION_MANIFEST.main(
                    ["compare", str(first_path), str(second_path)]
                )

        report = json.loads(stdout.getvalue())
        self.assertEqual(exit_code, 5)
        self.assertEqual(report["comparison_status"], "CHANGED")
        self.assertIn("identities.characters", report["changed_surfaces"])

    def test_compare_cli_refuses_nonstable_report(self) -> None:
        report = POPULATION_MANIFEST.operational_report(
            POPULATION_MANIFEST.evaluate_captures(fixture_capture(), fixture_capture())
        )
        report["capture_status"] = "UNSTABLE"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "unstable.json"
            path.write_text(json.dumps(report), encoding="utf-8")
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                exit_code = POPULATION_MANIFEST.main(["compare", str(path), str(path)])

        self.assertEqual(exit_code, 4)
        self.assertEqual(json.loads(stdout.getvalue())["comparison_status"], "REFUSED")

    def test_compare_cli_refuses_valid_json_with_invalid_report_shape(self) -> None:
        for invalid_report in ([], None, 17, "manifest"):
            with self.subTest(invalid_report=invalid_report):
                with tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "invalid.json"
                    path.write_text(json.dumps(invalid_report), encoding="utf-8")
                    stdout = io.StringIO()
                    with contextlib.redirect_stdout(stdout):
                        exit_code = POPULATION_MANIFEST.main(
                            ["compare", str(path), str(path)]
                        )

                self.assertEqual(exit_code, 4)
                self.assertEqual(
                    json.loads(stdout.getvalue())["comparison_status"], "REFUSED"
                )


if __name__ == "__main__":
    unittest.main()
