from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from population_manifest_projections import (
    capture_redis,
    identity_in_text,
    projection_provenance,
)

MYSQL_SCHEMAS = ("acore_auth", "acore_characters", "acore_playerbots", "medivh")
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
CONFIG_PREFIX = re.compile(
    r'^\s*AiPlayerbot\.RandomBotAccountPrefix\s*=\s*"([^"]+)"\s*$'
)


class CaptureRefusal(RuntimeError):
    pass


def _canonical_digest(value: Any) -> str:
    rendered = json.dumps(
        value, ensure_ascii=False, separators=(",", ":"), sort_keys=True
    )
    return hashlib.sha256(rendered.encode("utf-8")).hexdigest()


def _run(command: list[str], *, stdin: str | None = None) -> str:
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        input=stdin,
        text=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise CaptureRefusal(f"command_failed:{command[0]}:{detail}")
    return completed.stdout


def _mysql(arguments: argparse.Namespace, sql: str) -> str:
    suffix = str(arguments.mysql_defaults_group_suffix)
    if not re.fullmatch(r"[A-Za-z0-9_]+", suffix):
        raise CaptureRefusal("invalid_mysql_defaults_group_suffix")
    return _run(
        [
            "mysql",
            f"--defaults-group-suffix={suffix}",
            "--batch",
            "--raw",
            "--skip-column-names",
        ],
        stdin=sql,
    )


def _identifier(value: str) -> str:
    if IDENTIFIER.fullmatch(value) is None:
        raise CaptureRefusal(f"invalid_schema_identifier:{value}")
    return f"`{value}`"


def load_inventory(path: str | Path) -> dict[str, Any]:
    with Path(path).open(encoding="utf-8") as handle:
        inventory = json.load(handle)
    if inventory.get("version") != 1:
        raise CaptureRefusal("unsupported_inventory_version")
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


def read_schema(arguments: argparse.Namespace) -> list[dict[str, str]]:
    schemas = ",".join(f"'{schema}'" for schema in MYSQL_SCHEMAS)
    output = _mysql(
        arguments,
        "SELECT TABLE_SCHEMA,TABLE_NAME,COLUMN_NAME,COLUMN_TYPE,IS_NULLABLE,COLUMN_KEY,EXTRA "
        "FROM information_schema.COLUMNS "
        f"WHERE TABLE_SCHEMA IN ({schemas}) "
        "ORDER BY TABLE_SCHEMA,TABLE_NAME,ORDINAL_POSITION;\n",
    )
    rows: list[dict[str, str]] = []
    fields = ("schema", "table", "column", "column_type", "nullable", "key", "extra")
    for line in output.splitlines():
        values = line.split("\t")
        if len(values) != len(fields):
            raise CaptureRefusal("malformed_information_schema_row")
        rows.append(dict(zip(fields, values, strict=True)))
    return rows


def schema_hashes(schema_rows: list[dict[str, str]]) -> dict[str, str]:
    return {
        schema: _canonical_digest(
            [row for row in schema_rows if row["schema"] == schema]
        )
        for schema in MYSQL_SCHEMAS
    }


