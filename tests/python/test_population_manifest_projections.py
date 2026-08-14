from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

MODULE_PATH = (
    Path(__file__).resolve().parents[2] / "tools" / "population_manifest_projections.py"
)
SPEC = importlib.util.spec_from_file_location(
    "population_manifest_projections", MODULE_PATH
)
assert SPEC and SPEC.loader
POPULATION_MANIFEST_PROJECTIONS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POPULATION_MANIFEST_PROJECTIONS)


class PopulationManifestProjectionsTest(unittest.TestCase):
    def test_identity_matching_requires_exact_token_boundaries(self) -> None:
        match = POPULATION_MANIFEST_PROJECTIONS.identity_in_text

        self.assertTrue(match('{"character_guid":661}', {"661"}))
        self.assertTrue(match('{"public_id":"actor_1"}', {"actor_1"}))
        self.assertFalse(match('{"character_guid":1661}', {"661"}))
        self.assertFalse(match('{"public_id":"actor_10"}', {"actor_1"}))

    def test_capture_uses_only_read_only_redis_commands(self) -> None:
        commands: list[list[str]] = []

        def runner(command: list[str]) -> str:
            commands.append(command)
            if "XINFO" in command:
                return '[["name","manifest"]]'
            return "[]" if "--json" in command else ""

        arguments = type(
            "Arguments",
            (),
            {"redis_host": "127.0.0.1", "redis_port": 6379},
        )()
        POPULATION_MANIFEST_PROJECTIONS.capture_redis(
            arguments,
            {
                "redis_streams": ["medivh:telemetry", "medivh:social"],
                "redis_strings": ["medivh:social:cursor"],
            },
            {"1001"},
            {"661"},
            runner,
        )

        operations = {
            next(
                token
                for token in command
                if token in {"GET", "XPENDING", "XRANGE", "XINFO"}
            )
            for command in commands
        }
        self.assertEqual(operations, {"GET", "XPENDING", "XRANGE", "XINFO"})

    def test_missing_redis_stream_has_empty_projection_groups(self) -> None:
        def runner(command: list[str]) -> str:
            if "XINFO" in command:
                return '{"error":"ERR no such key"}'
            return "[]" if "--json" in command else ""

        arguments = type(
            "Arguments",
            (),
            {"redis_host": "127.0.0.1", "redis_port": 6379},
        )()
        _, surfaces, protected_surfaces = POPULATION_MANIFEST_PROJECTIONS.capture_redis(
            arguments,
            {
                "redis_streams": ["medivh:social"],
                "redis_strings": [],
            },
            {"1001"},
            {"661"},
            runner,
        )

        self.assertEqual(surfaces["redis.groups.medivh:social"], [])
        self.assertEqual(protected_surfaces["redis.stream.medivh:social"], [])

    def test_isolated_redis_transport_uses_only_the_explicit_socket(self) -> None:
        commands: list[list[str]] = []

        def runner(command: list[str]) -> str:
            commands.append(command)
            return "[]"

        arguments = type(
            "Arguments",
            (),
            {
                "redis_host": "127.0.0.1",
                "redis_port": 6379,
                "redis_socket": "/isolated/redis.sock",
            },
        )()
        POPULATION_MANIFEST_PROJECTIONS.capture_redis(
            arguments,
            {"redis_streams": ["medivh:telemetry"], "redis_strings": []},
            set(),
            set(),
            runner,
        )

        self.assertTrue(commands)
        self.assertTrue(
            all(command[1:3] == ["-s", "/isolated/redis.sock"] for command in commands)
        )
        self.assertTrue(
            all("-h" not in command and "-p" not in command for command in commands)
        )


if __name__ == "__main__":
    unittest.main()
