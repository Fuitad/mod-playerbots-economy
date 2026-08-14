from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

PROTECTED_ACCOUNT_ID = 157
PROTECTED_CHARACTER_GUID = 661
PROTECTED_CHARACTER_NAME = "Deszy"
TARGET_ACCOUNT_TYPES = frozenset({1, 2})
KNOWN_ACCOUNT_TYPES = frozenset({0, 1, 2})
EXIT_BY_STATUS = {"READY": 0, "NOT_READY": 0, "UNSTABLE": 3, "REFUSED": 4}
FORMAT_VERSION = 2


class OperationalRefusal(RuntimeError):
    pass


def canonicalize(value: Any) -> Any:
    if isinstance(value, dict):
        return {key: canonicalize(value[key]) for key in sorted(value)}
    if isinstance(value, list):
        normalized = [canonicalize(entry) for entry in value]
        return sorted(normalized, key=canonical_json)
    return value


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def canonical_digest(value: Any) -> str:
    return hashlib.sha256(
        canonical_json(canonicalize(value)).encode("utf-8")
    ).hexdigest()


def generated_account_ids(account_rows: list[dict[str, Any]], prefix: str) -> set[int]:
    exact_name = re.compile(rf"{re.escape(prefix)}(?:0|[1-9][0-9]*)", re.IGNORECASE)
    return {
        int(row["account_id"])
        for row in account_rows
        if exact_name.fullmatch(str(row["username"])) is not None
    }


def protected_identity_is_exact(
    capture: dict[str, Any], target_accounts: set[int]
) -> bool:
    protected = capture.get("protected_identity")
    if protected != {
        "account_id": PROTECTED_ACCOUNT_ID,
        "character_guid": PROTECTED_CHARACTER_GUID,
        "character_name": PROTECTED_CHARACTER_NAME,
    }:
        return False
    protected_accounts = [
        row
        for row in capture["account_rows"]
        if int(row["account_id"]) == PROTECTED_ACCOUNT_ID
    ]
    protected_characters = [
        row
        for row in capture["character_rows"]
        if int(row["character_guid"]) == PROTECTED_CHARACTER_GUID
    ]
    protected_ownership = [
        row
        for row in capture["ownership_rows"]
        if int(row["account_id"]) == PROTECTED_ACCOUNT_ID
    ]
    return (
        len(protected_accounts) == 1
        and len(protected_characters) == 1
        and int(protected_characters[0]["account_id"]) == PROTECTED_ACCOUNT_ID
        and protected_characters[0]["delete_account_id"] is None
        and protected_characters[0]["name"] == PROTECTED_CHARACTER_NAME
        and not protected_ownership
        and PROTECTED_ACCOUNT_ID not in target_accounts
    )


def contains_protected_actor(value: Any) -> bool:
    if isinstance(value, dict):
        for key, entry in value.items():
            if key in {"azeroth_guid", "bot_guid", "character_guid"}:
                try:
                    identity = int(entry)
                except (TypeError, ValueError):
                    return True
                if identity == PROTECTED_CHARACTER_GUID:
                    return True
            if contains_protected_actor(entry):
                return True
    elif isinstance(value, list):
        return any(contains_protected_actor(entry) for entry in value)
    return False


