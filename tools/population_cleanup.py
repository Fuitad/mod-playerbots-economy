from __future__ import annotations

import argparse
import hashlib
import json
import os
import plistlib
import re
import subprocess
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from population_cleanup_cli import run_cli
from population_cleanup_runtime import (
    apply_mysql,
    apply_redis,
    restore_mysql,
    restore_redis,
)
from population_cleanup_support import (
    CleanupApplyFailure,
    CleanupRefusal,
    build_cleanup_plan,
    canonical_json,
    legacy_social_event_deletion_count,
    validate_exact_plan,
    validate_frozen_authority,
    validate_protected_reconciliation,
    validate_settlement_boundary,
)

MYSQL_SCHEMAS = ("acore_auth", "acore_characters", "acore_playerbots", "medivh")
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
EXPECTED_OPERATION_STATUS = "VERIFIED_WRITERS_STOPPED"
EXPECTED_COMPARE_STATUS = "IDENTICAL"
PROTECTED_ACCOUNT_ID = 157
PROTECTED_CHARACTER_GUID = 661
AUDIT_FILENAMES = (
    "population_backup.py",
    "population_backup_support.py",
    "population_manifest.py",
    "population_manifest_live.py",
    "population_manifest_projections.py",
    "population_manifest_inventory.json",
)
SOCIAL_SPECIAL_TABLES = frozenset(
    {
        "playerbot_social_event",
        "playerbot_social_memory",
        "playerbot_social_moderation_case",
        "playerbot_social_profile",
        "playerbot_social_relationship",
    }
)
GUILD_SPECIAL_TABLES = frozenset(
    {
        "guild",
        "guild_bank_eventlog",
        "guild_bank_item",
        "guild_bank_right",
        "guild_bank_tab",
        "guild_eventlog",
        "guild_member",
        "guild_rank",
    }
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise CleanupRefusal(f"json_object_required:{path}")
    return value


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    stdin: str | None = None,
    accepted: frozenset[int] = frozenset({0}),
) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        capture_output=True,
        input=stdin,
        text=True,
    )
    if completed.returncode not in accepted:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise CleanupRefusal(f"command_failed:{command[0]}:{detail}")
    return completed


def mysql_command(suffix: str) -> list[str]:
    if re.fullmatch(r"[A-Za-z0-9_]+", suffix) is None:
        raise CleanupRefusal("invalid_mysql_defaults_group_suffix")
    return [
        "mysql",
        f"--defaults-group-suffix={suffix}",
        "--batch",
        "--raw",
        "--skip-column-names",
    ]


def mysql_query(suffix: str, sql: str) -> str:
    return run(mysql_command(suffix), stdin=sql).stdout


def identifier(value: str) -> str:
    if IDENTIFIER.fullmatch(value) is None:
        raise CleanupRefusal(f"invalid_identifier:{value}")
    return f"`{value}`"


def integer_literals(values: list[int]) -> str:
    if not values:
        return "NULL"
    if values != sorted(set(values)) or any(
        isinstance(value, bool) or not isinstance(value, int) for value in values
    ):
        raise CleanupRefusal("noncanonical_integer_targets")
    return ",".join(str(value) for value in values)


def string_literals(values: list[str]) -> str:
    if not values:
        return "NULL"
    if values != sorted(set(values)) or any(
        not isinstance(value, str) for value in values
    ):
        raise CleanupRefusal("noncanonical_string_targets")
    return ",".join("'" + value.replace("'", "''") + "'" for value in values)


def ensure_output_directory(path: Path) -> Path:
    path = path.resolve()
    path.mkdir(parents=True, exist_ok=False)
    return path


def write_durable(path: Path, value: dict[str, Any]) -> None:
    temporary = path.with_name(f".{path.name}.tmp")
    with temporary.open("w", encoding="utf-8") as handle:
        handle.write(canonical_json(value))
        handle.flush()
        os.fsync(handle.fileno())
    temporary.replace(path)
    directory = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def verify_evidence_hashes(operation: Path, record: dict[str, Any]) -> None:
    sums = operation / "SHA256SUMS"
    if not sums.is_file():
        raise CleanupRefusal("evidence_sums_missing")
    seen: dict[str, str] = {}
    for line in sums.read_text(encoding="utf-8").splitlines():
        digest, separator, relative = line.partition("  ")
        if separator != "  " or not relative or relative in seen:
            raise CleanupRefusal("evidence_sums_malformed")
        path = operation / relative
        if not path.is_file() or sha256_file(path) != digest:
            raise CleanupRefusal(f"evidence_hash_mismatch:{relative}")
        seen[relative] = digest
    if seen != record.get("evidence_sha256"):
        raise CleanupRefusal("evidence_hash_index_mismatch")


