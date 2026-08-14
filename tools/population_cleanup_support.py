from __future__ import annotations

import hashlib
import json
from typing import Any

PROTECTED_ACCOUNT_ID = 157
PROTECTED_CHARACTER_GUID = 661
PROTECTED_CHARACTER_NAME = "Deszy"
STABLE_STATUSES = frozenset({"READY", "NOT_READY"})


class CleanupRefusal(RuntimeError):
    pass


class CleanupApplyFailure(RuntimeError):
    pass


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def canonical_digest(value: Any) -> str:
    rendered = json.dumps(
        value, ensure_ascii=False, separators=(",", ":"), sort_keys=True
    )
    return hashlib.sha256(rendered.encode("utf-8")).hexdigest()


def _manifest(report: dict[str, Any]) -> dict[str, Any]:
    manifest = report.get("manifest")
    if not isinstance(manifest, dict):
        raise CleanupRefusal("frozen_manifest_missing")
    return manifest


def validate_frozen_authority(report: dict[str, Any], expected_digest: str) -> None:
    manifest = _manifest(report)
    actual_digest = canonical_digest(manifest)
    if (
        report.get("capture_status") != "STABLE"
        or report.get("status") not in STABLE_STATUSES
        or report.get("exit_code") != 0
        or report.get("refusal_reasons") != []
        or report.get("refusal_surfaces") != []
    ):
        raise CleanupRefusal("frozen_report_not_stable")
    if (
        report.get("digest") != expected_digest
        or report.get("first_digest") != expected_digest
        or report.get("second_digest") != expected_digest
        or actual_digest != expected_digest
    ):
        raise CleanupRefusal("frozen_digest_mismatch")
    for key in ("unknown_edges", "unavailable_sources", "protected_overlap_surfaces"):
        if manifest.get(key) != []:
            raise CleanupRefusal(f"frozen_{key}")

    identities = manifest.get("identities")
    if not isinstance(identities, dict):
        raise CleanupRefusal("frozen_identities_missing")
    account_ids = _integer_list(identities, "account_ids")
    character_guids = _integer_list(identities, "character_guids")
    if (
        PROTECTED_ACCOUNT_ID in account_ids
        or PROTECTED_CHARACTER_GUID in character_guids
    ):
        raise CleanupRefusal("protected_target_overlap")

    baseline = manifest.get("protected_baseline")
    if not isinstance(baseline, dict):
        raise CleanupRefusal("protected_baseline_missing")
    if baseline.get("accounts") != [
        {"account_id": PROTECTED_ACCOUNT_ID, "username": "FUITAD"}
    ]:
        raise CleanupRefusal("protected_account_mismatch")
    if baseline.get("characters") != [
        {
            "account_id": PROTECTED_ACCOUNT_ID,
            "character_guid": PROTECTED_CHARACTER_GUID,
            "delete_account_id": None,
            "name": PROTECTED_CHARACTER_NAME,
        }
    ]:
        raise CleanupRefusal("protected_character_mismatch")
    if baseline.get("ownership") != []:
        raise CleanupRefusal("protected_ownership_overlap")


def _integer_list(container: dict[str, Any], key: str) -> list[int]:
    values = container.get(key)
    if not isinstance(values, list) or any(
        isinstance(value, bool) or not isinstance(value, int) for value in values
    ):
        raise CleanupRefusal(f"invalid_target_list:{key}")
    if len(values) != len(set(values)):
        raise CleanupRefusal(f"duplicate_target_list:{key}")
    return sorted(values)


def validate_settlement_boundary(report: dict[str, Any]) -> None:
    identities = _manifest(report).get("identities")
    if not isinstance(identities, dict):
        raise CleanupRefusal("frozen_identities_missing")
    account_ids = set(_integer_list(identities, "account_ids"))
    character_guids = set(_integer_list(identities, "character_guids"))

    for auction in identities.get("auctions", []):
        owner_guid = auction.get("owner_guid")
        owner_account_id = auction.get("owner_account_id")
        bidder_guid = auction.get("bidder_guid")
        bidder_account_id = auction.get("bidder_account_id")
        if owner_guid not in character_guids or owner_account_id not in account_ids:
            raise CleanupRefusal("external_auction_owner")
        if bidder_guid not in (None, 0) and bidder_guid not in character_guids:
            raise CleanupRefusal("external_auction_bidder")
        if bidder_account_id is not None and bidder_account_id not in account_ids:
            raise CleanupRefusal("external_auction_bidder")

    for mail in identities.get("mail", []):
        sender_account_id = mail.get("sender_account_id")
        receiver_account_id = mail.get("receiver_account_id")
        sender_guid = mail.get("sender_guid")
        receiver_guid = mail.get("receiver_guid")
        external_sender = (
            sender_account_id is not None and sender_account_id not in account_ids
        )
        external_receiver = (
            receiver_account_id is not None and receiver_account_id not in account_ids
        )
        player_sender = (
            mail.get("message_type") == 0 and sender_guid not in character_guids
        )
        player_receiver = receiver_guid not in character_guids
        if external_sender or external_receiver or player_sender or player_receiver:
            raise CleanupRefusal("external_mail_participant")


def _derived_ids(identities: dict[str, Any], collection: str, field: str) -> list[int]:
    derived = identities.get("derived")
    if not isinstance(derived, dict):
        raise CleanupRefusal("derived_identities_missing")
    rows = derived.get(collection, [])
    if not isinstance(rows, list):
        raise CleanupRefusal(f"invalid_derived_identity:{collection}")
    values = [row[field] for row in rows]
    if any(isinstance(value, bool) or not isinstance(value, int) for value in values):
        raise CleanupRefusal(f"invalid_derived_identity:{collection}.{field}")
    return sorted(set(values))