def validate_inventory(
    inventory: dict[str, Any], schema_rows: list[dict[str, str]]
) -> tuple[list[str], dict[tuple[str, str], list[str]]]:
    available = {
        (row["schema"], row["table"], row["column"]): row for row in schema_rows
    }
    primary_keys: dict[tuple[str, str], list[str]] = {}
    for row in schema_rows:
        if row["key"] == "PRI":
            primary_keys.setdefault((row["schema"], row["table"]), []).append(
                row["column"]
            )

    unknown: list[str] = []
    expected_hashes = inventory.get("schema_hashes", {})
    for schema, actual_hash in schema_hashes(schema_rows).items():
        if expected_hashes.get(schema) != actual_hash:
            unknown.append(f"schema.{schema}")

    raw_edge_keys = [edge["key"] for edge in inventory["edges"]]
    edge_keys = set(raw_edge_keys)
    confirmed_edge_keys = set(inventory.get("confirmed_edge_keys", []))
    if len(raw_edge_keys) != len(edge_keys):
        unknown.append("inventory.duplicate_edge_key")
    for key in confirmed_edge_keys:
        if key not in edge_keys:
            unknown.append(f"inventory.missing_edge.{key}")
    for key in edge_keys - confirmed_edge_keys:
        unknown.append(f"inventory.unconfirmed_edge.{key}")

    for edge in expand_edges(inventory):
        schema = edge["schema"]
        table = edge["table"]
        required = set(edge.get("identity_columns", []))
        for selector in edge.get("selectors", []):
            required.update(selector["columns"])
        required.update(edge.get("static_values", {}).keys())
        if edge.get("source") not in inventory.get("source_artifacts", {}):
            unknown.append(f"inventory.unknown_source.{edge['key']}")
        selectors = edge.get("selectors", [])
        classified = bool(
            selectors or edge.get("static_values") or edge.get("capture_all")
        )
        if not classified:
            unknown.append(f"inventory.unclassified_edge.{edge['key']}")
        for column in sorted(required):
            if (schema, table, column) not in available:
                unknown.append(f"{schema}.{table}.{column}")
        if not edge.get("identity_columns") and not primary_keys.get((schema, table)):
            selector_columns = {
                column
                for selector in edge.get("selectors", [])
                for column in selector["columns"]
            }
            if not selector_columns:
                unknown.append(f"identity.{schema}.{table}")
    return sorted(set(unknown)), primary_keys


def _read_account_prefix(path: str | Path) -> tuple[str, str]:
    config_path = Path(path)
    content = config_path.read_text(encoding="utf-8")
    matches = [
        match.group(1)
        for line in content.splitlines()
        if (match := CONFIG_PREFIX.match(line))
    ]
    if len(matches) != 1 or not matches[0]:
        raise CaptureRefusal("account_prefix_missing_or_ambiguous")
    return matches[0], hashlib.sha256(content.encode("utf-8")).hexdigest()


def _source_roots(arguments: argparse.Namespace) -> dict[str, Path]:
    module_root = Path(__file__).resolve().parents[1]
    azerothcore = (
        Path(arguments.azerothcore_root).resolve()
        if arguments.azerothcore_root
        else module_root.parents[1]
    )
    roots = {
        "azerothcore": azerothcore,
        "mod-playerbots-economy": module_root,
    }
    for name in (
        "mod-playerbots",
        "mod-playerbots-llm",
        "mod-playerbots-lifecycle",
        "mod-playerbots-personality",
        "mod-playerbots-social",
    ):
        roots[name] = azerothcore / "modules" / name
    if arguments.medivh_root:
        roots["medivh"] = Path(arguments.medivh_root).resolve()
    return roots


def source_provenance(
    arguments: argparse.Namespace, inventory: dict[str, Any], config_hash: str
) -> tuple[dict[str, Any], list[str], list[str]]:
    roots = _source_roots(arguments)
    sources: dict[str, Any] = {
        "playerbots_config": {
            "path": str(Path(arguments.playerbots_config).resolve()),
            "sha256": config_hash,
        }
    }
    unavailable: list[str] = []
    source_drift: list[str] = []
    revisions: dict[str, str] = {}
    expected_hashes = inventory.get("source_hashes", {})
    for label, artifact in sorted(inventory["source_artifacts"].items()):
        repository, relative_path = artifact.split(":", 1)
        root = roots.get(repository)
        if root is None:
            unavailable.append(f"source.{label}")
            continue
        path = root / relative_path
        if not path.is_file():
            unavailable.append(f"source.{label}")
            continue
        if repository not in revisions:
            revisions[repository] = _run(
                ["git", "-C", str(root), "rev-parse", "HEAD"]
            ).strip()
        artifact_hash = hashlib.sha256(path.read_bytes()).hexdigest()
        sources[label] = {
            "artifact": artifact,
            "git_revision": revisions[repository],
            "sha256": artifact_hash,
        }
        if expected_hashes.get(label) != artifact_hash:
            source_drift.append(f"source.{label}")
    for label in set(expected_hashes) - set(inventory["source_artifacts"]):
        source_drift.append(f"source.removed.{label}")
    return sources, sorted(unavailable), sorted(set(source_drift))