def verify_backup_authority(
    operation_record: Path, expected_digest: str
) -> dict[str, Any]:
    operation = operation_record.resolve().parent
    record = load_json(operation_record)
    if record.get("operation") != str(operation):
        raise CleanupRefusal("operation_path_mismatch")
    if record.get("status") != EXPECTED_OPERATION_STATUS:
        raise CleanupRefusal("operation_status_mismatch")
    verify_evidence_hashes(operation, record)
    for kind in ("mysql", "redis"):
        backup = record.get("backup", {}).get(kind, {})
        path = Path(str(backup.get("path", ""))).resolve()
        if path.parent != operation or not path.is_file():
            raise CleanupRefusal(f"backup_path_mismatch:{kind}")
        if sha256_file(path) != backup.get("sha256"):
            raise CleanupRefusal(f"backup_hash_mismatch:{kind}")
    frozen = load_json(operation / "frozen-manifest.json")
    restored = load_json(operation / "restored-manifest.json")
    comparison = load_json(operation / "restore-comparison.json")
    validate_frozen_authority(frozen, expected_digest)
    validate_frozen_authority(restored, expected_digest)
    if comparison.get("comparison_status") != EXPECTED_COMPARE_STATUS:
        raise CleanupRefusal("restore_comparison_mismatch")
    return record


def verify_repositories_and_artifacts(record: dict[str, Any], audit_root: Path) -> None:
    for repository in record.get("repositories", []):
        path = Path(repository["path"])
        if (
            run(["git", "-C", str(path), "rev-parse", "HEAD"]).stdout.strip()
            != repository["revision"]
        ):
            raise CleanupRefusal(f"repository_revision_mismatch:{path}")
        current = run(
            [
                "git",
                "-C",
                str(path),
                "status",
                "--porcelain=v1",
                "--untracked-files=all",
            ]
        ).stdout.splitlines()
        if current != repository["status"]:
            raise CleanupRefusal(f"repository_status_mismatch:{path}")

    expected_audit = {
        Path(artifact["path"]).name: artifact["sha256"]
        for artifact in record.get("artifacts", [])
        if Path(artifact["path"]).name in AUDIT_FILENAMES
    }
    if set(expected_audit) != set(AUDIT_FILENAMES):
        raise CleanupRefusal("audit_artifact_authority_incomplete")
    for name, expected_hash in expected_audit.items():
        path = audit_root / "tools" / name
        if not path.is_file() or sha256_file(path) != expected_hash:
            raise CleanupRefusal(f"audit_artifact_hash_mismatch:{name}")

    for artifact in record.get("artifacts", []):
        path = Path(artifact["path"])
        if path.name in AUDIT_FILENAMES:
            continue
        if not path.is_file() or sha256_file(path) != artifact["sha256"]:
            raise CleanupRefusal(f"runtime_artifact_hash_mismatch:{path}")


def verify_service_boundary(record: dict[str, Any]) -> None:
    uid = os.getuid()
    final = record.get("final_services", {})
    services = record.get("services", {})
    for label in final.get("stopped", []):
        completed = run(
            ["launchctl", "print", f"gui/{uid}/{label}"],
            accepted=frozenset(range(256)),
        )
        if completed.returncode == 0:
            raise CleanupRefusal(f"writer_service_reappeared:{label}")
    for label in final.get("running", []):
        service = services.get(label, {})
        plist_path = Path(str(service.get("plist", ""))).resolve()
        if not plist_path.is_file() or sha256_file(plist_path) != service.get(
            "plist_sha256"
        ):
            raise CleanupRefusal(f"store_plist_mismatch:{label}")
        completed = run(["launchctl", "print", f"gui/{uid}/{label}"])
        if "state = running" not in completed.stdout:
            raise CleanupRefusal(f"store_not_running:{label}")
        with plist_path.open("rb") as handle:
            plist = plistlib.load(handle)
        if plist.get("Label") != label:
            raise CleanupRefusal(f"store_plist_label_mismatch:{label}")
    medivh_root = Path(record["inputs"]["medivh_root"])
    if (
        final.get("medivh_maintenance") is not True
        or not (medivh_root / "storage/framework/down").is_file()
    ):
        raise CleanupRefusal("medivh_maintenance_missing")


def protected_online_state(suffix: str) -> list[dict[str, Any]]:
    output = mysql_query(
        suffix,
        "SELECT a.id,a.online,c.guid,c.online,c.deleteInfos_Account,c.name "
        "FROM acore_auth.account a JOIN acore_characters.characters c ON c.account=a.id "
        "WHERE a.id=157 AND c.guid=661;\n",
    )
    rows: list[dict[str, Any]] = []
    for line in output.splitlines():
        account, account_online, guid, character_online, deleted, name = line.split(
            "\t"
        )
        rows.append(
            {
                "account_id": int(account),
                "account_online": int(account_online),
                "character_guid": int(guid),
                "character_online": int(character_online),
                "delete_account_id": None if deleted == "NULL" else int(deleted),
                "name": name,
            }
        )
    return rows


def capture_audit(
    record: dict[str, Any], audit_root: Path, output: Path
) -> dict[str, Any]:
    inputs = record["inputs"]
    command = [
        sys.executable,
        str(audit_root / "tools" / "population_manifest.py"),
        "live",
        "--playerbots-config",
        inputs["playerbots_config"],
        "--protected-account-id",
        str(PROTECTED_ACCOUNT_ID),
        "--protected-character-guid",
        str(PROTECTED_CHARACTER_GUID),
        "--protected-character-name",
        "Deszy",
        "--inventory",
        str(audit_root / "tools" / "population_manifest_inventory.json"),
        "--mysql-defaults-group-suffix",
        inputs["mysql_defaults_group_suffix"],
        "--azerothcore-root",
        inputs["azerothcore_root"],
        "--medivh-root",
        inputs["medivh_root"],
        "--redis-host",
        inputs["redis_host"],
        "--redis-port",
        str(inputs["redis_port"]),
        "--output",
        str(output),
    ]
    completed = run(command, accepted=frozenset({0, 3, 4}))
    output.with_suffix(".stdout").write_text(completed.stdout, encoding="utf-8")
    report = load_json(output)
    if completed.returncode != report.get("exit_code"):
        raise CleanupRefusal("audit_exit_status_mismatch")
    return report


