from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import plistlib
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from io import BufferedWriter
from pathlib import Path
from typing import Any

from population_backup_support import (
    MYSQL_SCHEMAS,
    RUN_NAME,
    SafetyRefusal,
    audit_until_stable,
    canonical_json,
    mysql_dump_command,
    parse_launchctl,
    require_isolated_path,
    run_command,
    sha256_file,
    validate_protected_baseline,
    validate_protected_online_state,
    validate_restored_report,
    writer_spec,
)


class PopulationBackupOperation:
    def __init__(self, arguments: argparse.Namespace) -> None:
        self.arguments = arguments
        self._validate_service_partition()
        self.uid = os.getuid()
        self.operation = self._create_operation_directory()
        self.record: dict[str, Any] = {
            "format_version": 1,
            "operation": str(self.operation),
            "started_at": datetime.now(timezone.utc).isoformat(),
            "status": "PREFLIGHT",
            "commands": [],
            "inputs": {
                "artifacts": list(arguments.artifact),
                "audit_attempts": arguments.audit_attempts,
                "azerothcore_root": arguments.azerothcore_root,
                "backup_root": arguments.backup_root,
                "medivh_root": arguments.medivh_root,
                "mysql_defaults_group_suffix": arguments.mysql_defaults_group_suffix,
                "playerbots_config": arguments.playerbots_config,
                "redis_host": arguments.redis_host,
                "redis_port": arguments.redis_port,
                "repositories": list(arguments.repository),
                "required_running_services": [
                    {"label": label, "plist": str(path)}
                    for label, path in arguments.required_running_service
                ],
                "run_name": arguments.run_name,
                "writer_services": [
                    {"label": label, "plist": str(path)}
                    for label, path in arguments.writer_service
                ],
            },
            "services": {},
        }
        self.stopped_labels: list[str] = []
        self.maintenance_enabled = False
        self.mysql_process: subprocess.Popen[bytes] | None = None
        self.redis_process: subprocess.Popen[bytes] | None = None
        self.redis_log: BufferedWriter | None = None
        self.write_record()

    def _validate_service_partition(self) -> None:
        writers = [label for label, _ in self.arguments.writer_service]
        required = [label for label, _ in self.arguments.required_running_service]
        if len(writers) != len(set(writers)) or len(required) != len(set(required)):
            raise SafetyRefusal("duplicate_service_label")
        overlap = sorted(set(writers) & set(required))
        if overlap:
            raise SafetyRefusal(f"service_partition_overlap:{','.join(overlap)}")

    def _create_operation_directory(self) -> Path:
        root = Path(self.arguments.backup_root).resolve()
        if not root.is_dir() or RUN_NAME.fullmatch(self.arguments.run_name) is None:
            raise SafetyRefusal("invalid_backup_destination")
        operation = root / self.arguments.run_name
        if operation.exists():
            raise SafetyRefusal(f"backup_destination_exists:{operation}")
        operation.mkdir(mode=0o700)
        return operation

    def write_record(self) -> None:
        destination = self.operation / "operation-record.json"
        temporary = self.operation / ".operation-record.json.tmp"
        temporary.write_text(canonical_json(self.record), encoding="utf-8")
        temporary.replace(destination)

    def record_command(self, command: list[str]) -> None:
        self.record["commands"].append(command)
        self.write_record()

    def command(
        self,
        command: list[str],
        *,
        cwd: Path | None = None,
        stdin: str | None = None,
        accepted: frozenset[int] = frozenset({0}),
    ) -> subprocess.CompletedProcess[str]:
        self.record_command(command)
        return run_command(command, cwd=cwd, stdin=stdin, accepted=accepted)

    def service_state(self, label: str, plist_path: Path) -> dict[str, Any]:
        if not plist_path.is_file():
            raise SafetyRefusal(f"service_plist_missing:{label}")
        with plist_path.open("rb") as handle:
            plist = plistlib.load(handle)
        if plist.get("Label") != label:
            raise SafetyRefusal(f"service_plist_label_mismatch:{label}")
        completed = self.command(["launchctl", "print", f"gui/{self.uid}/{label}"])
        state = parse_launchctl(completed.stdout)
        if Path(str(state["path"])).resolve() != plist_path.resolve():
            raise SafetyRefusal(f"service_plist_runtime_mismatch:{label}")
        if state["state"] != "running":
            raise SafetyRefusal(f"service_not_running:{label}")
        expected_program = str(plist["ProgramArguments"][0])
        if state["program"] != expected_program:
            raise SafetyRefusal(f"service_program_mismatch:{label}")
        working = plist.get("WorkingDirectory")
        if working and state.get("working directory") != working:
            raise SafetyRefusal(f"service_working_directory_mismatch:{label}")
        pid = int(state["pid"])
        listeners = self.command(
            ["lsof", "-Pan", "-p", str(pid), "-iTCP", "-sTCP:LISTEN", "-Fn"],
            accepted=frozenset({0, 1}),
        ).stdout.splitlines()
        return {
            "label": label,
            "plist": str(plist_path.resolve()),
            "plist_sha256": sha256_file(plist_path),
            "program": expected_program,
            "working_directory": working,
            "pid": pid,
            "listeners": sorted(line[1:] for line in listeners if line.startswith("n")),
            "prior_state": "running",
        }

    def capture_identities(self) -> None:
        repositories = []
        for value in self.arguments.repository:
            path = Path(value).resolve()
            revision = self.command(
                ["git", "-C", str(path), "rev-parse", "HEAD"]
            ).stdout.strip()
            status = self.command(
                [
                    "git",
                    "-C",
                    str(path),
                    "status",
                    "--porcelain=v1",
                    "--untracked-files=all",
                ]
            ).stdout.splitlines()
            repositories.append(
                {"path": str(path), "revision": revision, "status": status}
            )
        artifacts = []
        for value in self.arguments.artifact:
            path = Path(value).resolve()
            if not path.is_file():
                raise SafetyRefusal(f"artifact_missing:{path}")
            artifacts.append({"path": str(path), "sha256": sha256_file(path)})
        self.record["repositories"] = repositories
        self.record["artifacts"] = artifacts
        self.write_record()

    def mysql_query(self, sql: str, *, isolated_socket: Path | None = None) -> str:
        if isolated_socket is None:
            command = [
                "mysql",
                f"--defaults-group-suffix={self.arguments.mysql_defaults_group_suffix}",
                "--batch",
                "--raw",
                "--skip-column-names",
            ]
        else:
            command = [
                "mysql",
                "--no-defaults",
                f"--socket={isolated_socket}",
                "--user=root",
                "--batch",
                "--raw",
                "--skip-column-names",
            ]
        return self.command(command, stdin=sql).stdout

    def protected_online_state(
        self, socket: Path | None = None
    ) -> list[dict[str, Any]]:
        sql = (
            "SELECT a.id,a.online,c.guid,c.online,c.deleteInfos_Account,c.name "
            "FROM acore_auth.account a JOIN acore_characters.characters c ON c.account=a.id "
            "WHERE a.id=157 AND c.guid=661;"
        )
        rows = []
        for line in self.mysql_query(sql, isolated_socket=socket).splitlines():
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
        validate_protected_online_state(rows)
        return rows

    def storage_identity(self) -> dict[str, Any]:
        schemas = ",".join(f"'{schema}'" for schema in MYSQL_SCHEMAS)
        mysql = self.mysql_query(
            "SELECT VERSION();"
            "SELECT TABLE_SCHEMA,COALESCE(ENGINE,'NULL'),COUNT(*) FROM information_schema.TABLES "
            f"WHERE TABLE_SCHEMA IN ({schemas}) GROUP BY TABLE_SCHEMA,ENGINE "
            "ORDER BY TABLE_SCHEMA,ENGINE;"
        )
        lines = mysql.splitlines()
        if not lines:
            raise SafetyRefusal("mysql_identity_missing")
        engine_rows = [line.split("\t") for line in lines[1:]]
        if {row[0] for row in engine_rows if len(row) == 3} != set(MYSQL_SCHEMAS):
            raise SafetyRefusal("mysql_schema_identity_mismatch")
        events = self.mysql_query(
            "SELECT EVENT_SCHEMA,EVENT_NAME,STATUS FROM information_schema.EVENTS "
            f"WHERE EVENT_SCHEMA IN ({schemas}) ORDER BY EVENT_SCHEMA,EVENT_NAME;"
        )
        if events.strip():
            raise SafetyRefusal("scoped_mysql_event_present")
        redis = self.command(
            [
                "redis-cli",
                "-h",
                self.arguments.redis_host,
                "-p",
                str(self.arguments.redis_port),
                "--raw",
                "INFO",
                "server",
            ]
        ).stdout
        if f"tcp_port:{self.arguments.redis_port}" not in redis.splitlines():
            raise SafetyRefusal("redis_identity_mismatch")
        return {
            "mysql_version": lines[0],
            "mysql_engines": engine_rows,
            "mysql_scoped_events": [],
            "redis_server_sha256": hashlib.sha256(redis.encode()).hexdigest(),
        }

    def audit_command(
        self,
        output: Path,
        *,
        mysql_socket: Path | None = None,
        redis_socket: Path | None = None,
    ) -> list[str]:
        command = [
            sys.executable,
            str(Path(__file__).with_name("population_manifest.py")),
            "live",
            "--playerbots-config",
            str(Path(self.arguments.playerbots_config).resolve()),
            "--protected-account-id",
            "157",
            "--protected-character-guid",
            "661",
            "--protected-character-name",
            "Deszy",
            "--azerothcore-root",
            str(Path(self.arguments.azerothcore_root).resolve()),
            "--medivh-root",
            str(Path(self.arguments.medivh_root).resolve()),
            "--mysql-defaults-group-suffix",
            self.arguments.mysql_defaults_group_suffix,
            "--redis-host",
            self.arguments.redis_host,
            "--redis-port",
            str(self.arguments.redis_port),
            "--output",
            str(output),
        ]
        if mysql_socket is not None:
            command.extend(["--mysql-no-defaults", "--mysql-socket", str(mysql_socket)])
        if redis_socket is not None:
            command.extend(["--redis-socket", str(redis_socket)])
        return command

    def capture_audit(
        self,
        name: str,
        attempt: int,
        *,
        mysql_socket: Path | None = None,
        redis_socket: Path | None = None,
    ) -> dict[str, Any]:
        output = self.operation / f"{name}-attempt-{attempt}.json"
        completed = self.command(
            self.audit_command(
                output, mysql_socket=mysql_socket, redis_socket=redis_socket
            ),
            accepted=frozenset({0, 3, 4}),
        )
        report = json.loads(output.read_text(encoding="utf-8"))
        if completed.returncode == 4 and report.get("status") != "REFUSED":
            raise SafetyRefusal("audit_exit_status_mismatch")
        return report

    def preflight(self) -> None:
        self.capture_identities()
        for label, plist_path in self.arguments.writer_service:
            self.record["services"][label] = self.service_state(label, plist_path)
        for label, plist_path in self.arguments.required_running_service:
            self.record["services"][label] = self.service_state(label, plist_path)
        self.record["protected_online_state"] = self.protected_online_state()
        self.record["storage_identity"] = self.storage_identity()
        report = self.capture_audit("active-preflight", 1)
        validate_protected_baseline(report)
        self.record["active_preflight"] = {
            "status": report["status"],
            "first_digest": report.get("first_digest"),
            "second_digest": report.get("second_digest"),
            "changed_surfaces": report.get("changed_surfaces", []),
        }
        self.write_record()

    def wait_for_service_unload(self, label: str) -> bool:
        deadline = time.monotonic() + self.arguments.stop_timeout_seconds
        while time.monotonic() < deadline:
            completed = subprocess.run(
                ["launchctl", "print", f"gui/{self.uid}/{label}"],
                check=False,
                capture_output=True,
                text=True,
            )
            if completed.returncode != 0:
                return True
            time.sleep(1)
        return False

    def bootstrap_service(self, label: str, plist: str) -> str | None:
        failures = []
        for attempt in range(3):
            completed = subprocess.run(
                ["launchctl", "bootstrap", f"gui/{self.uid}", plist],
                check=False,
                capture_output=True,
                text=True,
            )
            if completed.returncode == 0:
                return None
            failures.append(
                completed.stderr.strip()
                or completed.stdout.strip()
                or f"exit_{completed.returncode}"
            )
            if attempt < 2:
                time.sleep(1)
        return f"bootstrap:{label}:{'|'.join(failures)}"

    def stop_writers(self) -> None:
        medivh = Path(self.arguments.medivh_root).resolve()
        down = medivh / "storage/framework/down"
        if down.exists():
            raise SafetyRefusal("medivh_already_in_maintenance")
        self.command([self.arguments.php, "artisan", "down", "--no-ansi"], cwd=medivh)
        self.maintenance_enabled = True
        for label, _ in self.arguments.writer_service:
            self.command(["launchctl", "bootout", f"gui/{self.uid}/{label}"])
            self.stopped_labels.append(label)
        deadline = time.monotonic() + self.arguments.stop_timeout_seconds
        while time.monotonic() < deadline:
            survivors = []
            for label, _ in self.arguments.writer_service:
                service = self.record["services"][label]
                pid = int(service["pid"])
                completed = subprocess.run(
                    ["ps", "-p", str(pid), "-o", "pid="],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                if completed.stdout.strip():
                    survivors.append(service["label"])
            if not survivors:
                break
            time.sleep(1)
        else:
            raise SafetyRefusal(f"writer_process_survived:{','.join(survivors)}")
        for label, _ in self.arguments.writer_service:
            if not self.wait_for_service_unload(label):
                raise SafetyRefusal(f"writer_service_still_loaded:{label}")
            service = self.record["services"][label]
            for listener in service["listeners"]:
                port = listener.rsplit(":", 1)[-1]
                completed = self.command(
                    ["lsof", "-nP", f"-iTCP:{port}", "-sTCP:LISTEN"],
                    accepted=frozenset({0, 1}),
                )
                if completed.stdout.strip():
                    raise SafetyRefusal(f"writer_listener_survived:{listener}")
        for label, plist_path in self.arguments.required_running_service:
            self.service_state(label, plist_path)

    def freeze(self) -> dict[str, Any]:
        self.record["frozen_storage_identity"] = self.storage_identity()
        report = audit_until_stable(
            lambda attempt: self.capture_audit("frozen", attempt),
            self.arguments.audit_attempts,
        )
        destination = self.operation / "frozen-manifest.json"
        destination.write_text(canonical_json(report), encoding="utf-8")
        self.record["frozen"] = {
            "status": report["status"],
            "digest": report["digest"],
            "counts": report["counts"],
            "sha256": sha256_file(destination),
        }
        self.write_record()
        return report

    def dump_mysql(self) -> Path:
        destination = self.operation / "authoritative-mysql.sql.gz"
        error_path = self.operation / "authoritative-mysql.stderr.log"
        command = mysql_dump_command(self.arguments.mysql_defaults_group_suffix)
        self.record_command(command)
        with error_path.open("wb") as error_output:
            process = subprocess.Popen(
                command, stdout=subprocess.PIPE, stderr=error_output
            )
            assert process.stdout is not None
            with gzip.open(destination, "wb", compresslevel=9) as output:
                shutil.copyfileobj(process.stdout, output)
        error = error_path.read_text(encoding="utf-8", errors="replace").strip()
        if process.wait() != 0:
            raise SafetyRefusal(f"mysqldump_failed:{error}")
        if error:
            raise SafetyRefusal(f"mysqldump_warning:{error}")
        completed = False
        with gzip.open(destination, "rt", encoding="utf-8") as source:
            for line in source:
                if line.startswith("-- Dump completed on "):
                    completed = True
        if not completed:
            raise SafetyRefusal("mysqldump_completion_marker_missing")
        return destination

    def dump_redis(self) -> Path:
        destination = self.operation / "authoritative-redis.rdb"
        self.command(
            [
                "redis-cli",
                "-h",
                self.arguments.redis_host,
                "-p",
                str(self.arguments.redis_port),
                "--rdb",
                str(destination),
            ]
        )
        if not destination.is_file() or destination.stat().st_size == 0:
            raise SafetyRefusal("redis_backup_missing")
        return destination

    def backup(self) -> tuple[Path, Path]:
        mysql_dump = self.dump_mysql()
        redis_dump = self.dump_redis()
        hashes = {
            mysql_dump.name: sha256_file(mysql_dump),
            redis_dump.name: sha256_file(redis_dump),
        }
        (self.operation / "SHA256SUMS").write_text(
            "".join(f"{digest}  {name}\n" for name, digest in sorted(hashes.items())),
            encoding="utf-8",
        )
        self.record["backup"] = {
            "mysql": {"path": str(mysql_dump), "sha256": hashes[mysql_dump.name]},
            "redis": {"path": str(redis_dump), "sha256": hashes[redis_dump.name]},
            "mutually_consistent_basis": "writers quiesced plus one cross-schema global MySQL read lock",
        }
        self.write_record()
        return mysql_dump, redis_dump

    def wait_for(self, command: list[str], timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            completed = subprocess.run(command, check=False, capture_output=True)
            if completed.returncode == 0:
                return
            time.sleep(0.25)
        raise SafetyRefusal(f"isolated_service_timeout:{command[0]}")

    def start_isolated_mysql(self, rehearsal: Path) -> tuple[Path, Path]:
        data = require_isolated_path(self.operation, rehearsal / "mysql-data")
        socket = require_isolated_path(self.operation, rehearsal / "mysql.sock")
        data.mkdir()
        self.command(
            ["mysqld", "--no-defaults", "--initialize-insecure", f"--datadir={data}"]
        )
        command = [
            "mysqld",
            "--no-defaults",
            f"--datadir={data}",
            f"--socket={socket}",
            f"--pid-file={rehearsal / 'mysql.pid'}",
            f"--log-error={rehearsal / 'mysql.log'}",
            "--skip-networking",
            "--mysqlx=OFF",
        ]
        self.record_command(command)
        self.mysql_process = subprocess.Popen(command)
        self.wait_for(
            [
                "mysqladmin",
                "--no-defaults",
                f"--socket={socket}",
                "--user=root",
                "ping",
            ],
            30,
        )
        return data, socket

    def restore_mysql(self, dump: Path, socket: Path) -> None:
        command = ["mysql", "--no-defaults", f"--socket={socket}", "--user=root"]
        self.record_command(command)
        process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stderr=subprocess.PIPE
        )
        assert process.stdin is not None
        assert process.stderr is not None
        with gzip.open(dump, "rb") as source:
            shutil.copyfileobj(source, process.stdin)
        process.stdin.close()
        error = process.stderr.read().decode("utf-8", errors="replace").strip()
        if process.wait() != 0:
            raise SafetyRefusal(f"isolated_mysql_restore_failed:{error}")

    def start_isolated_redis(self, rehearsal: Path, dump: Path) -> tuple[Path, Path]:
        data = require_isolated_path(self.operation, rehearsal / "redis-data")
        socket = require_isolated_path(self.operation, rehearsal / "redis.sock")
        data.mkdir()
        shutil.copy2(dump, data / "dump.rdb")
        command = [
            "redis-server",
            "--port",
            "0",
            "--unixsocket",
            str(socket),
            "--unixsocketperm",
            "700",
            "--dir",
            str(data),
            "--dbfilename",
            "dump.rdb",
            "--appendonly",
            "no",
        ]
        self.record_command(command)
        self.redis_log = (rehearsal / "redis.log").open("wb")
        self.redis_process = subprocess.Popen(
            command, stdout=self.redis_log, stderr=subprocess.STDOUT
        )
        self.wait_for(["redis-cli", "-s", str(socket), "PING"], 15)
        return data, socket

    def stop_rehearsal(
        self, mysql_socket: Path | None, redis_socket: Path | None
    ) -> None:
        if redis_socket is not None and redis_socket.exists():
            subprocess.run(
                ["redis-cli", "-s", str(redis_socket), "SHUTDOWN", "NOSAVE"],
                check=False,
                capture_output=True,
            )
        if self.redis_process is not None:
            self._stop_process(self.redis_process, 10)
        if self.redis_log is not None:
            self.redis_log.close()
        if mysql_socket is not None and mysql_socket.exists():
            subprocess.run(
                [
                    "mysqladmin",
                    "--no-defaults",
                    f"--socket={mysql_socket}",
                    "--user=root",
                    "shutdown",
                ],
                check=False,
                capture_output=True,
            )
        if self.mysql_process is not None:
            self._stop_process(self.mysql_process, 30)

    @staticmethod
    def _stop_process(process: subprocess.Popen[bytes], timeout: int) -> None:
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)

    def rehearse(
        self, frozen: dict[str, Any], mysql_dump: Path, redis_dump: Path
    ) -> None:
        rehearsal = require_isolated_path(self.operation, self.operation / "rehearsal")
        rehearsal.mkdir()
        mysql_data: Path | None = None
        redis_data: Path | None = None
        mysql_socket: Path | None = None
        redis_socket: Path | None = None
        try:
            mysql_data, mysql_socket = self.start_isolated_mysql(rehearsal)
            self.restore_mysql(mysql_dump, mysql_socket)
            redis_data, redis_socket = self.start_isolated_redis(rehearsal, redis_dump)
            restored_online = self.protected_online_state(mysql_socket)
            restored = audit_until_stable(
                lambda attempt: self.capture_audit(
                    "restored",
                    attempt,
                    mysql_socket=mysql_socket,
                    redis_socket=redis_socket,
                ),
                self.arguments.audit_attempts,
            )
            validate_restored_report(frozen, restored)
            restored_path = self.operation / "restored-manifest.json"
            restored_path.write_text(canonical_json(restored), encoding="utf-8")
            comparison = self.operation / "restore-comparison.json"
            self.command(
                [
                    sys.executable,
                    str(Path(__file__).with_name("population_manifest.py")),
                    "compare",
                    str(self.operation / "frozen-manifest.json"),
                    str(restored_path),
                    "--output",
                    str(comparison),
                ]
            )
            self.record["restore"] = {
                "isolated_mysql": {
                    "skip_networking": True,
                    "socket": str(mysql_socket),
                },
                "isolated_redis": {"port": 0, "socket": str(redis_socket)},
                "digest": restored["digest"],
                "counts": restored["counts"],
                "protected_online_state": restored_online,
                "comparison_sha256": sha256_file(comparison),
                "manifest_sha256": sha256_file(restored_path),
                "rollback_feasible": True,
            }
            self.write_record()
        finally:
            self.stop_rehearsal(mysql_socket, redis_socket)
            for data in (mysql_data, redis_data):
                if data is not None and data.exists():
                    require_isolated_path(self.operation, data)
                    shutil.rmtree(data)

    def restore_prior_service_state(self) -> None:
        errors = []
        restarted_labels = []
        if self.stopped_labels:
            for label in reversed(self.stopped_labels):
                service = self.record["services"][label]
                if not self.wait_for_service_unload(label):
                    errors.append(f"release_timeout:{label}")
                    continue
                bootstrap_error = self.bootstrap_service(label, service["plist"])
                if bootstrap_error is None:
                    restarted_labels.append(label)
                else:
                    errors.append(bootstrap_error)
        if self.maintenance_enabled:
            completed = subprocess.run(
                [self.arguments.php, "artisan", "up", "--no-ansi"],
                cwd=Path(self.arguments.medivh_root),
                check=False,
                capture_output=True,
                text=True,
            )
            if completed.returncode != 0:
                errors.append(f"medivh_up:{completed.stderr.strip()}")
        for label in restarted_labels:
            deadline = time.monotonic() + self.arguments.stop_timeout_seconds
            while time.monotonic() < deadline:
                completed = subprocess.run(
                    ["launchctl", "print", f"gui/{self.uid}/{label}"],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                if completed.returncode == 0 and "state = running" in completed.stdout:
                    break
                time.sleep(1)
            else:
                errors.append(f"restart_timeout:{label}")
        if errors:
            raise SafetyRefusal("prior_state_restore_failed:" + "|".join(errors))

    def verify_final_service_state(self) -> None:
        for label, _ in self.arguments.writer_service:
            completed = self.command(
                ["launchctl", "print", f"gui/{self.uid}/{label}"],
                accepted=frozenset(range(256)),
            )
            if completed.returncode == 0:
                raise SafetyRefusal(f"writer_service_reappeared:{label}")
        for label, plist_path in self.arguments.required_running_service:
            self.service_state(label, plist_path)
        if not (Path(self.arguments.medivh_root) / "storage/framework/down").is_file():
            raise SafetyRefusal("medivh_maintenance_missing")

    def preserve_evidence_hashes(self) -> None:
        excluded = {"operation-record.json", "SHA256SUMS"}
        hashes = {
            path.relative_to(self.operation).as_posix(): sha256_file(path)
            for path in sorted(self.operation.rglob("*"))
            if path.is_file() and path.name not in excluded
        }
        (self.operation / "SHA256SUMS").write_text(
            "".join(f"{digest}  {name}\n" for name, digest in hashes.items()),
            encoding="utf-8",
        )
        self.record["evidence_sha256"] = hashes

    def execute(self) -> None:
        success = False
        try:
            self.preflight()
            self.stop_writers()
            frozen = self.freeze()
            mysql_dump, redis_dump = self.backup()
            self.rehearse(frozen, mysql_dump, redis_dump)
            self.verify_final_service_state()
            self.preserve_evidence_hashes()
            self.record["status"] = "VERIFIED_WRITERS_STOPPED"
            self.record["completed_at"] = datetime.now(timezone.utc).isoformat()
            self.record["final_services"] = {
                "stopped": sorted(label for label, _ in self.arguments.writer_service),
                "running": sorted(
                    label for label, _ in self.arguments.required_running_service
                ),
                "medivh_maintenance": True,
            }
            self.write_record()
            success = True
        except BaseException as error:
            self.record["status"] = "FAILED"
            self.record["failure"] = f"{type(error).__name__}:{error}"
            self.write_record()
            raise
        finally:
            if not success and (self.stopped_labels or self.maintenance_enabled):
                self.restore_prior_service_state()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Freeze, back up, and rehearse restoration of Playerbot population state."
    )
    parser.add_argument("--backup-root", required=True)
    parser.add_argument("--run-name", required=True)
    parser.add_argument("--playerbots-config", required=True)
    parser.add_argument("--azerothcore-root", required=True)
    parser.add_argument("--medivh-root", required=True)
    parser.add_argument("--php", required=True)
    parser.add_argument(
        "--writer-service", action="append", required=True, type=writer_spec
    )
    parser.add_argument(
        "--required-running-service", action="append", required=True, type=writer_spec
    )
    parser.add_argument("--repository", action="append", default=[])
    parser.add_argument("--artifact", action="append", default=[])
    parser.add_argument("--mysql-defaults-group-suffix", default="root")
    parser.add_argument("--redis-host", default="127.0.0.1")
    parser.add_argument("--redis-port", default=6379, type=int)
    parser.add_argument("--audit-attempts", default=3, type=int)
    parser.add_argument("--stop-timeout-seconds", default=180, type=int)
    return parser


def main(arguments: list[str] | None = None) -> int:
    try:
        operation = PopulationBackupOperation(build_parser().parse_args(arguments))
        operation.execute()
    except (SafetyRefusal, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 4
    except KeyboardInterrupt:
        signal.signal(signal.SIGINT, signal.SIG_DFL)
        print("REFUSED: interrupted", file=sys.stderr)
        return 130
    print(operation.operation)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