def _cte(
    protected_account_id: int | None = None,
    protected_character_guid: int | None = None,
) -> str:
    if protected_account_id is None and protected_character_guid is None:
        account_query = (
            "SELECT account_id FROM acore_playerbots.playerbots_account_type "
            "WHERE account_type IN (1, 2)"
        )
        character_predicate = (
            "account IN (SELECT account_id FROM target_accounts) "
            "OR deleteInfos_Account IN (SELECT account_id FROM target_accounts)"
        )
    elif protected_account_id is not None and protected_character_guid is not None:
        account_query = (
            "SELECT id AS account_id FROM acore_auth.account "
            f"WHERE id={protected_account_id}"
        )
        character_predicate = (
            f"guid={protected_character_guid} AND account={protected_account_id} "
            "AND deleteInfos_Account IS NULL"
        )
    else:
        raise CaptureRefusal("incomplete_protected_cte_binding")
    return f"""
WITH target_accounts AS (
    {account_query}
), target_names AS (
    SELECT username FROM acore_auth.account WHERE id IN (SELECT account_id FROM target_accounts)
), target_characters AS (
    SELECT guid FROM acore_characters.characters
    WHERE {character_predicate}
), target_items AS (
    SELECT guid FROM acore_characters.item_instance
    WHERE owner_guid IN (SELECT guid FROM target_characters)
), target_auctions AS (
    SELECT id FROM acore_characters.auctionhouse
    WHERE itemowner IN (SELECT guid FROM target_characters)
       OR buyguid IN (SELECT guid FROM target_characters)
       OR itemguid IN (SELECT guid FROM target_items)
), target_mail AS (
    SELECT id FROM acore_characters.mail
    WHERE sender IN (SELECT guid FROM target_characters)
       OR receiver IN (SELECT guid FROM target_characters)
       OR id IN (
           SELECT mail_id FROM acore_characters.mail_items
           WHERE item_guid IN (SELECT guid FROM target_items)
       )
), target_guilds AS (
    SELECT guildid FROM acore_characters.guild_member
    WHERE guid IN (SELECT guid FROM target_characters)
), target_pets AS (
    SELECT id FROM acore_characters.character_pet
    WHERE owner IN (SELECT guid FROM target_characters)
), target_positions AS (
    SELECT public_id FROM acore_playerbots.playerbot_economy_position
    WHERE trader_guid IN (SELECT guid FROM target_characters)
), target_social_actors AS (
    SELECT id FROM acore_playerbots.playerbot_social_actor
    WHERE character_guid IN (SELECT guid FROM target_characters)
)
""".strip()


def _json_object(alias: str, columns: list[str]) -> str:
    arguments: list[str] = []
    for column in columns:
        arguments.extend((f"'{column}'", f"{alias}.{_identifier(column)}"))
    return f"JSON_OBJECT({','.join(arguments)})"


def _emit_query(section: str, query: str, row_expression: str) -> str:
    escaped = section.replace("'", "''")
    return (
        f"SELECT CONCAT('__PM__', JSON_OBJECT('section','{escaped}','row',row_data)) "
        f"FROM ({query}) population_manifest_rows;\n"
    ).replace("row_data", row_expression)