def build_snapshot(capture: dict[str, Any]) -> dict[str, Any]:
    ownership_types = {int(row["account_type"]) for row in capture["ownership_rows"]}
    ownership_accounts = {
        int(row["account_id"])
        for row in capture["ownership_rows"]
        if int(row["account_type"]) in TARGET_ACCOUNT_TYPES
    }
    name_accounts = generated_account_ids(
        capture["account_rows"], str(capture["account_prefix"])
    )
    account_ids = sorted(ownership_accounts)
    character_rows = [
        row
        for row in capture["character_rows"]
        if int(row["account_id"]) in ownership_accounts
        or (
            row["delete_account_id"] is not None
            and int(row["delete_account_id"]) in ownership_accounts
        )
    ]
    character_guids = sorted(int(row["character_guid"]) for row in character_rows)
    character_guid_set = set(character_guids)
    item_owner_rows = [
        row
        for row in capture["item_owner_rows"]
        if int(row["owner_guid"]) in character_guid_set
    ]
    owned_item_guids = {int(row["item_guid"]) for row in item_owner_rows}
    located_item_guids = {
        int(row["item_guid"]) for row in capture["item_location_rows"]
    }
    item_guids = sorted(owned_item_guids | located_item_guids)
    auction_rows = canonicalize(capture["auction_rows"])
    mail_rows = canonicalize(capture["mail_rows"])
    derived = canonicalize(capture["derived_identities"])
    surface_policies = canonicalize(capture.get("surface_policies", {}))
    surfaces = canonicalize(
        {
            key: {"count": len(rows), "identity_digest": canonical_digest(rows)}
            for key, rows in capture["surface_rows"].items()
        }
    )
    protected_surface_rows = capture.get("protected_surface_rows")
    if not isinstance(protected_surface_rows, dict):
        protected_surface_rows = {}
        protected_capture_missing = True
    else:
        protected_capture_missing = False
    protected_surfaces = canonicalize(
        {
            key: {"count": len(rows), "identity_digest": canonical_digest(rows)}
            for key, rows in protected_surface_rows.items()
        }
    )
    surface_role_rows = capture.get("surface_role_rows", {})
    protected_surface_role_rows = capture.get("protected_surface_role_rows", {})
    classified_surfaces = {
        key
        for key, policy in surface_policies.items()
        if policy.get("cleanup_behavior") in {"delete_owned", "retain"}
    }
    reference_surfaces: dict[str, Any] = {}
    shared_reference_surfaces: dict[str, Any] = {}
    protected_overlap_surfaces: list[str] = []
    for key in sorted(set(capture["surface_rows"]) & set(protected_surface_rows)):
        target_rows = {
            canonical_json(canonicalize(row)) for row in capture["surface_rows"][key]
        }
        protected_rows = {
            canonical_json(canonicalize(row)) for row in protected_surface_rows[key]
        }
        if key not in classified_surfaces:
            if target_rows & protected_rows:
                protected_overlap_surfaces.append(key)
            continue

        target_roles = surface_role_rows.get(key, {})
        protected_roles = protected_surface_role_rows.get(key, {})
        reference_surfaces[key] = {
            "cleanup_behavior": surface_policies[key]["cleanup_behavior"],
            "protected": {
                role: {"count": len(rows), "identity_digest": canonical_digest(rows)}
                for role, rows in sorted(protected_roles.items())
            },
            "target": {
                role: {"count": len(rows), "identity_digest": canonical_digest(rows)}
                for role, rows in sorted(target_roles.items())
            },
        }
        overlaps: dict[str, Any] = {}
        for target_role, rows in sorted(target_roles.items()):
            target_role_rows = {canonical_json(canonicalize(row)) for row in rows}
            for protected_role, protected_role_values in sorted(
                protected_roles.items()
            ):
                shared = target_role_rows & {
                    canonical_json(canonicalize(row)) for row in protected_role_values
                }
                if shared:
                    decoded = [json.loads(row) for row in sorted(shared)]
                    overlaps[f"{target_role}_to_{protected_role}"] = {
                        "count": len(decoded),
                        "identity_digest": canonical_digest(decoded),
                    }
        if overlaps:
            shared_reference_surfaces[key] = overlaps
        if surface_policies[key]["cleanup_behavior"] == "delete_owned":
            target_owned = {
                canonical_json(canonicalize(row))
                for row in target_roles.get("owned", [])
            }
            protected_owned = {
                canonical_json(canonicalize(row))
                for row in protected_roles.get("owned", [])
            }
            if target_owned & protected_owned:
                protected_overlap_surfaces.append(key)
    errors: list[str] = []
    if name_accounts != ownership_accounts:
        errors.append("account_authority_mismatch")
    if not ownership_types.issubset(KNOWN_ACCOUNT_TYPES):
        errors.append("unknown_account_type")
    if not protected_identity_is_exact(capture, ownership_accounts):
        errors.append("protected_deszy")
    if PROTECTED_CHARACTER_GUID in character_guid_set:
        errors.append("protected_deszy")
    if any(
        PROTECTED_CHARACTER_GUID in {int(row["owner_guid"]), int(row["bidder_guid"])}
        for row in auction_rows
    ):
        errors.append("protected_deszy")
    if any(
        PROTECTED_CHARACTER_GUID in {int(row["sender_guid"]), int(row["receiver_guid"])}
        for row in mail_rows
    ):
        errors.append("protected_deszy")
    if contains_protected_actor(derived):
        errors.append("protected_deszy")
    if protected_capture_missing:
        errors.append("protected_capture_missing")
    if protected_overlap_surfaces:
        errors.append("protected_deszy")
    if owned_item_guids != located_item_guids:
        errors.append("item_ownership_location_mismatch")
    if capture["unknown_edges"]:
        errors.append("unknown_edges")
    if capture["unavailable_sources"]:
        errors.append("unavailable_sources")

    identities = {
        "account_ids": account_ids,
        "accounts": [
            row
            for row in capture["account_rows"]
            if int(row["account_id"]) in ownership_accounts
        ],
        "auctions": auction_rows,
        "character_guids": character_guids,
        "characters": character_rows,
        "derived": derived,
        "item_guids": item_guids,
        "item_locations": capture["item_location_rows"],
        "item_owners": item_owner_rows,
        "mail": mail_rows,
    }
    zero_predicates = {
        "target_accounts": not account_ids,
        "target_auctions": not auction_rows,
        "target_characters": not character_guids,
        "target_items": not item_guids,
        "target_mail": not mail_rows,
    }
    for key, rows in derived.items():
        zero_predicates[f"derived.{key}"] = not rows
    for key, summary in surfaces.items():
        policy = surface_policies.get(key, {})
        if not policy.get("closure_required", True):
            continue
        if policy.get("cleanup_behavior") == "delete_owned":
            zero_predicates[f"surface.{key}"] = not surface_role_rows.get(key, {}).get(
                "owned", []
            )
        else:
            zero_predicates[f"surface.{key}"] = summary["count"] == 0

    stable_payload = canonicalize(
        {
            "identities": identities,
            "protected_baseline": {
                "accounts": [
                    row
                    for row in capture["account_rows"]
                    if int(row["account_id"]) == PROTECTED_ACCOUNT_ID
                ],
                "characters": [
                    row
                    for row in capture["character_rows"]
                    if int(row["character_guid"]) == PROTECTED_CHARACTER_GUID
                ],
                "ownership": [
                    row
                    for row in capture["ownership_rows"]
                    if int(row["account_id"]) == PROTECTED_ACCOUNT_ID
                ],
                "surfaces": protected_surfaces,
            },
            "protected_overlap_surfaces": protected_overlap_surfaces,
            "reference_surfaces": reference_surfaces,
            "shared_reference_surfaces": shared_reference_surfaces,
            "provenance": capture["provenance"],
            "surface_policies": surface_policies,
            "surfaces": surfaces,
            "unknown_edges": capture["unknown_edges"],
            "unavailable_sources": capture["unavailable_sources"],
            "zero_predicates": zero_predicates,
        }
    )
    return {
        "captured_at": capture["captured_at"],
        "errors": sorted(set(errors)),
        "stable_payload": stable_payload,
    }


