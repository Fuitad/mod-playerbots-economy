from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Callable

from population_cleanup_support import CleanupApplyFailure, CleanupRefusal

Handler = Callable[[argparse.Namespace], int]


def add_common_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--operation-record", required=True)
    parser.add_argument("--audit-root", required=True)
    parser.add_argument("--expected-frozen-digest", required=True)
    parser.add_argument("--output-dir", required=True)


def build_parser(
    plan_handler: Handler, apply_handler: Handler
) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Plan or apply an exact frozen Playerbot population cleanup."
    )
    commands = parser.add_subparsers(dest="command", required=True)
    plan = commands.add_parser("plan", help="Build a read only exact cleanup plan.")
    add_common_arguments(plan)
    plan.set_defaults(handler=plan_handler)
    apply = commands.add_parser("apply", help="Apply one unchanged exact cleanup plan.")
    add_common_arguments(apply)
    apply.add_argument("--plan", required=True)
    apply.set_defaults(handler=apply_handler)
    return parser


def run_cli(
    plan_handler: Handler,
    apply_handler: Handler,
    argv: list[str] | None = None,
) -> int:
    try:
        arguments = build_parser(plan_handler, apply_handler).parse_args(argv)
        return int(arguments.handler(arguments))
    except (
        CleanupRefusal,
        CleanupApplyFailure,
        OSError,
        ValueError,
        json.JSONDecodeError,
    ) as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 4
    except KeyboardInterrupt:
        print("REFUSED: interrupted", file=sys.stderr)
        return 130