def verify_frozen_live_pair(
    record: dict[str, Any],
    audit_root: Path,
    evidence: Path,
    expected_digest: str,
    prefix: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    first = capture_audit(record, audit_root, evidence / f"{prefix}-1.json")
    second = capture_audit(record, audit_root, evidence / f"{prefix}-2.json")
    validate_frozen_authority(first, expected_digest)
    validate_frozen_authority(second, expected_digest)
    if first["manifest"] != second["manifest"]:
        raise CleanupRefusal("fresh_frozen_manifest_mismatch")
    return first, second


def live_gate(
    operation_record: Path,
    audit_root: Path,
    evidence: Path,
    expected_digest: str,
    prefix: str,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    record = verify_backup_authority(operation_record, expected_digest)
    verify_repositories_and_artifacts(record, audit_root)
    verify_service_boundary(record)
    suffix = record["inputs"]["mysql_defaults_group_suffix"]
    if protected_online_state(suffix) != record.get("protected_online_state"):
        raise CleanupRefusal("protected_online_state_mismatch")
    first, second = verify_frozen_live_pair(
        record, audit_root, evidence, expected_digest, prefix
    )
    return record, first, second


def load_inventory(audit_root: Path) -> dict[str, Any]:
    inventory = load_json(audit_root / "tools" / "population_manifest_inventory.json")
    if inventory.get("version") != 2:
        raise CleanupRefusal("unsupported_inventory_version")
    return inventory


def expand_edges(inventory: dict[str, Any]) -> list[dict[str, Any]]:
    expanded: list[dict[str, Any]] = []
    for edge in inventory["edges"]:
        if "table" in edge:
            expanded.append(dict(edge))
            continue
        for table, columns in edge["tables"].items():
            expanded.append(
                {
                    "key": f"{edge['key']}.{table}",
                    "schema": edge["schema"],
                    "table": table,
                    "selectors": [{"kind": edge["selector_kind"], "columns": columns}],
                    "source": edge["source"],
                }
            )
    return expanded


def read_table_metadata(suffix: str) -> dict[tuple[str, str], dict[str, Any]]:
    schemas = ",".join(f"'{schema}'" for schema in MYSQL_SCHEMAS)
    output = mysql_query(
        suffix,
        "SELECT t.TABLE_SCHEMA,t.TABLE_NAME,COALESCE(t.ENGINE,'NULL'),c.COLUMN_NAME,c.COLUMN_KEY "
        "FROM information_schema.TABLES t JOIN information_schema.COLUMNS c "
        "ON c.TABLE_SCHEMA=t.TABLE_SCHEMA AND c.TABLE_NAME=t.TABLE_NAME "
        f"WHERE t.TABLE_SCHEMA IN ({schemas}) ORDER BY t.TABLE_SCHEMA,t.TABLE_NAME,c.ORDINAL_POSITION;\n",
    )
    metadata: dict[tuple[str, str], dict[str, Any]] = {}
    for line in output.splitlines():
        schema, table, engine, column, key = line.split("\t")
        entry = metadata.setdefault(
            (schema, table), {"columns": [], "engine": engine, "primary_key": []}
        )
        entry["columns"].append(column)
        if key == "PRI":
            entry["primary_key"].append(column)
    return metadata


def discover_derived_targets(
    suffix: str, targets: dict[str, Any]
) -> dict[str, list[int]]:
    characters = integer_literals(targets["character_guids"])
    actors = integer_literals(targets["social_actor_ids"])
    output = mysql_query(
        suffix,
        "SELECT CONCAT('pet\\t',id) FROM acore_characters.character_pet "
        f"WHERE owner IN ({characters}) ORDER BY id;"
        "SELECT CONCAT('guild\\t',guildid) FROM acore_characters.guild_member "
        f"WHERE guid IN ({characters}) GROUP BY guildid ORDER BY guildid;"
        "SELECT CONCAT('mixed_guild\\t',gm.guildid) FROM acore_characters.guild_member gm "
        f"WHERE gm.guildid IN (SELECT guildid FROM acore_characters.guild_member WHERE guid IN ({characters})) "
        f"AND gm.guid NOT IN ({characters}) GROUP BY gm.guildid ORDER BY gm.guildid;"
        "SELECT CONCAT('guild_item\\t',item_guid) FROM acore_characters.guild_bank_item "
        f"WHERE guildid IN (SELECT guildid FROM acore_characters.guild_member WHERE guid IN ({characters})) "
        "ORDER BY item_guid;"
        "SELECT CONCAT('position\\t',public_id) FROM acore_playerbots.playerbot_economy_position "
        f"WHERE trader_guid IN ({characters}) ORDER BY public_id;"
        "SELECT CONCAT('external_event_actor\\t',id) FROM acore_playerbots.playerbot_social_actor "
        f"WHERE id NOT IN ({actors}) AND id IN ("
        "SELECT actor_id FROM acore_playerbots.playerbot_social_event "
        f"WHERE actor_id IN ({actors}) OR target_actor_id IN ({actors}) OR bot_actor_id IN ({actors}) "
        "UNION SELECT target_actor_id FROM acore_playerbots.playerbot_social_event "
        f"WHERE actor_id IN ({actors}) OR target_actor_id IN ({actors}) OR bot_actor_id IN ({actors}) "
        "UNION SELECT bot_actor_id FROM acore_playerbots.playerbot_social_event "
        f"WHERE actor_id IN ({actors}) OR target_actor_id IN ({actors}) OR bot_actor_id IN ({actors})) "
        "ORDER BY id;"
        "SELECT CONCAT('protected_shared_event_count\\t',COUNT(*)) "
        "FROM acore_playerbots.playerbot_social_event e "
        "WHERE (e.actor_id IN (SELECT id FROM acore_playerbots.playerbot_social_actor WHERE character_guid=661) "
        "OR e.target_actor_id IN (SELECT id FROM acore_playerbots.playerbot_social_actor WHERE character_guid=661) "
        "OR e.bot_actor_id IN (SELECT id FROM acore_playerbots.playerbot_social_actor WHERE character_guid=661)) "
        f"AND (e.actor_id IN ({actors}) OR e.target_actor_id IN ({actors}) OR e.bot_actor_id IN ({actors}));",
    )
    derived: dict[str, list[int]] = defaultdict(list)
    for line in output.splitlines():
        kind, value = line.split("\t")
        derived[kind].append(int(value))
    if derived["mixed_guild"]:
        raise CleanupRefusal("mixed_guild_membership")
    return {key: sorted(set(values)) for key, values in sorted(derived.items())}


def selector_predicate(
    selector: dict[str, Any], targets: dict[str, Any], alias: str = "t"
) -> str:
    values = {
        "account": targets["account_ids"],
        "account_name": targets["account_names"],
        "auction": targets["auction_ids"],
        "character": targets["character_guids"],
        "economy_position": targets["economy_position_ids"],
        "item": targets["all_item_guids"],
        "mail": targets["mail_ids"],
        "pet": targets["pet_ids"],
        "social_actor": targets["social_actor_ids"],
    }
    kind = selector["kind"]
    if kind not in values:
        raise CleanupRefusal(f"unknown_selector_kind:{kind}")
    literals = (
        string_literals(values[kind])
        if kind in {"account_name", "economy_position"}
        else integer_literals(values[kind])
    )
    return (
        "("
        + " OR ".join(
            f"{alias}.{identifier(column)} IN ({literals})"
            for column in selector["columns"]
        )
        + ")"
    )


def edge_predicate(edge: dict[str, Any], targets: dict[str, Any]) -> str:
    selectors = edge.get("selectors", [])
    if edge.get("cleanup_behavior") == "delete_owned":
        selectors = [
            selector for selector in selectors if selector.get("role") == "owned"
        ]
    conditions = [selector_predicate(selector, targets) for selector in selectors]
    for column, values in edge.get("static_values", {}).items():
        conditions.append(
            f"t.{identifier(column)} IN ({string_literals(sorted(values))})"
        )
    if edge.get("capture_all"):
        return "1=1"
    if not conditions:
        raise CleanupRefusal(f"edge_without_cleanup_selector:{edge['key']}")
    return " OR ".join(conditions)


def phase_for(schema: str, table: str) -> int:
    if schema == "medivh":
        return 10
    if table == "playerbot_social_actor":
        return 25
    if table.startswith("playerbot_social_"):
        return 20
    if table == "playerbot_economy_position":
        return 25
    if table.startswith(("playerbot_economy_", "playerbot_llm_")):
        return 20
    if table in {"auctionhouse", "mail_items", "mail"}:
        return 30
    guild_phases = {
        "guild_member": 39,
        "guild": 40,
        "guild_rank": 41,
        "guild_bank_tab": 42,
        "guild_bank_item": 44,
        "guild_bank_right": 45,
        "guild_bank_eventlog": 46,
        "guild_eventlog": 47,
    }
    if table in guild_phases:
        return guild_phases[table]
    if table.startswith("pet_") or table == "character_pet_declinedname":
        return 50
    if table in {"character_pet", "item_instance", "characters"}:
        return 65 if table == "characters" else 60
    if schema == "acore_characters":
        return 55
    if schema == "acore_playerbots" and table.startswith("playerbots_account_"):
        return 75
    if schema == "acore_playerbots":
        return 25
    if schema == "acore_auth" and table == "account":
        return 90
    if schema == "acore_auth":
        return 80
    return 70


def special_surface_specs(
    targets: dict[str, Any], inventory: dict[str, Any]
) -> list[dict[str, Any]]:
    actors = integer_literals(targets["social_actor_ids"])
    guilds = integer_literals(targets["guild_ids"])
    guild_items = integer_literals(targets["guild_item_guids"])
    specs = [
        {
            "action": "delete",
            "key": "archive.playerbot_social_event",
            "predicate": (
                f"t.`actor_id` IN ({actors}) "
                f"OR t.`target_actor_id` IN ({actors}) "
                f"OR t.`bot_actor_id` IN ({actors})"
            ),
            "schema": "acore_playerbots",
            "source": "social_cleanup",
            "table": "playerbot_social_event",
        },
        {
            "action": "delete",
            "key": "playerbots.social_memory_owned",
            "predicate": f"t.`bot_actor_id` IN ({actors})",
            "schema": "acore_playerbots",
            "source": "social_cleanup",
            "table": "playerbot_social_memory",
        },
        {
            "action": "retain",
            "key": "playerbots.social_memory_survivor_owned",
            "predicate": f"t.`subject_actor_id` IN ({actors}) AND t.`bot_actor_id` NOT IN ({actors})",
            "schema": "acore_playerbots",
            "source": "social_cleanup",
            "table": "playerbot_social_memory",
        },
        {
            "action": "retain",
            "key": "playerbots.social_moderation_audit",
            "predicate": f"t.`subject_actor_id` IN ({actors})",
            "schema": "acore_playerbots",
            "source": "social_cleanup",
            "table": "playerbot_social_moderation_case",
        },
        {
            "action": "delete",
            "key": "playerbots.social_profile_owned",
            "predicate": f"t.`bot_actor_id` IN ({actors})",
            "schema": "acore_playerbots",
            "source": "social_cleanup",
            "table": "playerbot_social_profile",
        },
        {
            "action": "delete",
            "key": "playerbots.social_relationship_owned",
            "predicate": f"t.`bot_actor_id` IN ({actors})",
            "schema": "acore_playerbots",
            "source": "social_cleanup",
            "table": "playerbot_social_relationship",
        },
        {
            "action": "retain",
            "key": "playerbots.social_relationship_survivor_owned",
            "predicate": f"t.`subject_actor_id` IN ({actors}) AND t.`bot_actor_id` NOT IN ({actors})",
            "schema": "acore_playerbots",
            "source": "social_cleanup",
            "table": "playerbot_social_relationship",
        },
    ]
    for table in sorted(GUILD_SPECIAL_TABLES):
        specs.append(
            {
                "action": "delete",
                "key": f"characters.target_owned_guilds.{table}",
                "predicate": f"t.`guildid` IN ({guilds})",
                "schema": "acore_characters",
                "source": "character_queries",
                "table": table,
            }
        )
    specs.append(
        {
            "action": "delete",
            "key": "characters.target_owned_guilds.item_instance",
            "predicate": f"t.`guid` IN ({guild_items})",
            "schema": "acore_characters",
            "source": "character_queries",
            "table": "item_instance",
        }
    )
    for key in inventory["projection_keys"]["medivh_cache"]:
        specs.append(
            {
                "action": "delete",
                "key": f"medivh.cache.{key}",
                "predicate": f"t.`key` IN ({string_literals([key])})",
                "schema": "medivh",
                "source": "medivh_cache",
                "table": "cache",
            }
        )
    return specs


def compile_surface_specs(
    inventory: dict[str, Any], targets: dict[str, Any]
) -> list[dict[str, Any]]:
    specs: list[dict[str, Any]] = []
    for edge in expand_edges(inventory):
        table = edge["table"]
        if table in SOCIAL_SPECIAL_TABLES or table in GUILD_SPECIAL_TABLES:
            continue
        if edge["key"] == "auth.account.recruiter":
            specs.append(
                {
                    "action": "update_zero",
                    "key": edge["key"],
                    "predicate": edge_predicate(edge, targets),
                    "schema": edge["schema"],
                    "source": edge["source"],
                    "table": table,
                    "update_column": "recruiter",
                }
            )
            continue
        specs.append(
            {
                "action": "delete",
                "key": edge["key"],
                "predicate": edge_predicate(edge, targets),
                "schema": edge["schema"],
                "source": edge["source"],
                "table": table,
            }
        )
    specs.extend(special_surface_specs(targets, inventory))
    return specs


def group_mutations(
    specs: list[dict[str, Any]], metadata: dict[tuple[str, str], dict[str, Any]]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    retained = [spec for spec in specs if spec["action"] == "retain"]
    grouped: dict[tuple[str, str, str, str | None], list[dict[str, Any]]] = defaultdict(
        list
    )
    for spec in specs:
        if spec["action"] == "retain":
            continue
        key = (
            spec["schema"],
            spec["table"],
            spec["action"],
            spec.get("update_column"),
        )
        grouped[key].append(spec)
    effects: list[dict[str, Any]] = []
    for (schema, table, action, update_column), members in grouped.items():
        table_metadata = metadata.get((schema, table))
        if table_metadata is None:
            raise CleanupRefusal(f"table_missing:{schema}.{table}")
        predicates = sorted({member["predicate"] for member in members})
        effect = {
            "action": action,
            "engine": table_metadata["engine"],
            "phase": phase_for(schema, table),
            "predicate": " OR ".join(f"({predicate})" for predicate in predicates),
            "schema": schema,
            "source": sorted({member["source"] for member in members}),
            "surfaces": sorted(member["key"] for member in members),
            "table": table,
        }
        if update_column is not None:
            effect["update_column"] = update_column
        effects.append(effect)
    return sorted(
        effects,
        key=lambda effect: (
            effect["phase"],
            effect["schema"],
            effect["table"] == "characters",
            effect["table"] == "account",
            effect["table"],
        ),
    ), retained


def count_effects(suffix: str, effects: list[dict[str, Any]]) -> None:
    statements = []
    for index, effect in enumerate(effects):
        statements.append(
            f"SELECT CONCAT('{index}\\t',COUNT(*)) FROM {identifier(effect['schema'])}."
            f"{identifier(effect['table'])} t WHERE {effect['predicate']};"
        )
    output = mysql_query(suffix, "".join(statements))
    counts = {}
    for line in output.splitlines():
        index, count = line.split("\t")
        counts[int(index)] = int(count)
    if set(counts) != set(range(len(effects))):
        raise CleanupRefusal("effect_count_output_incomplete")
    for index, effect in enumerate(effects):
        effect["expected_rows"] = counts[index]


def reconcile_mysql_effects(
    suffix: str,
    effects: list[dict[str, Any]],
    retained: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    probes = [dict(effect) for effect in effects + retained]
    count_effects(suffix, probes)
    results: list[dict[str, Any]] = []
    for index, effect in enumerate(effects):
        remaining = probes[index]["expected_rows"]
        if remaining != 0:
            raise CleanupApplyFailure(
                f"post_cleanup_rows_remain:{effect['schema']}.{effect['table']}:{remaining}"
            )
        results.append(
            {
                "remaining_rows": 0,
                "schema": effect["schema"],
                "surfaces": effect["surfaces"],
                "table": effect["table"],
            }
        )
    offset = len(effects)
    for index, effect in enumerate(retained):
        actual = probes[offset + index]["expected_rows"]
        if actual != effect["expected_rows"]:
            raise CleanupApplyFailure(
                f"retained_surface_changed:{effect['key']}:"
                f"expected={effect['expected_rows']}:actual={actual}"
            )
        results.append(
            {
                "retained_rows": actual,
                "schema": effect["schema"],
                "surfaces": [effect["key"]],
                "table": effect["table"],
            }
        )
    return results


def redis_effects(
    record: dict[str, Any], inventory: dict[str, Any]
) -> list[dict[str, Any]]:
    inputs = record["inputs"]
    keys = sorted(
        inventory["projection_keys"]["redis_strings"]
        + inventory["projection_keys"]["redis_streams"]
    )
    effects = []
    for key in keys:
        output = run(
            [
                "redis-cli",
                "-h",
                inputs["redis_host"],
                "-p",
                str(inputs["redis_port"]),
                "--raw",
                "EXISTS",
                key,
            ]
        ).stdout.strip()
        if output not in {"0", "1"}:
            raise CleanupRefusal(f"redis_exists_invalid:{key}")
        effects.append({"action": "delete", "expected_exists": int(output), "key": key})
    return effects


def redis_rollback_identity(record: dict[str, Any]) -> dict[str, str]:
    inputs = record["inputs"]
    output = run(
        [
            "redis-cli",
            "-h",
            inputs["redis_host"],
            "-p",
            str(inputs["redis_port"]),
            "--raw",
            "CONFIG",
            "GET",
            "dir",
            "dbfilename",
            "appendonly",
        ]
    ).stdout.splitlines()
    if len(output) % 2 != 0:
        raise CleanupRefusal("redis_rollback_identity_malformed")
    identity = dict(zip(output[::2], output[1::2], strict=True))
    if set(identity) != {"appendonly", "dbfilename", "dir"}:
        raise CleanupRefusal("redis_rollback_identity_incomplete")
    directory = Path(identity["dir"])
    filename = Path(identity["dbfilename"])
    if not directory.is_absolute() or filename.name != identity["dbfilename"]:
        raise CleanupRefusal("redis_rollback_path_unsafe")
    if identity["appendonly"] != "no":
        raise CleanupRefusal("redis_appendonly_restore_unsupported")
    return identity


def compile_plan(
    report: dict[str, Any],
    record: dict[str, Any],
    audit_root: Path,
    expected_digest: str,
) -> dict[str, Any]:
    validate_settlement_boundary(report)
    suffix = record["inputs"]["mysql_defaults_group_suffix"]
    inventory = load_inventory(audit_root)
    manifest_identities = report["manifest"]["identities"]
    targets: dict[str, Any] = {
        "account_ids": sorted(manifest_identities["account_ids"]),
        "account_names": sorted(
            row["username"] for row in manifest_identities["accounts"]
        ),
        "auction_ids": sorted(
            row["auction_id"] for row in manifest_identities["auctions"]
        ),
        "character_guids": sorted(manifest_identities["character_guids"]),
        "item_guids": sorted(manifest_identities["item_guids"]),
        "mail_ids": sorted(row["mail_id"] for row in manifest_identities["mail"]),
        "social_actor_ids": sorted(
            row["actor_id"] for row in manifest_identities["derived"]["social_actors"]
        ),
    }
    derived = discover_derived_targets(suffix, targets)
    shared_counts = derived.get("protected_shared_event_count", [])
    if len(shared_counts) != 1:
        raise CleanupRefusal("protected_shared_social_history_count_missing")
    targets.update(
        {
            "economy_position_ids": derived.get("position", []),
            "external_event_actor_ids": derived.get("external_event_actor", []),
            "guild_ids": derived.get("guild", []),
            "guild_item_guids": derived.get("guild_item", []),
            "pet_ids": derived.get("pet", []),
        }
    )
    targets["all_item_guids"] = sorted(
        set(targets["item_guids"]) | set(targets["guild_item_guids"])
    )
    metadata = read_table_metadata(suffix)
    specs = compile_surface_specs(inventory, targets)
    effects, retained = group_mutations(specs, metadata)
    count_effects(suffix, effects)
    count_effects(suffix, retained)
    event_effects = [
        effect
        for effect in effects
        if "archive.playerbot_social_event" in effect["surfaces"]
    ]
    if len(event_effects) != 1:
        raise CleanupRefusal("legacy_social_event_effect_missing")
    legacy_social_event_deletion_count(
        event_effects[0]["expected_rows"], shared_counts[0]
    )
    plan = build_cleanup_plan(
        report,
        record,
        effects,
        expected_frozen_digest=expected_digest,
    )
    plan["targets"].update(targets)
    plan["retained_surfaces"] = retained
    plan["archive"]["legacy_social_events"] = {
        "delete_from_live_store": event_effects[0]["expected_rows"],
        "deszy_participant_events_included": shared_counts[0],
        "policy": "participant_history_is_archived_legacy_epoch_not_protected_entitlement",
    }
    plan["redis_effects"] = redis_effects(record, inventory)
    plan["rollback"] = {"redis": redis_rollback_identity(record)}
    plan["plan_digest"] = ""
    from population_cleanup_support import plan_digest

    plan["plan_digest"] = plan_digest(plan)
    return plan


def verify_post_cleanup(
    record: dict[str, Any],
    audit_root: Path,
    evidence: Path,
    frozen: dict[str, Any],
    plan: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any], list[dict[str, Any]]]:
    mysql_reconciliation = reconcile_mysql_effects(
        record["inputs"]["mysql_defaults_group_suffix"],
        plan["effects"],
        plan["retained_surfaces"],
    )
    first = capture_audit(record, audit_root, evidence / "post-manifest-1.json")
    second = capture_audit(record, audit_root, evidence / "post-manifest-2.json")
    if first.get("digest") != second.get("digest") or first.get(
        "manifest"
    ) != second.get("manifest"):
        raise CleanupApplyFailure("post_manifests_differ")
    for report in (first, second):
        if (
            report.get("capture_status") != "STABLE"
            or report.get("status") != "READY"
            or report.get("exit_code") != 0
            or report.get("refusal_reasons") != []
            or report.get("refusal_surfaces") != []
        ):
            raise CleanupApplyFailure("post_manifest_not_ready")
        manifest = report.get("manifest", {})
        if any(not value for value in report.get("zero_predicates", {}).values()):
            raise CleanupApplyFailure("post_zero_predicate_false")
        if manifest.get("unknown_edges") or manifest.get("unavailable_sources"):
            raise CleanupApplyFailure("post_unclassified_or_unavailable")
        identities = manifest.get("identities", {})
        if identities.get("account_ids") or identities.get("character_guids"):
            raise CleanupApplyFailure("post_targets_remain")
        validate_protected_reconciliation(frozen["manifest"], manifest)
    suffix = record["inputs"]["mysql_defaults_group_suffix"]
    if protected_online_state(suffix) != record.get("protected_online_state"):
        raise CleanupApplyFailure("protected_online_state_changed")
    verify_service_boundary(record)
    for effect in (
        load_inventory(audit_root)["projection_keys"]["redis_strings"]
        + load_inventory(audit_root)["projection_keys"]["redis_streams"]
    ):
        exists = run(
            [
                "redis-cli",
                "-h",
                record["inputs"]["redis_host"],
                "-p",
                str(record["inputs"]["redis_port"]),
                "--raw",
                "EXISTS",
                effect,
            ]
        ).stdout.strip()
        if exists != "0":
            raise CleanupApplyFailure(f"redis_projection_remains:{effect}")
    return first, second, mysql_reconciliation


def rollback_all(
    record: dict[str, Any],
    audit_root: Path,
    evidence: Path,
    expected_digest: str,
    failure: str,
    plan: dict[str, Any],
) -> dict[str, Any]:
    rollback = {
        "failure": failure,
        "started_at": datetime.now(timezone.utc).isoformat(),
    }
    write_durable(evidence / "rollback-record.json", rollback)
    restore_errors = []
    for name, restore in (
        ("mysql", lambda: restore_mysql(record)),
        ("redis", lambda: restore_redis(record, plan["rollback"]["redis"])),
    ):
        try:
            restore()
        except (
            CleanupApplyFailure,
            CleanupRefusal,
            KeyError,
            OSError,
            ValueError,
        ) as error:
            restore_errors.append(f"{name}:{type(error).__name__}:{error}")
    if restore_errors:
        raise CleanupApplyFailure(
            "rollback_all_stores_failed:" + "|".join(restore_errors)
        )
    restored = capture_audit(record, audit_root, evidence / "rollback-manifest.json")
    validate_frozen_authority(restored, expected_digest)
    if protected_online_state(
        record["inputs"]["mysql_defaults_group_suffix"]
    ) != record.get("protected_online_state"):
        raise CleanupApplyFailure("rollback_protected_state_mismatch")
    rollback.update(
        {
            "completed_at": datetime.now(timezone.utc).isoformat(),
            "restored_digest": restored["digest"],
            "status": "RESTORED_ALL_STORES",
        }
    )
    write_durable(evidence / "rollback-record.json", rollback)
    return rollback


def preserve_hashes(evidence: Path) -> None:
    excluded = {"SHA256SUMS"}
    lines = []
    for path in sorted(evidence.rglob("*")):
        if path.is_file() and path.name not in excluded:
            lines.append(
                f"{sha256_file(path)}  {path.relative_to(evidence).as_posix()}\n"
            )
    (evidence / "SHA256SUMS").write_text("".join(lines), encoding="utf-8")


def plan_command(arguments: argparse.Namespace) -> int:
    evidence = ensure_output_directory(Path(arguments.output_dir))
    record, first, _ = live_gate(
        Path(arguments.operation_record),
        Path(arguments.audit_root).resolve(),
        evidence,
        arguments.expected_frozen_digest,
        "pre-manifest",
    )
    plan = compile_plan(
        first,
        record,
        Path(arguments.audit_root).resolve(),
        arguments.expected_frozen_digest,
    )
    validate_exact_plan(plan)
    write_durable(evidence / "cleanup-plan.json", plan)
    operation = {
        "completed_at": datetime.now(timezone.utc).isoformat(),
        "mode": "plan",
        "plan_digest": plan["plan_digest"],
        "status": "PLANNED",
    }
    write_durable(evidence / "operation-record.json", operation)
    preserve_hashes(evidence)
    print(evidence / "cleanup-plan.json")
    return 0


def apply_command(arguments: argparse.Namespace) -> int:
    evidence = ensure_output_directory(Path(arguments.output_dir))
    supplied_plan = load_json(Path(arguments.plan))
    validate_exact_plan(supplied_plan)
    record, first, _ = live_gate(
        Path(arguments.operation_record),
        Path(arguments.audit_root).resolve(),
        evidence,
        arguments.expected_frozen_digest,
        "apply-pre-manifest",
    )
    current_plan = compile_plan(
        first,
        record,
        Path(arguments.audit_root).resolve(),
        arguments.expected_frozen_digest,
    )
    if current_plan != supplied_plan:
        raise CleanupRefusal("exact_plan_no_longer_matches")
    write_durable(evidence / "cleanup-plan.json", supplied_plan)
    archive = {
        "archived_at": datetime.now(timezone.utc).isoformat(),
        "cleanup_plan_digest": supplied_plan["plan_digest"],
        "frozen_manifest_digest": arguments.expected_frozen_digest,
        "legacy_epoch_authority": supplied_plan["archive"],
        "retained_surfaces": supplied_plan["retained_surfaces"],
        "status": "DURABLE_BEFORE_LIVE_PROJECTION_CLEAR",
    }
    write_durable(evidence / "archive-epoch.json", archive)
    operation = {
        "archive": archive,
        "mode": "apply",
        "plan_digest": supplied_plan["plan_digest"],
        "started_at": datetime.now(timezone.utc).isoformat(),
        "status": "APPLYING",
    }
    write_durable(evidence / "operation-record.json", operation)
    mutation_started = False
    try:
        mutation_started = True
        operation["mysql_results"] = apply_mysql(
            record["inputs"]["mysql_defaults_group_suffix"], supplied_plan["effects"]
        )
        operation["redis_results"] = apply_redis(record, supplied_plan["redis_effects"])
        post_first, post_second, mysql_reconciliation = verify_post_cleanup(
            record,
            Path(arguments.audit_root).resolve(),
            evidence,
            first,
            supplied_plan,
        )
        operation.update(
            {
                "completed_at": datetime.now(timezone.utc).isoformat(),
                "post_digest": post_first["digest"],
                "post_pair_identical": (
                    post_first.get("digest") == post_second.get("digest")
                    and post_first.get("manifest") == post_second.get("manifest")
                ),
                "protected_online_state": protected_online_state(
                    record["inputs"]["mysql_defaults_group_suffix"]
                ),
                "rollback_needed": False,
                "status": "VERIFIED_CLEANUP_WRITERS_STOPPED",
            }
        )
        write_durable(evidence / "operation-record.json", operation)
        write_durable(
            evidence / "reconciliation.json",
            {
                "after_digest": post_first["digest"],
                "before_digest": arguments.expected_frozen_digest,
                "plan_digest": supplied_plan["plan_digest"],
                "protected_unchanged": True,
                "redis_projections_closed": True,
                "mysql_surfaces": mysql_reconciliation,
                "status": "RECONCILED",
                "target_accounts_remaining": 0,
                "target_characters_remaining": 0,
            },
        )
    except BaseException as error:
        operation.update(
            {
                "apply_failure": f"{type(error).__name__}:{error}",
                "status": "ROLLBACK_REQUIRED" if mutation_started else "REFUSED",
            }
        )
        write_durable(evidence / "operation-record.json", operation)
        if mutation_started:
            operation["rollback"] = rollback_all(
                record,
                Path(arguments.audit_root).resolve(),
                evidence,
                arguments.expected_frozen_digest,
                operation["apply_failure"],
                supplied_plan,
            )
            operation["status"] = "ROLLED_BACK_ALL_STORES"
            write_durable(evidence / "operation-record.json", operation)
        preserve_hashes(evidence)
        raise
    preserve_hashes(evidence)
    print(evidence)
    return 0


if __name__ == "__main__":
    raise SystemExit(run_cli(plan_command, apply_command))