def _core_queries(protected_account_id: int, protected_guid: int) -> list[str]:
    cte = _cte()
    return [
        _emit_query(
            "account_rows",
            "SELECT a.id AS account_id,a.username FROM acore_auth.account a",
            "JSON_OBJECT('account_id',account_id,'username',username)",
        ),
        _emit_query(
            "ownership_rows",
            "SELECT account_id,account_type FROM acore_playerbots.playerbots_account_type",
            "JSON_OBJECT('account_id',account_id,'account_type',account_type)",
        ),
        _emit_query(
            "character_rows",
            f"{cte} SELECT guid AS character_guid,account AS account_id,deleteInfos_Account AS "
            "delete_account_id,name FROM acore_characters.characters WHERE guid IN "
            f"(SELECT guid FROM target_characters) OR guid={protected_guid}",
            "JSON_OBJECT('character_guid',character_guid,'account_id',account_id,"
            "'delete_account_id',delete_account_id,'name',name)",
        ),
        _emit_query(
            "item_owner_rows",
            f"{cte} SELECT guid AS item_guid,owner_guid FROM acore_characters.item_instance "
            "WHERE guid IN (SELECT guid FROM target_items)",
            "JSON_OBJECT('item_guid',item_guid,'owner_guid',owner_guid)",
        ),
        _emit_query(
            "item_location_rows",
            f"{cte} SELECT 'inventory' AS surface,item AS item_guid,guid AS holder_id "
            "FROM acore_characters.character_inventory WHERE guid IN (SELECT guid FROM target_characters) "
            "UNION ALL SELECT 'mail',item_guid,mail_id FROM acore_characters.mail_items "
            "WHERE mail_id IN (SELECT id FROM target_mail) "
            "UNION ALL SELECT 'auction',itemguid,id FROM acore_characters.auctionhouse "
            "WHERE id IN (SELECT id FROM target_auctions) "
            "UNION ALL SELECT 'guild_bank',item_guid,guildid FROM acore_characters.guild_bank_item "
            "WHERE guildid IN (SELECT guildid FROM target_guilds)",
            "JSON_OBJECT('surface',surface,'item_guid',item_guid,'holder_id',holder_id)",
        ),
        _emit_query(
            "auction_rows",
            f"{cte} SELECT a.id AS auction_id,a.houseid AS house_id,a.itemguid AS item_guid,"
            "i.itemEntry AS item_entry,i.count AS item_count,a.itemowner AS owner_guid,"
            "COALESCE(NULLIF(owner.account,0),owner.deleteInfos_Account) AS owner_account_id,"
            "a.buyoutprice AS buyout_price,a.time AS expires_at,a.buyguid AS bidder_guid,"
            "COALESCE(NULLIF(bidder.account,0),bidder.deleteInfos_Account) AS bidder_account_id,"
            "a.lastbid AS last_bid,a.startbid AS start_bid,a.deposit "
            "FROM acore_characters.auctionhouse a "
            "LEFT JOIN acore_characters.item_instance i ON i.guid=a.itemguid "
            "LEFT JOIN acore_characters.characters owner ON owner.guid=a.itemowner "
            "LEFT JOIN acore_characters.characters bidder ON bidder.guid=a.buyguid "
            "WHERE a.id IN (SELECT id FROM target_auctions)",
            "JSON_OBJECT('auction_id',auction_id,'house_id',house_id,'item_guid',item_guid,"
            "'item_entry',item_entry,'item_count',item_count,'owner_guid',owner_guid,"
            "'owner_account_id',owner_account_id,'bidder_guid',bidder_guid,"
            "'bidder_account_id',bidder_account_id,'buyout_price',buyout_price,"
            "'expires_at',expires_at,'last_bid',last_bid,'start_bid',start_bid,"
            "'deposit',deposit)",
        ),
        _emit_query(
            "mail_rows",
            f"{cte} SELECT m.id AS mail_id,m.messageType AS message_type,m.stationery,"
            "m.mailTemplateId AS template_id,m.sender AS sender_guid,m.receiver AS receiver_guid,"
            "COALESCE(NULLIF(sender.account,0),sender.deleteInfos_Account) AS sender_account_id,"
            "COALESCE(NULLIF(receiver.account,0),receiver.deleteInfos_Account) AS receiver_account_id,"
            "SHA2(COALESCE(m.subject,''),256) AS subject_sha256,"
            "SHA2(COALESCE(m.body,''),256) AS body_sha256,m.has_items,m.expire_time,"
            "m.deliver_time,m.money,m.cod,m.checked,"
            "COALESCE((SELECT JSON_ARRAYAGG(mi.item_guid) FROM acore_characters.mail_items mi "
            "WHERE mi.mail_id=m.id),JSON_ARRAY()) AS item_guids FROM acore_characters.mail m "
            "LEFT JOIN acore_characters.characters sender ON sender.guid=m.sender "
            "LEFT JOIN acore_characters.characters receiver ON receiver.guid=m.receiver "
            "WHERE m.id IN (SELECT id FROM target_mail)",
            "JSON_OBJECT('mail_id',mail_id,'message_type',message_type,'stationery',stationery,"
            "'template_id',template_id,'sender_guid',sender_guid,"
            "'sender_account_id',sender_account_id,'receiver_guid',receiver_guid,"
            "'receiver_account_id',receiver_account_id,"
            "'subject_sha256',subject_sha256,'body_sha256',body_sha256,'has_items',has_items,"
            "'expire_time',expire_time,'deliver_time',deliver_time,'money',money,'cod',cod,"
            "'checked',checked,'item_guids',item_guids)",
        ),
        _emit_query(
            "derived.economy_positions",
            f"{cte} SELECT public_id,trader_guid AS character_guid "
            "FROM acore_playerbots.playerbot_economy_position "
            "WHERE trader_guid IN (SELECT guid FROM target_characters)",
            "JSON_OBJECT('public_id',public_id,'character_guid',character_guid)",
        ),
        _emit_query(
            "derived.social_actors",
            f"{cte} SELECT id AS actor_id,public_id,character_guid "
            "FROM acore_playerbots.playerbot_social_actor "
            "WHERE id IN (SELECT id FROM target_social_actors)",
            "JSON_OBJECT('actor_id',actor_id,'public_id',public_id,"
            "'character_guid',character_guid)",
        ),
        _emit_query(
            "derived.playerbots",
            f"{cte} SELECT id,owner,bot FROM acore_playerbots.playerbots_random_bots "
            "WHERE owner IN (SELECT guid FROM target_characters) "
            "OR bot IN (SELECT guid FROM target_characters)",
            "JSON_OBJECT('id',id,'owner_guid',owner,'bot_guid',bot)",
        ),
        _emit_query(
            "derived.personalities",
            f"{cte} SELECT character_guid FROM acore_playerbots.playerbot_personality "
            "WHERE character_guid IN (SELECT guid FROM target_characters)",
            "JSON_OBJECT('character_guid',character_guid)",
        ),
        _emit_query(
            "derived.careers",
            f"{cte} SELECT bot_guid,career_version FROM "
            "acore_playerbots.playerbot_llm_career_decision "
            "WHERE bot_guid IN (SELECT guid FROM target_characters)",
            "JSON_OBJECT('bot_guid',bot_guid,'career_version',career_version)",
        ),
        _emit_query(
            "derived.llm_actors",
            f"{cte} SELECT DISTINCT source_table,bot_guid FROM ("
            "SELECT 'playerbot_llm_bot_purge' AS source_table,bot_guid "
            "FROM acore_playerbots.playerbot_llm_bot_purge UNION ALL "
            "SELECT 'playerbot_llm_career_decision',bot_guid "
            "FROM acore_playerbots.playerbot_llm_career_decision UNION ALL "
            "SELECT 'playerbot_llm_conversation_turn',bot_guid "
            "FROM acore_playerbots.playerbot_llm_conversation_turn UNION ALL "
            "SELECT 'playerbot_llm_profile',bot_guid "
            "FROM acore_playerbots.playerbot_llm_profile) llm "
            "WHERE bot_guid IN (SELECT guid FROM target_characters)",
            "JSON_OBJECT('source_table',source_table,'bot_guid',bot_guid)",
        ),
        _emit_query(
            "derived.medivh_observed_bots",
            f"{cte} SELECT id,public_id,azeroth_guid AS character_guid FROM medivh.observed_bots "
            "WHERE azeroth_guid IN (SELECT guid FROM target_characters)",
            "JSON_OBJECT('id',id,'public_id',public_id,'character_guid',character_guid)",
        ),
    ]


