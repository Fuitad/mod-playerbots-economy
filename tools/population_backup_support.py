from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from collections.abc import Callable
from pathlib import Path
from typing import Any

MYSQL_SCHEMAS = ("acore_auth", "acore_characters", "acore_playerbots", "medivh")
RUN_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}")
SERVICE_LABEL = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]+")
STABLE_AUDIT_STATUSES = frozenset({"READY", "NOT_READY"})


class SafetyRefusal(RuntimeError):
    pass


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_isolated_path(operation: Path, candidate: Path) -> Path:
    root = operation.resolve()
    path = candidate.resolve()
    if path == root or root not in path.parents:
        raise SafetyRefusal(f"outside_operation:{path}")
    return path


def mysql_dump_command(defaults_group_suffix: str) -> list[str]:
    if re.fullmatch(r"[A-Za-z0-9_]+", defaults_group_suffix) is None:
        raise SafetyRefusal("invalid_mysql_defaults_group_suffix")
    return [
        "mysqldump",
        f"--defaults-group-suffix={defaults_group_suffix}",
        "--lock-all-tables",
        "--routines",
        "--events",
        "--triggers",
        "--hex-blob",
        "--set-gtid-purged=OFF",
        "--default-character-set=utf8mb4",
        "--skip-tz-utc",
        "--databases",
        *MYSQL_SCHEMAS,
    ]


def validate_protected_baseline(report: dict[str, Any]) -> None:
    if report.get("refusal_reasons") or report.get("refusal_surfaces"):
        raise SafetyRefusal("protected_deszy:audit_refusal")
    manifest = report.get("manifest")
    if not isinstance(manifest, dict):
        raise SafetyRefusal("protected_deszy:manifest_missing")
    for key in ("unknown_edges", "unavailable_sources", "protected_overlap_surfaces"):
        if manifest.get(key) != []:
            raise SafetyRefusal(f"protected_deszy:{key}")
    baseline = manifest.get("protected_baseline")
    if not isinstance(baseline, dict):
        raise SafetyRefusal("protected_deszy:baseline_missing")
    accounts = baseline.get("accounts")
    characters = baseline.get("characters")
    ownership = baseline.get("ownership")
    if not isinstance(accounts, list) or len(accounts) != 1:
        raise SafetyRefusal("protected_deszy:account")
    if not isinstance(characters, list) or len(characters) != 1:
        raise SafetyRefusal("protected_deszy:character")
    if int(accounts[0].get("account_id", -1)) != 157:
        raise SafetyRefusal("protected_deszy:account")
    if characters[0] != {
        "account_id": 157,
        "character_guid": 661,
        "delete_account_id": None,
        "name": "Deszy",
    }:
        raise SafetyRefusal("protected_deszy:character")
    if ownership != []:
        raise SafetyRefusal("protected_deszy:ownership")


def validate_protected_online_state(rows: list[dict[str, Any]]) -> None:
    expected = [
        {
            "account_id": 157,
            "account_online": 0,
            "character_guid": 661,
            "character_online": 0,
            "delete_account_id": None,
            "name": "Deszy",
        }
    ]
    if rows != expected:
        raise SafetyRefusal("protected_deszy:online_state")


def audit_until_stable(
    capture: Callable[[int], dict[str, Any]], attempts: int
) -> dict[str, Any]:
    if attempts < 1:
        raise SafetyRefusal("invalid_audit_attempts")
    last_status = "missing"
    for attempt in range(1, attempts + 1):
        report = capture(attempt)
        last_status = str(report.get("status", "missing"))
        if (
            last_status == "REFUSED"
            or report.get("refusal_reasons")
            or report.get("refusal_surfaces")
        ):
            raise SafetyRefusal("audit_refused")
        if last_status in STABLE_AUDIT_STATUSES:
            validate_protected_baseline(report)
            return report
        if last_status != "UNSTABLE":
            raise SafetyRefusal(f"audit_invalid_status:{last_status}")
    raise SafetyRefusal(f"audit_unstable_after_{attempts}:{last_status}")


def validate_restored_report(frozen: dict[str, Any], restored: dict[str, Any]) -> None:
    validate_protected_baseline(frozen)
    validate_protected_baseline(restored)
    if frozen.get("digest") != restored.get("digest"):
        raise SafetyRefusal("restore_digest_mismatch")
    if frozen.get("counts") != restored.get("counts"):
        raise SafetyRefusal("restore_counts_mismatch")


def parse_launchctl(output: str) -> dict[str, Any]:
    values: dict[str, Any] = {}
    for line in output.splitlines():
        match = re.match(
            r"^\s*(path|state|program|working directory|pid) = (.+)$", line
        )
        if match is None or match.group(1) in values:
            continue
        key, value = match.groups()
        values[key] = int(value) if key == "pid" and value.isdigit() else value
    if not {"path", "state", "program", "pid"}.issubset(values):
        raise SafetyRefusal("service_identity_incomplete")
    return values


def run_command(
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
        raise SafetyRefusal(f"command_failed:{command[0]}:{detail}")
    return completed


def writer_spec(value: str) -> tuple[str, Path]:
    label, separator, plist = value.partition("=")
    if separator != "=" or SERVICE_LABEL.fullmatch(label) is None:
        raise argparse.ArgumentTypeError(
            "writer service must be LABEL=/absolute/path.plist"
        )
    path = Path(plist)
    if not path.is_absolute():
        raise argparse.ArgumentTypeError("writer service plist must be absolute")
    return label, path
