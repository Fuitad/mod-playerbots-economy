from __future__ import annotations

import gzip
import os
import re
import shutil
import subprocess
import time
from pathlib import Path
from typing import Any

from population_cleanup_support import CleanupApplyFailure, require_affected_rows

IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def identifier(value: str) -> str:
    if IDENTIFIER.fullmatch(value) is None:
        raise CleanupApplyFailure(f"invalid_identifier:{value}")
    return f"`{value}`"


def mysql_command(suffix: str) -> list[str]:
    if re.fullmatch(r"[A-Za-z0-9_]+", suffix) is None:
        raise CleanupApplyFailure("invalid_mysql_defaults_group_suffix")
    return [
        "mysql",
        f"--defaults-group-suffix={suffix}",
        "--batch",
        "--raw",
        "--skip-column-names",
    ]


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(command, check=False, capture_output=True, text=True)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise CleanupApplyFailure(f"command_failed:{command[0]}:{detail}")
    return completed


def mutation_sql(effect: dict[str, Any]) -> str:
    qualified = f"{identifier(effect['schema'])}.{identifier(effect['table'])}"
    if effect["action"] == "delete":
        return f"DELETE FROM {qualified} AS t WHERE {effect['predicate']}"
    if effect["action"] == "update_zero":
        return (
            f"UPDATE {qualified} t SET t.{identifier(effect['update_column'])}=0 "
            f"WHERE {effect['predicate']}"
        )
    raise CleanupApplyFailure(f"unsupported_mutation_action:{effect['action']}")