def _selector_condition(selector: dict[str, Any]) -> str:
    targets = {
        "account": "SELECT account_id FROM target_accounts",
        "account_name": "SELECT username FROM target_names",
        "auction": "SELECT id FROM target_auctions",
        "character": "SELECT guid FROM target_characters",
        "economy_position": "SELECT public_id FROM target_positions",
        "item": "SELECT guid FROM target_items",
        "mail": "SELECT id FROM target_mail",
        "pet": "SELECT id FROM target_pets",
        "social_actor": "SELECT id FROM target_social_actors",
    }
    kind = selector["kind"]
    if kind not in targets:
        raise CaptureRefusal(f"unknown_selector_kind:{kind}")
    return (
        "("
        + " OR ".join(
            f"t.{_identifier(column)} IN ({targets[kind]})"
            for column in selector["columns"]
        )
        + ")"
    )


def _edge_query(
    edge: dict[str, Any],
    primary_keys: dict[tuple[str, str], list[str]],
    *,
    cte: str | None = None,
    section_prefix: str = "surface.",
) -> str:
    schema = edge["schema"]
    table = edge["table"]
    identity_columns = edge.get("identity_columns") or primary_keys.get((schema, table))
    if not identity_columns:
        identity_columns = sorted(
            {
                column
                for selector in edge.get("selectors", [])
                for column in selector["columns"]
            }
        )
    conditions = [
        _selector_condition(selector) for selector in edge.get("selectors", [])
    ]
    for column, values in edge.get("static_values", {}).items():
        literals = ",".join(
            "'" + str(value).replace("'", "''") + "'" for value in values
        )
        conditions.append(f"t.{_identifier(column)} IN ({literals})")
    where = "1=1" if edge.get("capture_all") else " OR ".join(conditions)
    if not where:
        raise CaptureRefusal(f"edge_without_selector:{edge['key']}")
    query = (
        f"{cte or _cte()} SELECT {_json_object('t', identity_columns)} AS identity "
        f"FROM {_identifier(schema)}.{_identifier(table)} t WHERE {where}"
    )
    return _emit_query(f"{section_prefix}{edge['key']}", query, "identity")


