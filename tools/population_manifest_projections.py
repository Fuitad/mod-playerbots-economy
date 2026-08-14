from __future__ import annotations

import argparse
import json
import re
from collections.abc import Callable
from pathlib import Path
from typing import Any

CommandRunner = Callable[[list[str]], str]


def projection_provenance(
    arguments: argparse.Namespace, inventory: dict[str, Any]
) -> tuple[dict[str, Any], list[str], list[str]]:
    if not arguments.medivh_root:
        return {}, [], ["projection.medivh_env"]
    env_path = Path(arguments.medivh_root).resolve() / ".env"
    if not env_path.is_file():
        return {}, [], ["projection.medivh_env"]
    config = inventory["projection_config"]
    wanted = {
        config["redis_host_env"],
        config["redis_port_env"],
        *config["redis_stream_env"],
    }
    values: dict[str, str] = {}
    for line in env_path.read_text(encoding="utf-8").splitlines():
        if "=" not in line or line.lstrip().startswith("#"):
            continue
        key, value = line.split("=", 1)
        if key in wanted:
            values[key] = value.strip().strip('"').strip("'")
    host = values.get(config["redis_host_env"], config["redis_host_default"])
    port_text = values.get(config["redis_port_env"], str(config["redis_port_default"]))
    try:
        port = int(port_text)
    except ValueError:
        return {}, ["projection.redis_port"], []
    streams = {
        env_name: values.get(env_name, default)
        for env_name, default in config["redis_stream_env"].items()
    }
    expected_streams = set(inventory["projection_keys"]["redis_streams"])
    actual_streams = set(streams.values())
    expected_strings = set(inventory["projection_keys"]["redis_strings"])
    actual_strings = {
        streams[config["social_stream_env"]] + config["social_cursor_suffix"]
    }
    drift = [
        *(
            f"projection.redis_stream.{key}"
            for key in expected_streams ^ actual_streams
        ),
        *(
            f"projection.redis_string.{key}"
            for key in expected_strings ^ actual_strings
        ),
    ]
    if str(arguments.redis_host) != host:
        drift.append("projection.redis_host")
    if int(arguments.redis_port) != port:
        drift.append("projection.redis_port")
    return (
        {
            "env_path": str(env_path),
            "redis_host": host,
            "redis_port": port,
            "streams": streams,
        },
        sorted(set(drift)),
        [],
    )


def _redis_command(
    arguments: argparse.Namespace, runner: CommandRunner, command: list[str]
) -> str:
    return runner(
        [
            "redis-cli",
            "--json",
            "-h",
            str(arguments.redis_host),
            "-p",
            str(arguments.redis_port),
            *command,
        ]
    )


def _redis_entries(
    arguments: argparse.Namespace, runner: CommandRunner, stream: str
) -> list[dict[str, Any]]:
    output = _redis_command(arguments, runner, ["XRANGE", stream, "-", "+"])
    entries: list[dict[str, Any]] = []
    for entry_id, values in json.loads(output or "[]"):
        fields = dict(zip(values[0::2], values[1::2], strict=True))
        entries.append({"entry_id": entry_id, "fields": fields})
    return entries


def _redis_groups(
    arguments: argparse.Namespace, runner: CommandRunner, stream: str
) -> list[dict[str, Any]]:
    output = _redis_command(arguments, runner, ["XINFO", "GROUPS", stream])
    if output.strip().startswith('error:"ERR no such key"'):
        return []
    decoded = json.loads(output or "[]")
    if isinstance(decoded, dict):
        error = decoded.get("error")
        if isinstance(error, str) and "no such key" in error.lower():
            return []
        raise ValueError(
            f"Redis XINFO failed for {stream}: {error or 'invalid response'}"
        )
    if not isinstance(decoded, list):
        raise TypeError(f"Redis XINFO returned an invalid response for {stream}")
    return [dict(zip(values[0::2], values[1::2], strict=True)) for values in decoded]


def _redis_pending(
    arguments: argparse.Namespace,
    runner: CommandRunner,
    stream: str,
    group: str,
) -> list[dict[str, Any]]:
    pending: list[dict[str, Any]] = []
    start = "-"
    while True:
        output = _redis_command(
            arguments,
            runner,
            ["XPENDING", stream, group, start, "+", "1000"],
        )
        rows = json.loads(output or "[]")
        pending.extend(
            {
                "consumer": row[1],
                "delivery_count": row[3],
                "entry_id": row[0],
                "idle_milliseconds": row[2],
            }
            for row in rows
        )
        if len(rows) < 1000:
            return pending
        start = f"({rows[-1][0]}"


def _entry_matches(entry: dict[str, Any], identities: set[str]) -> bool:
    serialized = json.dumps(entry["fields"], ensure_ascii=False, sort_keys=True)
    return identity_in_text(serialized, identities)


def identity_in_text(text: str, identities: set[str]) -> bool:
    return any(
        re.search(rf"(?<![A-Za-z0-9]){re.escape(identity)}(?![A-Za-z0-9])", text)
        for identity in identities
    )


def capture_redis(
    arguments: argparse.Namespace,
    projection_keys: dict[str, Any],
    target_identities: set[str],
    protected_identities: set[str],
    runner: CommandRunner,
) -> tuple[dict[str, list[Any]], dict[str, list[Any]], dict[str, list[Any]]]:
    derived = {
        "redis_pending_entries": [],
        "redis_social_entries": [],
        "redis_telemetry_entries": [],
    }
    surfaces: dict[str, list[Any]] = {}
    protected_surfaces: dict[str, list[Any]] = {}
    for stream in projection_keys["redis_streams"]:
        entries = _redis_entries(arguments, runner, stream)
        key = (
            "redis_telemetry_entries"
            if stream.endswith("telemetry")
            else "redis_social_entries"
        )
        derived[key] = [
            entry for entry in entries if _entry_matches(entry, target_identities)
        ]
        surfaces[f"redis.stream.{stream}"] = derived[key]
        protected_entries = [
            entry for entry in entries if _entry_matches(entry, protected_identities)
        ]
        protected_surfaces[f"redis.stream.{stream}"] = protected_entries
        groups = _redis_groups(arguments, runner, stream)
        surfaces[f"redis.groups.{stream}"] = groups
        target_entry_ids = {entry["entry_id"] for entry in derived[key]}
        protected_entry_ids = {entry["entry_id"] for entry in protected_entries}
        for group in groups:
            pending = _redis_pending(arguments, runner, stream, str(group["name"]))
            target_pending = [
                row for row in pending if row["entry_id"] in target_entry_ids
            ]
            derived["redis_pending_entries"].extend(
                {"group": group["name"], "stream": stream, **row}
                for row in target_pending
            )
            surfaces[f"redis.pending.{stream}.{group['name']}"] = target_pending
            protected_surfaces[f"redis.pending.{stream}.{group['name']}"] = [
                row for row in pending if row["entry_id"] in protected_entry_ids
            ]
    for key in projection_keys["redis_strings"]:
        value = runner(
            [
                "redis-cli",
                "--raw",
                "-h",
                str(arguments.redis_host),
                "-p",
                str(arguments.redis_port),
                "GET",
                key,
            ]
        ).rstrip("\n")
        surfaces[f"redis.string.{key}"] = (
            [] if not value else [{"key": key, "value": value}]
        )
    return derived, surfaces, protected_surfaces
