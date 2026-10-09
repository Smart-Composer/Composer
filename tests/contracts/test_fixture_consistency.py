"""Check the portable contract corpus, not the native instrument implementation."""

import argparse
import copy
import json
import math
from pathlib import Path
import unittest

from jsonschema import Draft202012Validator
from referencing import Registry, Resource


ROOT = Path(__file__).resolve().parents[2]
MAX_REVISION = 9007199254740991


def strict_json(source):
    def object_members(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate object member")
            result[key] = value
        return result

    def invalid_constant(value):
        raise ValueError("nonfinite JSON constant: " + value)

    def check_finite(value):
        if isinstance(value, float) and not math.isfinite(value):
            raise ValueError("nonfinite JSON number")
        if isinstance(value, dict):
            for child in value.values():
                check_finite(child)
        elif isinstance(value, list):
            for child in value:
                check_finite(child)

    result = json.loads(
        source, object_pairs_hook=object_members, parse_constant=invalid_constant
    )
    check_finite(result)
    return result


def load(path):
    return strict_json(path.read_text(encoding="utf-8"))


def contract_json(source):
    value = strict_json(source)

    def check_integer_tokens(node):
        if isinstance(node, dict):
            for field, child in node.items():
                if field in {"schema_version", "expected_revision"} and type(child) is not int:
                    raise ValueError("version and revision require integer JSON tokens")
                check_integer_tokens(child)
        elif isinstance(node, list):
            for child in node:
                check_integer_tokens(child)

    check_integer_tokens(value)
    return value


def change_object(base, case):
    value = copy.deepcopy(base)
    value.update(copy.deepcopy(case.get("changes", {})))
    for field in case.get("remove", []):
        del value[field]
    return value


class FixtureConsistencyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture_dir = ROOT / "tests/contracts/fixtures"
        cls.patch_schema = load(ROOT / "docs/contracts/instrument-patch.schema.json")
        cls.command_schema = load(ROOT / "docs/contracts/project-command.schema.json")
        cls.patches = load(cls.fixture_dir / "patches.json")
        registry = Registry().with_resources(
            (schema["$id"], Resource.from_contents(schema))
            for schema in (cls.patch_schema, cls.command_schema)
        )
        cls.patch_validator = Draft202012Validator(cls.patch_schema, registry=registry)
        cls.command_validator = Draft202012Validator(cls.command_schema, registry=registry)

    def patch_value(self, value):
        if isinstance(value, str) and value in self.patches:
            return copy.deepcopy(self.patches[value])
        return copy.deepcopy(value)

    def command_value(self, value):
        result = copy.deepcopy(value)
        if "patch" in result:
            result["patch"] = self.patch_value(result["patch"])
        return result

    def assert_unique_ids(self, cases):
        ids = [case["id"] for case in cases]
        self.assertEqual(len(ids), len(set(ids)), "fixture IDs must be unique")

    def assert_state(self, state):
        self.assertEqual(set(state), {"project_instance_id", "patch", "revision", "undo", "redo"})
        self.assertIs(type(state["revision"]), int)
        self.assertIsInstance(state["undo"], list)
        self.assertIsInstance(state["redo"], list)
        for patch in [state["patch"], *state["undo"], *state["redo"]]:
            self.assertIn(patch, self.patches)
        self.command_validator.validate({
            "schema_version": 1,
            "type": "replace_instrument_patch",
            "project_instance_id": state["project_instance_id"],
            "expected_revision": state["revision"],
            "patch": self.patches[state["patch"]],
        })

    def test_schemas_are_valid_and_all_named_patches_conform(self):
        for schema in (self.patch_schema, self.command_schema):
            Draft202012Validator.check_schema(schema)
        for name, patch in self.patches.items():
            with self.subTest(patch=name):
                self.patch_validator.validate(patch)
        self.assertEqual(
            {patch["waveform"] for patch in self.patches.values()},
            set(self.patch_schema["properties"]["waveform"]["enum"]),
        )
        for field, definition in self.patch_schema["properties"].items():
            if "minimum" in definition:
                self.assertEqual(self.patches["lower_bounds"][field], definition["minimum"])
                self.assertEqual(self.patches["upper_bounds"][field], definition["maximum"])

    def test_patch_value_expectations_match_schema(self):
        cases = load(self.fixture_dir / "patch-validation.json")["cases"]
        self.assert_unique_ids(cases)
        for case in cases:
            with self.subTest(case=case["id"]):
                self.assertIn(case["expected"], {"valid", "invalid"})
                patch = change_object(self.patches[case["base"]], case)
                self.assertEqual(
                    self.patch_validator.is_valid(patch), case["expected"] == "valid"
                )

    def test_command_value_expectations_match_schema(self):
        fixture = load(self.fixture_dir / "command-validation.json")
        self.assert_unique_ids(fixture["cases"])
        for case in fixture["cases"]:
            with self.subTest(case=case["id"]):
                self.assertIn(case["expected"], {"valid", "invalid"})
                command = self.command_value(change_object(fixture["base"], case))
                self.assertEqual(
                    self.command_validator.is_valid(command), case["expected"] == "valid"
                )

    def test_raw_patch_expectations_match_strict_json_and_schema(self):
        cases = load(self.fixture_dir / "patch-parsing.json")["cases"]
        self.assert_unique_ids(cases)
        for case in cases:
            with self.subTest(case=case["id"]):
                self.assertIn(case["expected"], {"valid", "invalid"})
                try:
                    value = contract_json(case["source"])
                    valid = self.patch_validator.is_valid(value)
                except ValueError:
                    valid = False
                self.assertEqual(valid, case["expected"] == "valid")
                if valid:
                    self.assertEqual(value, self.patches[case["patch"]])

    def test_raw_command_expectations_match_strict_json_and_schema(self):
        cases = load(self.fixture_dir / "command-parsing.json")["cases"]
        self.assert_unique_ids(cases)
        for case in cases:
            with self.subTest(case=case["id"]):
                self.assertIn(case["expected"], {"valid", "invalid"})
                try:
                    value = contract_json(case["source"])
                    valid = self.command_validator.is_valid(value)
                except ValueError:
                    valid = False
                self.assertEqual(valid, case["expected"] == "valid")

    def test_history_expectations_are_internally_consistent(self):
        scenarios = load(self.fixture_dir / "command-scenarios.json")["scenarios"]
        self.assert_unique_ids(scenarios)
        for scenario in scenarios:
            state = copy.deepcopy(scenario["initial"])
            self.assert_state(state)
            for index, step in enumerate(scenario["steps"]):
                with self.subTest(scenario=scenario["id"], step=index):
                    self.assertIn(step["action"], {"apply", "undo", "redo"})
                    outcome = step["expected"]["result"]
                    self.assertIn(outcome, {
                        "applied", "invalid_command", "wrong_project", "stale_revision",
                        "no_change", "revision_exhausted",
                    })
                    after = {k: v for k, v in step["expected"].items() if k != "result"}
                    self.assert_state(after)

                    if step["action"] == "apply":
                        command = self.command_value(step["command"])
                        if not self.command_validator.is_valid(command):
                            expected_outcome = "invalid_command"
                        elif command["project_instance_id"] != state["project_instance_id"]:
                            expected_outcome = "wrong_project"
                        elif command["expected_revision"] != state["revision"]:
                            expected_outcome = "stale_revision"
                        elif command["patch"] == self.patch_value(state["patch"]):
                            expected_outcome = "no_change"
                        elif state["revision"] == MAX_REVISION:
                            expected_outcome = "revision_exhausted"
                        else:
                            expected_outcome = "applied"
                    elif not state[step["action"]]:
                        expected_outcome = "no_change"
                    elif state["revision"] == MAX_REVISION:
                        expected_outcome = "revision_exhausted"
                    else:
                        expected_outcome = "applied"
                    self.assertEqual(outcome, expected_outcome)

                    if outcome == "applied":
                        self.assertEqual(after["revision"], state["revision"] + 1)
                        self.assertEqual(after["project_instance_id"], state["project_instance_id"])
                        if step["action"] == "apply":
                            self.assertEqual(self.patch_value(after["patch"]), command["patch"])
                            self.assertNotEqual(self.patch_value(after["patch"]), self.patch_value(state["patch"]))
                            self.assertEqual(after["undo"], state["undo"] + [state["patch"]])
                            self.assertEqual(after["redo"], [])
                        elif step["action"] == "undo":
                            self.assertEqual(after["patch"], state["undo"][-1])
                            self.assertEqual(after["undo"], state["undo"][:-1])
                            self.assertEqual(after["redo"], state["redo"] + [state["patch"]])
                        elif step["action"] == "redo":
                            self.assertEqual(after["patch"], state["redo"][-1])
                            self.assertEqual(after["redo"], state["redo"][:-1])
                            self.assertEqual(after["undo"], state["undo"] + [state["patch"]])
                    else:
                        self.assertEqual(after, state, "rejection and no-op preserve all state")
                    state = after


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help="tree containing the schemas and fixtures")
    args, unittest_args = parser.parse_known_args()
    ROOT = args.root.resolve()
    unittest.main(argv=[__file__, *unittest_args])