class MysqlMutationSession:
    def __init__(self, suffix: str):
        self.process = subprocess.Popen(
            [*mysql_command(suffix), "--unbuffered"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        if self.process.stdin is None or self.process.stdout is None:
            raise CleanupApplyFailure("mysql_session_pipe_missing")

    def command(self, sql: str, marker: str) -> None:
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(f"{sql};SELECT '{marker}';\n")
        self.process.stdin.flush()
        while True:
            line = self.process.stdout.readline()
            if line == "":
                detail = (
                    self.process.stderr.read().strip() if self.process.stderr else ""
                )
                raise CleanupApplyFailure(f"mysql_session_ended:{detail}")
            if line.rstrip("\n") == marker:
                return

    def scalar(self, sql: str, marker: str) -> int:
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(f"SELECT CONCAT('{marker}',({sql}));\n")
        self.process.stdin.flush()
        return self._read_integer(marker)

    def mutate(self, sql: str, marker: str) -> int:
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(f"{sql};SELECT CONCAT('{marker}',ROW_COUNT());\n")
        self.process.stdin.flush()
        return self._read_integer(marker)

    def _read_integer(self, marker: str) -> int:
        assert self.process.stdout is not None
        while True:
            line = self.process.stdout.readline()
            if line == "":
                detail = (
                    self.process.stderr.read().strip() if self.process.stderr else ""
                )
                raise CleanupApplyFailure(f"mysql_session_ended:{detail}")
            value = line.rstrip("\n")
            if value.startswith(marker):
                return int(value.removeprefix(marker))

    def close(self) -> None:
        if self.process.stdin is not None:
            self.process.stdin.close()
        self.process.wait(timeout=30)
        if self.process.returncode != 0:
            detail = self.process.stderr.read().strip() if self.process.stderr else ""
            raise CleanupApplyFailure(f"mysql_session_failed:{detail}")


def apply_mysql(suffix: str, effects: list[dict[str, Any]]) -> list[dict[str, Any]]:
    session = MysqlMutationSession(suffix)
    results: list[dict[str, Any]] = []
    committed = False
    primary_error: BaseException | None = None
    try:
        session.command(
            "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE;START TRANSACTION",
            "__STARTED__",
        )
        for index, effect in enumerate(effects):
            qualified = f"{identifier(effect['schema'])}.{identifier(effect['table'])}"
            actual_before = session.scalar(
                f"SELECT COUNT(*) FROM {qualified} t WHERE {effect['predicate']}",
                f"__COUNT_{index}__",
            )
            surface = ",".join(effect["surfaces"])
            require_affected_rows(surface, effect["expected_rows"], actual_before)
            affected = session.mutate(mutation_sql(effect), f"__ROWS_{index}__")
            require_affected_rows(surface, effect["expected_rows"], affected)
            results.append(
                {
                    "affected_rows": affected,
                    "engine": effect["engine"],
                    "schema": effect["schema"],
                    "surfaces": effect["surfaces"],
                    "table": effect["table"],
                }
            )
        session.command("COMMIT", "__COMMITTED__")
        committed = True
    except BaseException as error:
        primary_error = error
        raise
    finally:
        if not committed and session.process.poll() is None:
            try:
                session.command("ROLLBACK", "__ROLLED_BACK__")
            except CleanupApplyFailure:
                pass
        try:
            session.close()
        except CleanupApplyFailure:
            if primary_error is None:
                raise
    return results


def apply_redis(
    record: dict[str, Any], effects: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    inputs = record["inputs"]
    results = []
    for effect in effects:
        completed = run(
            [
                "redis-cli",
                "-h",
                inputs["redis_host"],
                "-p",
                str(inputs["redis_port"]),
                "--raw",
                "DEL",
                effect["key"],
            ]
        )
        deleted = int(completed.stdout.strip())
        require_affected_rows(effect["key"], effect["expected_exists"], deleted)
        results.append({"deleted": deleted, "key": effect["key"]})
    return results


def restore_mysql(record: dict[str, Any]) -> None:
    suffix = record["inputs"]["mysql_defaults_group_suffix"]
    backup = Path(record["backup"]["mysql"]["path"])
    mysql = subprocess.Popen(mysql_command(suffix), stdin=subprocess.PIPE)
    if mysql.stdin is None:
        raise CleanupApplyFailure("rollback_mysql_pipe_missing")
    try:
        with gzip.open(backup, "rb") as source:
            shutil.copyfileobj(source, mysql.stdin)
    finally:
        mysql.stdin.close()
    mysql.wait()
    if mysql.returncode != 0:
        raise CleanupApplyFailure("rollback_mysql_restore_failed")


def wait_launchctl(label: str, *, running: bool, timeout: int = 60) -> None:
    uid = os.getuid()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        completed = subprocess.run(
            ["launchctl", "print", f"gui/{uid}/{label}"],
            check=False,
            capture_output=True,
            text=True,
        )
        is_running = completed.returncode == 0 and "state = running" in completed.stdout
        if is_running == running:
            return
        time.sleep(0.25)
    raise CleanupApplyFailure(f"rollback_service_timeout:{label}:{running}")


def stop_redis(label: str) -> None:
    subprocess.run(
        ["launchctl", "bootout", f"gui/{os.getuid()}/{label}"],
        check=False,
        capture_output=True,
        text=True,
    )
    wait_launchctl(label, running=False)


def bootstrap_redis(plist: Path, label: str, attempts: int = 3) -> None:
    failures = []
    for attempt in range(attempts):
        completed = subprocess.run(
            ["launchctl", "bootstrap", f"gui/{os.getuid()}", str(plist)],
            check=False,
            capture_output=True,
            text=True,
        )
        if completed.returncode == 0:
            wait_launchctl(label, running=True)
            return
        failures.append(completed.stderr.strip() or completed.stdout.strip())
        if attempt + 1 < attempts:
            time.sleep(1.0)
    raise CleanupApplyFailure("redis_bootstrap_failed:" + "|".join(failures))


def redis_command(record: dict[str, Any], *arguments: str) -> str:
    inputs = record["inputs"]
    return run(
        [
            "redis-cli",
            "-h",
            inputs["redis_host"],
            "-p",
            str(inputs["redis_port"]),
            "--raw",
            *arguments,
        ]
    ).stdout.strip()


def wipe_redis(
    record: dict[str, Any], identity: dict[str, str], projection_keys: list[str]
) -> dict[str, Any]:
    label = "homebrew.mxcl.redis"
    service = record["services"][label]
    plist = Path(service["plist"])
    directory = Path(identity["dir"])
    filename = Path(identity["dbfilename"])
    if (
        identity.get("appendonly") != "no"
        or identity.get("mode") != "whole_instance_wipe"
        or not directory.is_absolute()
        or directory == Path("/")
        or filename.name != identity["dbfilename"]
    ):
        raise CleanupApplyFailure("redis_wipe_identity_invalid")
    if projection_keys != sorted(set(projection_keys)):
        raise CleanupApplyFailure("redis_projection_keys_noncanonical")

    stop_redis(label)
    snapshot = directory / filename
    snapshot.unlink(missing_ok=True)
    time.sleep(1.0)
    bootstrap_redis(plist, label)

    if redis_command(record, "FLUSHALL", "SYNC") != "OK":
        raise CleanupApplyFailure("redis_flushall_failed")
    if redis_command(record, "SAVE") != "OK":
        raise CleanupApplyFailure("redis_empty_save_failed")
    if redis_command(record, "PING") != "PONG":
        raise CleanupApplyFailure("redis_ping_failed")
    dbsize = int(redis_command(record, "DBSIZE"))
    projection_exists = {
        key: int(redis_command(record, "EXISTS", key)) for key in projection_keys
    }
    if dbsize != 0 or any(projection_exists.values()):
        raise CleanupApplyFailure("redis_wipe_incomplete")
    return {
        "dbsize": dbsize,
        "mode": "whole_instance_wipe",
        "projection_exists": projection_exists,
        "restart_proven": True,
    }