def build_capture_sql(
    inventory: dict[str, Any],
    primary_keys: dict[tuple[str, str], list[str]],
    protected_account_id: int,
    protected_guid: int,
) -> str:
    queries = _core_queries(protected_account_id, protected_guid)
    expanded = expand_edges(inventory)
    queries.extend(_edge_query(edge, primary_keys) for edge in expanded)
    protected_cte = _cte(protected_account_id, protected_guid)
    queries.extend(
        _edge_query(
            edge,
            primary_keys,
            cte=protected_cte,
            section_prefix="protected_surface.",
        )
        for edge in expanded
        if not edge.get("capture_all")
    )
    body = "".join(queries)
    return (
        "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ;\n"
        "SET SESSION TRANSACTION READ ONLY;\n"
        "START TRANSACTION WITH CONSISTENT SNAPSHOT;\n" + body + "COMMIT;\n"
    )


def parse_capture_rows(output: str) -> dict[str, Any]:
    sections: dict[str, list[Any]] = {}
    for line in output.splitlines():
        if not line.startswith("__PM__"):
            continue
        decoded = json.loads(line[len("__PM__") :])
        sections.setdefault(decoded["section"], []).append(decoded["row"])
    return sections


def capture_once(arguments: argparse.Namespace) -> dict[str, Any]:
    inventory = load_inventory(arguments.inventory)
    prefix, config_hash = _read_account_prefix(arguments.playerbots_config)
    schema_rows = read_schema(arguments)
    unknown_edges, primary_keys = validate_inventory(inventory, schema_rows)
    sources, unavailable_sources, source_drift = source_provenance(
        arguments, inventory, config_hash
    )
    projection_sources, projection_drift, projection_unavailable = (
        projection_provenance(arguments, inventory)
    )
    unknown_edges = sorted(
        set(unknown_edges) | set(source_drift) | set(projection_drift)
    )
    unavailable_sources = sorted(set(unavailable_sources) | set(projection_unavailable))
    sql = build_capture_sql(
        inventory,
        primary_keys,
        arguments.protected_account_id,
        arguments.protected_character_guid,
    )
    sections = parse_capture_rows(_mysql(arguments, sql))

    derived: dict[str, list[Any]] = {
        key: []
        for key in (
            "careers",
            "economy_positions",
            "llm_actors",
            "medivh_observed_bots",
            "personalities",
            "playerbots",
            "redis_pending_entries",
            "redis_social_entries",
            "redis_telemetry_entries",
            "social_actors",
        )
    }
    surfaces: dict[str, list[Any]] = {
        edge["key"]: [] for edge in expand_edges(inventory)
    }
    protected_surfaces: dict[str, list[Any]] = {
        edge["key"]: []
        for edge in expand_edges(inventory)
        if not edge.get("capture_all")
    }
    for key, rows in sections.items():
        if key.startswith("derived."):
            derived[key.removeprefix("derived.")] = rows
        elif key.startswith("protected_surface."):
            protected_surfaces[key.removeprefix("protected_surface.")] = rows
        elif key.startswith("surface."):
            surfaces[key.removeprefix("surface.")] = rows

    target_accounts = {
        int(row["account_id"])
        for row in sections.get("ownership_rows", [])
        if int(row["account_type"]) in {1, 2}
    }
    target_identities = {
        str(row["character_guid"])
        for row in sections.get("character_rows", [])
        if int(row["account_id"]) in target_accounts
        or (
            row["delete_account_id"] is not None
            and int(row["delete_account_id"]) in target_accounts
        )
    }
    target_identities.update(
        str(row["public_id"]) for row in derived.get("social_actors", [])
    )
    target_identities.update(
        str(row["public_id"]) for row in derived.get("medivh_observed_bots", [])
    )
    protected_identities = {
        str(arguments.protected_character_guid),
    }
    for key in ("medivh.observed_bots", "playerbots.social_actor"):
        protected_identities.update(
            str(row["public_id"])
            for row in protected_surfaces.get(key, [])
            if row.get("public_id") is not None
        )
    cache_rows = surfaces.get("medivh.latest_cache", [])
    surfaces["medivh.latest_cache"] = [
        row
        for row in cache_rows
        if identity_in_text(str(row.get("value", "")), target_identities)
    ]
    protected_cache_rows = protected_surfaces.get("medivh.latest_cache", [])
    protected_surfaces["medivh.latest_cache"] = [
        row
        for row in protected_cache_rows
        if identity_in_text(str(row.get("value", "")), protected_identities)
    ]
    projection_keys = inventory["projection_keys"]
    redis_derived, redis_surfaces, protected_redis_surfaces = capture_redis(
        arguments,
        projection_keys,
        target_identities,
        protected_identities,
        _run,
    )
    derived.update(redis_derived)
    surfaces.update(redis_surfaces)
    protected_surfaces.update(protected_redis_surfaces)

    return {
        "account_prefix": prefix,
        "account_rows": sections.get("account_rows", []),
        "auction_rows": sections.get("auction_rows", []),
        "captured_at": datetime.now(timezone.utc).isoformat(),
        "character_rows": sections.get("character_rows", []),
        "derived_identities": derived,
        "diagnostics": {},
        "item_location_rows": sections.get("item_location_rows", []),
        "item_owner_rows": sections.get("item_owner_rows", []),
        "mail_rows": sections.get("mail_rows", []),
        "ownership_rows": sections.get("ownership_rows", []),
        "protected_identity": inventory["protected_identity"],
        "protected_surface_rows": protected_surfaces,
        "provenance": {
            "access": {
                "mysql": (
                    "SESSION TRANSACTION REPEATABLE READ, READ ONLY, "
                    "WITH CONSISTENT SNAPSHOT"
                ),
                "redis": ["GET", "XPENDING", "XRANGE", "XINFO GROUPS"],
            },
            "inventory_sha256": hashlib.sha256(
                Path(arguments.inventory).read_bytes()
            ).hexdigest(),
            "projections": projection_sources,
            "schemas": schema_hashes(schema_rows),
            "sources": sources,
        },
        "surface_rows": surfaces,
        "surface_policies": {
            edge["key"]: {
                "closure_required": edge.get("classification")
                != "known_unattributable_projection"
            }
            for edge in expand_edges(inventory)
        }
        | {
            f"redis.string.{key}": {"closure_required": False}
            for key in projection_keys["redis_strings"]
        }
        | {
            f"redis.groups.{stream}": {"closure_required": False}
            for stream in projection_keys["redis_streams"]
        },
        "unavailable_sources": unavailable_sources,
        "unknown_edges": unknown_edges,
    }


def capture_live_pair(
    arguments: argparse.Namespace,
) -> tuple[dict[str, Any], dict[str, Any]]:
    first = capture_once(arguments)
    if arguments.capture_delay_seconds > 0:
        time.sleep(arguments.capture_delay_seconds)
    second = capture_once(arguments)
    return first, second