def changed_paths(first: Any, second: Any, prefix: str = "") -> list[str]:
    if type(first) is not type(second):
        return [prefix or "manifest"]
    if isinstance(first, dict):
        paths: list[str] = []
        for key in sorted(set(first) | set(second)):
            path = f"{prefix}.{key}" if prefix else key
            if key not in first or key not in second:
                paths.append(path)
            else:
                paths.extend(changed_paths(first[key], second[key], path))
        return paths
    if first != second:
        return [prefix or "manifest"]
    return []


def evaluate_captures(
    first_capture: dict[str, Any], second_capture: dict[str, Any]
) -> dict[str, Any]:
    first = build_snapshot(first_capture)
    second = build_snapshot(second_capture)
    first_payload = first["stable_payload"]
    second_payload = second["stable_payload"]
    first_digest = canonical_digest(first_payload)
    second_digest = canonical_digest(second_payload)
    refusal_reasons = sorted(set(first["errors"]) | set(second["errors"]))
    if refusal_reasons:
        refusal_surfaces = sorted(
            set(refusal_reasons)
            | {
                f"unknown:{edge}"
                for edge in first_payload["unknown_edges"]
                + second_payload["unknown_edges"]
            }
            | {
                f"unavailable:{source}"
                for source in first_payload["unavailable_sources"]
                + second_payload["unavailable_sources"]
            }
            | {
                f"protected_overlap:{surface}"
                for surface in first_payload["protected_overlap_surfaces"]
                + second_payload["protected_overlap_surfaces"]
            }
        )
        return {
            "capture_status": "REFUSED",
            "capture_times": [first["captured_at"], second["captured_at"]],
            "changed_surfaces": changed_paths(first_payload, second_payload),
            "first_digest": first_digest,
            "manifest": first_payload,
            "readiness": "REFUSED",
            "refusal_reasons": refusal_reasons,
            "refusal_surfaces": refusal_surfaces,
            "second_digest": second_digest,
            "zero_predicates": first_payload["zero_predicates"],
        }

    differences = changed_paths(first_payload, second_payload)
    if differences or first_digest != second_digest:
        return {
            "capture_status": "UNSTABLE",
            "capture_times": [first["captured_at"], second["captured_at"]],
            "changed_surfaces": differences,
            "first_digest": first_digest,
            "manifest": first_payload,
            "readiness": "UNKNOWN",
            "refusal_reasons": [],
            "refusal_surfaces": [],
            "second_digest": second_digest,
            "zero_predicates": first_payload["zero_predicates"],
        }

    zero_predicates = first_payload["zero_predicates"]
    readiness = "READY" if all(zero_predicates.values()) else "NOT_READY"
    return {
        "capture_status": "STABLE",
        "capture_times": [first["captured_at"], second["captured_at"]],
        "changed_surfaces": [],
        "first_digest": first_digest,
        "manifest": first_payload,
        "readiness": readiness,
        "refusal_reasons": [],
        "refusal_surfaces": [],
        "second_digest": second_digest,
        "zero_predicates": zero_predicates,
    }


