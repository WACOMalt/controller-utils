import gzip
import json
import math
import os
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class BehaviorReplayTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixture = json.loads(
            gzip.decompress((ROOT / "tests/fixtures/behavior.json.gz").read_bytes())
        )
        cls.cases = fixture["cases"]
        executable = os.environ.get("CONTROLLER_UTILS_REPLAY", str(ROOT / "build/native_replay"))
        result = subprocess.run(
            [executable],
            input="".join(json.dumps(case["request"]) + "\n" for case in cls.cases),
            text=True,
            capture_output=True,
            timeout=60,
            check=True,
        )
        cls.results = [json.loads(line) for line in result.stdout.splitlines()]
        if len(cls.results) != len(cls.cases):
            raise AssertionError("Native replay did not return one result per reference trace")

    def same(self, actual, expected, path):
        if isinstance(expected, dict):
            self.assertIsInstance(actual, dict, path)
            self.assertEqual(set(actual), set(expected), path)
            for key, value in expected.items():
                self.same(actual[key], value, path + "." + key)
        elif isinstance(expected, list):
            self.assertIsInstance(actual, list, path)
            self.assertEqual(len(actual), len(expected), path)
            for index, (a, b) in enumerate(zip(actual, expected)):
                self.same(a, b, path + "[" + str(index) + "]")
        elif isinstance(expected, bool):
            self.assertIs(type(actual), bool, path)
            self.assertEqual(actual, expected, path)
        elif isinstance(expected, float):
            self.assertIsInstance(actual, (int, float), path)
            self.assertTrue(
                math.isclose(actual, expected, rel_tol=2e-12, abs_tol=2e-10),
                f"{path}: {actual!r} != {expected!r}",
            )
        else:
            self.assertEqual(actual, expected, path)

    def test_behavior_matches_reference_traces(self):
        for case, actual in zip(self.cases, self.results):
            with self.subTest(trace=case["name"]):
                self.same(actual, case["expected"], case["name"])


if __name__ == "__main__":
    unittest.main()