def _identity_ids(identities: dict[str, Any], collection: str, field: str) -> list[int]:
    rows = identities.get(collection, [])
    if not isinstance(rows, list):
        raise CleanupRefusal(f"invalid_identity:{collection}")
    values = [row[field] for row in rows]
    if any(isinstance(value, bool) or not isinstance(value, int) for value in values):
        raise CleanupRefusal(f"invalid_identity:{collection}.{field}")
    return sorted(set(values))


def build_cleanup_plan(
    report: dict[str, Any],
    operation_record: dict[str, Any],
    effects: list[dict[str, Any]],
    *,
    expected_frozen_digest: str,
) -> dict[str, Any]:
    validate_frozen_authority(report, expected_frozen_digest)
    validate_settlement_boundary(report)
    identities = _manifest(report)["identities"]
    backup = operation_record.get("backup")
    if not isinstance(backup, dict):
        raise CleanupRefusal("backup_identity_missing")
    mysql = backup.get("mysql")
    redis = backup.get("redis")
    if not isinstance(mysql, dict) or not isinstance(redis, dict):
        raise CleanupRefusal("backup_identity_missing")

    plan: dict[str, Any] = {
        "archive": {
            "authority": operation_record.get("operation"),
            "frozen_digest": expected_frozen_digest,
            "mysql_path": mysql.get("path"),
            "mysql_sha256": mysql.get("sha256"),
            "redis_path": redis.get("path"),
            "redis_sha256": redis.get("sha256"),
            "transition": "verified_backup_is_immutable_legacy_epoch",
        },
        "backup_status": operation_record.get("status"),
        "effects": effects,
        "format_version": 1,
        "protected_controls": {
            "account_id": PROTECTED_ACCOUNT_ID,
            "character_guid": PROTECTED_CHARACTER_GUID,
            "character_name": PROTECTED_CHARACTER_NAME,
            "expected_online_state": operation_record.get("protected_online_state"),
        },
        "source_evidence": _manifest(report).get("provenance"),
        "targets": {
            "account_ids": _integer_list(identities, "account_ids"),
            "auction_ids": _identity_ids(identities, "auctions", "auction_id"),
            "character_guids": _integer_list(identities, "character_guids"),
            "item_guids": _integer_list(identities, "item_guids"),
            "mail_ids": _identity_ids(identities, "mail", "mail_id"),
            "medivh_observed_bot_ids": _derived_ids(
                identities, "medivh_observed_bots", "id"
            ),
            "social_actor_ids": _derived_ids(identities, "social_actors", "actor_id"),
        },
        "writer_boundary": operation_record.get("final_services"),
    }
    plan["plan_digest"] = plan_digest(plan)
    return plan


def plan_digest(plan: dict[str, Any]) -> str:
    unsigned = {key: value for key, value in plan.items() if key != "plan_digest"}
    return canonical_digest(unsigned)


def validate_exact_plan(plan: dict[str, Any]) -> None:
    digest = plan.get("plan_digest")
    if not isinstance(digest, str) or digest != plan_digest(plan):
        raise CleanupRefusal("plan_digest_mismatch")
    targets = plan.get("targets")
    if not isinstance(targets, dict):
        raise CleanupRefusal("plan_targets_missing")
    for key, values in targets.items():
        if not isinstance(values, list) or values != sorted(set(values)):
            raise CleanupRefusal(f"plan_targets_noncanonical:{key}")
    if PROTECTED_ACCOUNT_ID in targets.get("account_ids", []):
        raise CleanupRefusal("protected_target_overlap")
    if PROTECTED_CHARACTER_GUID in targets.get("character_guids", []):
        raise CleanupRefusal("protected_target_overlap")


def require_affected_rows(surface: str, expected: int, actual: int) -> None:
    if actual != expected:
        raise CleanupApplyFailure(
            f"affected_rows_mismatch:{surface}:expected={expected}:actual={actual}"
        )


def legacy_social_event_deletion_count(total_events: int, shared_events: int) -> int:
    if total_events < 0 or shared_events < 0 or shared_events > total_events:
        raise CleanupRefusal("invalid_shared_legacy_social_event_count")
    return total_events


def validate_protected_reconciliation(
    frozen_manifest: dict[str, Any], post_manifest: dict[str, Any]
) -> None:
    frozen_baseline = frozen_manifest.get("protected_baseline")
    post_baseline = post_manifest.get("protected_baseline")
    if not isinstance(frozen_baseline, dict) or not isinstance(post_baseline, dict):
        raise CleanupApplyFailure("protected_baseline_missing")
    for key in ("accounts", "characters", "ownership"):
        if post_baseline.get(key) != frozen_baseline.get(key):
            raise CleanupApplyFailure(f"protected_{key}_changed")

    frozen_references = frozen_manifest.get("reference_surfaces", {})
    post_references = post_manifest.get("reference_surfaces", {})
    if set(frozen_references) != set(post_references):
        raise CleanupApplyFailure("protected_reference_surface_set_changed")
    classified = set(frozen_references)
    frozen_surfaces = frozen_baseline.get("surfaces", {})
    post_surfaces = post_baseline.get("surfaces", {})
    if set(frozen_surfaces) != set(post_surfaces):
        raise CleanupApplyFailure("protected_surface_set_changed")
    for surface, frozen_value in frozen_surfaces.items():
        if surface in classified:
            continue
        if post_surfaces.get(surface) != frozen_value:
            raise CleanupApplyFailure(f"protected_surface_changed:{surface}")

    for surface, frozen_value in frozen_references.items():
        frozen_protected = frozen_value.get("protected", {})
        post_protected = post_references[surface].get("protected", {})
        if post_protected.get("owned") != frozen_protected.get("owned"):
            raise CleanupApplyFailure(f"protected_owned_reference_changed:{surface}")