def capture_live_pair(
    arguments: argparse.Namespace,
) -> tuple[dict[str, Any], dict[str, Any]]:
    from population_manifest_live import (
        CaptureRefusal,
    )
    from population_manifest_live import (
        capture_live_pair as capture_pair,
    )

    try:
        return capture_pair(arguments)
    except CaptureRefusal as error:
        raise OperationalRefusal(str(error)) from error


def result_counts(result: dict[str, Any]) -> dict[str, Any]:
    manifest = result["manifest"]
    identities = manifest["identities"]
    return {
        "derived": {
            key: len(rows) for key, rows in sorted(identities["derived"].items())
        },
        "surfaces": {
            key: summary["count"]
            for key, summary in sorted(manifest["surfaces"].items())
        },
        "target_accounts": len(identities["account_ids"]),
        "target_auctions": len(identities["auctions"]),
        "target_characters": len(identities["character_guids"]),
        "target_items": len(identities["item_guids"]),
        "target_mail": len(identities["mail"]),
    }


def operational_report(result: dict[str, Any]) -> dict[str, Any]:
    if result["capture_status"] == "STABLE":
        status = result["readiness"]
        digest = result["first_digest"]
    else:
        status = result["capture_status"]
        digest = None
    return canonicalize(
        {
            "capture_status": result["capture_status"],
            "capture_times": result["capture_times"],
            "changed_surfaces": result["changed_surfaces"],
            "counts": result_counts(result),
            "digest": digest,
            "exit_code": EXIT_BY_STATUS[status],
            "first_digest": result["first_digest"],
            "format_version": FORMAT_VERSION,
            "manifest": result["manifest"],
            "refusal_reasons": result["refusal_reasons"],
            "refusal_surfaces": result["refusal_surfaces"],
            "second_digest": result["second_digest"],
            "status": status,
            "zero_predicates": result["zero_predicates"],
        }
    )


def write_report(report: dict[str, Any], output_path: str | None) -> None:
    rendered = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if output_path is not None:
        Path(output_path).write_text(rendered, encoding="utf-8")
    sys.stdout.write(rendered)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Capture and verify the generated Playerbots population manifest."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    live = subparsers.add_parser("live", help="capture the live realm twice")
    live.add_argument("--playerbots-config", required=True)
    live.add_argument("--protected-account-id", required=True, type=int)
    live.add_argument("--protected-character-guid", required=True, type=int)
    live.add_argument("--protected-character-name", required=True)
    live.add_argument(
        "--inventory",
        default=str(Path(__file__).with_name("population_manifest_inventory.json")),
    )
    live.add_argument("--mysql-defaults-group-suffix", default="root")
    live.add_argument("--azerothcore-root")
    live.add_argument("--medivh-root")
    live.add_argument("--redis-host", default="127.0.0.1")
    live.add_argument("--redis-port", default=6379, type=int)
    live.add_argument("--capture-delay-seconds", default=2.0, type=float)
    live.add_argument("--output")
    compare = subparsers.add_parser("compare", help="compare two preserved reports")
    compare.add_argument("first")
    compare.add_argument("second")
    compare.add_argument("--output")
    return parser


def compare_reports(first_path: str, second_path: str) -> tuple[dict[str, Any], int]:
    reports = []
    for path in (first_path, second_path):
        report = json.loads(Path(path).read_text(encoding="utf-8"))
        manifest = report.get("manifest") if isinstance(report, dict) else None
        valid = isinstance(report, dict) and (
            report.get("format_version") == FORMAT_VERSION
            and report.get("capture_status") == "STABLE"
            and report.get("status") in {"READY", "NOT_READY"}
            and isinstance(manifest, dict)
            and report.get("digest") == canonical_digest(manifest)
            and report.get("refusal_reasons") == []
            and report.get("refusal_surfaces") == []
            and manifest.get("unknown_edges") == []
            and manifest.get("unavailable_sources") == []
            and manifest.get("protected_overlap_surfaces") == []
        )
        if not valid:
            refusal = {
                "changed_surfaces": [],
                "comparison_status": "REFUSED",
                "exit_code": EXIT_BY_STATUS["REFUSED"],
                "format_version": FORMAT_VERSION,
                "refusal_reasons": [f"invalid_report:{path}"],
                "refusal_surfaces": [path],
            }
            return refusal, EXIT_BY_STATUS["REFUSED"]
        reports.append(report)
    first_manifest = canonicalize(reports[0]["manifest"])
    second_manifest = canonicalize(reports[1]["manifest"])
    differences = changed_paths(first_manifest, second_manifest)
    exit_code = 0 if not differences else 5
    return (
        canonicalize(
            {
                "changed_surfaces": differences,
                "comparison_status": "IDENTICAL" if not differences else "CHANGED",
                "exit_code": exit_code,
                "first_digest": canonical_digest(first_manifest),
                "format_version": FORMAT_VERSION,
                "refusal_reasons": [],
                "refusal_surfaces": [],
                "second_digest": canonical_digest(second_manifest),
            }
        ),
        exit_code,
    )


def main(arguments: list[str] | None = None) -> int:
    parsed = build_parser().parse_args(arguments)
    if parsed.command == "compare":
        try:
            report, exit_code = compare_reports(parsed.first, parsed.second)
        except (json.JSONDecodeError, OSError) as error:
            report = {
                "changed_surfaces": [],
                "comparison_status": "REFUSED",
                "exit_code": EXIT_BY_STATUS["REFUSED"],
                "format_version": FORMAT_VERSION,
                "refusal_reasons": [f"comparison_error:{error}"],
                "refusal_surfaces": [str(error)],
            }
            exit_code = EXIT_BY_STATUS["REFUSED"]
        write_report(report, parsed.output)
        return exit_code
    protected = {
        "account_id": parsed.protected_account_id,
        "character_guid": parsed.protected_character_guid,
        "character_name": parsed.protected_character_name,
    }
    expected = {
        "account_id": PROTECTED_ACCOUNT_ID,
        "character_guid": PROTECTED_CHARACTER_GUID,
        "character_name": PROTECTED_CHARACTER_NAME,
    }
    if protected != expected:
        report = {
            "capture_status": "REFUSED",
            "capture_times": [],
            "changed_surfaces": [],
            "counts": {},
            "digest": None,
            "exit_code": EXIT_BY_STATUS["REFUSED"],
            "first_digest": None,
            "format_version": FORMAT_VERSION,
            "manifest": None,
            "refusal_reasons": ["protected_binding_inexact"],
            "refusal_surfaces": ["protected_identity"],
            "second_digest": None,
            "status": "REFUSED",
            "zero_predicates": {},
        }
        write_report(report, parsed.output)
        return EXIT_BY_STATUS["REFUSED"]

    try:
        first, second = capture_live_pair(parsed)
        report = operational_report(evaluate_captures(first, second))
    except (
        AttributeError,
        KeyError,
        OperationalRefusal,
        TypeError,
        ValueError,
        json.JSONDecodeError,
        OSError,
    ) as error:
        report = {
            "capture_status": "REFUSED",
            "capture_times": [],
            "changed_surfaces": [],
            "counts": {},
            "digest": None,
            "exit_code": EXIT_BY_STATUS["REFUSED"],
            "first_digest": None,
            "format_version": FORMAT_VERSION,
            "manifest": None,
            "refusal_reasons": [f"capture_error:{error}"],
            "refusal_surfaces": [str(error)],
            "second_digest": None,
            "status": "REFUSED",
            "zero_predicates": {},
        }
        write_report(report, parsed.output)
        return EXIT_BY_STATUS["REFUSED"]
    write_report(report, parsed.output)
    return int(report["exit_code"])


if __name__ == "__main__":
    raise SystemExit(main())
